#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "famo_pipe_security.h"
#include "famo_runtime_protocol.h"

namespace famo::runtime {

class RuntimeService;

class RuntimeControlService {
public:
  RuntimeControlService(RuntimeService *runtime, std::atomic<bool> *running);
  ~RuntimeControlService();
  RuntimeControlService(const RuntimeControlService &) = delete;
  RuntimeControlService &operator=(const RuntimeControlService &) = delete;

  bool Start();
  void Stop() noexcept;
  Frame Dispatch(const Frame &request,
                 const PipeClientIdentity &owner = {});
  void InvalidateConnection(uint64_t client_id,
                            uint64_t activation_generation,
                            uint64_t connection_generation,
                            const PipeClientIdentity &owner = {});

private:
  struct ClientState {
    uint64_t activation_generation = 0;
    uint64_t connection_generation = 0;
    uint64_t last_sequence = 0;
    PipeClientIdentity owner;
  };
  struct Operation {
    uint64_t id = 0;
    Command command = Command::ControlStatus;
  };

  Frame Reply(const Frame &request, Status status) const;
  Frame ResultReply(const Frame &request, const ControlResult &result) const;
  void WorkerMain() noexcept;

  RuntimeService *runtime_ = nullptr;
  std::atomic<bool> *running_ = nullptr;
  std::mutex mutex_;
  std::condition_variable available_;
  std::map<uint64_t, ClientState> clients_;
  std::map<uint64_t, ControlResult> results_;
  std::deque<Operation> queue_;
  std::thread worker_;
  uint64_t next_operation_id_ = 1;
  bool stop_ = false;
  static constexpr size_t kMaxClients = 64;
};

bool RunControlClient(const PipeEndpoint &endpoint,
                      std::wstring_view expected_server, Command command,
                      std::chrono::milliseconds timeout, ControlResult *result,
                      std::string *error);

bool ParseControlCommand(std::wstring_view value, Command *command);

} // namespace famo::runtime
