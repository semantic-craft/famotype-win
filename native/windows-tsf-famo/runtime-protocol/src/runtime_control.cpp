#include "famo_runtime_control.h"

#include <algorithm>

#include "famo_runtime_service.h"

namespace famo::runtime {

RuntimeControlService::RuntimeControlService(RuntimeService *runtime,
                                             std::atomic<bool> *running)
    : runtime_(runtime), running_(running) {}

RuntimeControlService::~RuntimeControlService() { Stop(); }

bool RuntimeControlService::Start() {
  Stop();
  if (!runtime_ || !running_)
    return false;
  {
    std::lock_guard lock(mutex_);
    stop_ = false;
  }
  try {
    worker_ = std::thread(&RuntimeControlService::WorkerMain, this);
  } catch (...) {
    return false;
  }
  return true;
}

void RuntimeControlService::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  available_.notify_all();
  if (worker_.joinable())
    worker_.join();
  std::lock_guard lock(mutex_);
  queue_.clear();
  clients_.clear();
  results_.clear();
}

Frame RuntimeControlService::Reply(const Frame &request, Status status) const {
  Frame reply;
  reply.command = request.command;
  reply.flags = kFlagResponse;
  reply.status = status;
  reply.correlation = request.correlation;
  return reply;
}

Frame RuntimeControlService::ResultReply(const Frame &request,
                                         const ControlResult &result) const {
  Frame reply = Reply(request, Status::Ok);
  std::string error;
  if (!EncodeControlResult(result, &reply.payload, &error))
    return Reply(request, Status::EngineError);
  return reply;
}

Frame RuntimeControlService::Dispatch(const Frame &request) {
  if (request.flags != 0 || request.status != Status::Ok)
    return Reply(request, Status::InvalidFrame);
  const Correlation &c = request.correlation;
  if (c.client_id == 0 || c.activation_generation == 0 ||
      c.connection_generation == 0 || c.session_id != 0 ||
      c.session_generation != 0)
    return Reply(request, Status::InvalidFrame);

  std::lock_guard lock(mutex_);
  if (request.command == Command::Hello) {
    if (!request.payload.empty() || c.sequence != 0)
      return Reply(request, Status::InvalidFrame);
    const auto found = clients_.find(c.client_id);
    if (found != clients_.end() &&
        (c.activation_generation < found->second.activation_generation ||
         (c.activation_generation == found->second.activation_generation &&
          c.connection_generation < found->second.connection_generation)))
      return Reply(request, Status::StaleRequest);
    if (found != clients_.end() &&
        c.activation_generation == found->second.activation_generation &&
        c.connection_generation == found->second.connection_generation)
      return Reply(request, Status::Ok);
    clients_[c.client_id] =
        ClientState{c.activation_generation, c.connection_generation, 0};
    return Reply(request, Status::Ok);
  }

  const auto client = clients_.find(c.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != c.activation_generation ||
      client->second.connection_generation != c.connection_generation ||
      c.sequence == 0 || c.sequence <= client->second.last_sequence)
    return Reply(request, Status::StaleRequest);
  client->second.last_sequence = c.sequence;

  if (request.command == Command::ControlStatus) {
    uint64_t operation_id = 0;
    std::string error;
    if (!DecodeControlOperationId(request.payload, &operation_id, &error))
      return Reply(request, Status::InvalidFrame);
    const auto found = results_.find(operation_id);
    if (found == results_.end())
      return Reply(request, Status::StaleRequest);
    ControlResult snapshot = found->second;
    if (snapshot.state == ControlState::Pending ||
        snapshot.state == ControlState::Running) {
      snapshot.readiness = runtime_->readiness();
      snapshot.engine_generation = runtime_->engine_generation();
    }
    return ResultReply(request, snapshot);
  }
  if (!IsControlOperation(request.command) || !request.payload.empty())
    return Reply(request, Status::InvalidFrame);

  const uint64_t operation_id = next_operation_id_++;
  ControlResult result{operation_id,
                       ControlState::Pending,
                       ControlError::None,
                       false,
                       runtime_->readiness(),
                       runtime_->engine_generation()};
  if (queue_.size() >= 64) {
    result.state = ControlState::Failed;
    result.error = ControlError::QueueFull;
    result.retryable = true;
  } else {
    queue_.push_back(Operation{operation_id, request.command});
    available_.notify_one();
  }
  while (results_.size() >= 256) {
    const auto terminal = std::find_if(results_.begin(), results_.end(),
                                       [](const auto &entry) {
      return entry.second.state == ControlState::Succeeded ||
             entry.second.state == ControlState::Failed;
    });
    if (terminal == results_.end())
      break;
    results_.erase(terminal);
  }
  results_[operation_id] = result;
  return ResultReply(request, result);
}

void RuntimeControlService::InvalidateConnection(
    uint64_t client_id, uint64_t activation_generation,
    uint64_t connection_generation) {
  std::lock_guard lock(mutex_);
  const auto found = clients_.find(client_id);
  if (found != clients_.end() &&
      found->second.activation_generation == activation_generation &&
      found->second.connection_generation == connection_generation)
    clients_.erase(found);
}

void RuntimeControlService::WorkerMain() {
  for (;;) {
    Operation operation;
    {
      std::unique_lock lock(mutex_);
      available_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      operation = queue_.front();
      queue_.pop_front();
      ControlResult &result = results_[operation.id];
      result.state = ControlState::Running;
      result.readiness = runtime_->readiness();
    }

    const ControlError control_error = runtime_->ExecuteControl(operation.command);
    {
      std::lock_guard lock(mutex_);
      ControlResult &result = results_[operation.id];
      result.state = control_error == ControlError::None
                         ? ControlState::Succeeded
                         : ControlState::Failed;
      result.error = control_error;
      result.retryable = control_error != ControlError::None &&
                         control_error != ControlError::InvalidOperation &&
                         control_error != ControlError::UserDictionaryRollback;
      result.readiness = runtime_->readiness();
      result.engine_generation = runtime_->engine_generation();
    }
    if (operation.command == Command::ControlShutdown &&
        control_error == ControlError::None)
      running_->store(false);
  }
}

bool ParseControlCommand(std::wstring_view value, Command *command) {
  if (!command)
    return false;
  if (value == L"reload-style") *command = Command::ControlReloadStyle;
  else if (value == L"reload-options") *command = Command::ControlReloadOptions;
  else if (value == L"select-schema") *command = Command::ControlSelectSchema;
  else if (value == L"deploy") *command = Command::ControlDeploy;
  else if (value == L"reset-user-dictionary") *command = Command::ControlResetUserDictionary;
  else if (value == L"shutdown") *command = Command::ControlShutdown;
  else return false;
  return true;
}

} // namespace famo::runtime
