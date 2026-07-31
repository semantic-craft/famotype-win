#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <windows.h>

#include "famo_runtime_control.h"
#include "famo_runtime_service.h"
#include "famo_user_data_lock.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace famo::runtime {
struct RuntimeServiceTestAccess {
  static bool CopyInvalidResultWithoutError(RuntimeService &service) {
    FamoEngineActionResultV2 invalid{};
    Composition composition;
    return service.CopyResult(
        invalid, FAMO_ENGINE_ACTION_STATUS,
        FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES, &composition, nullptr);
  }
  static size_t RetiredContextCount(const RuntimeService &service) {
    return service.retired_contexts_.size();
  }
  static size_t SessionCount(const RuntimeService &service) {
    return service.sessions_.size();
  }
};
} // namespace famo::runtime

uint64_t g_connection_generation = 13;

class TestSink final : public RuntimeSnapshotSink {
public:
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    latest = std::move(snapshot);
  }
  bool
  PrepareStyle(std::string_view text, bool exists,
               std::shared_ptr<const void> *presentation) noexcept override {
    if (!presentation)
      return false;
    ++reloads;
    *presentation = exists ? std::static_pointer_cast<const void>(
                                 std::make_shared<const std::string>(text))
                           : nullptr;
    return true;
  }
  void ActivateStyle(
      std::shared_ptr<const RuntimeStyleState> style) noexcept override {
    active_style = std::move(style);
  }
  void PrepareForRuntimeReady() noexcept override { ++runtime_ready_count; }
  int reloads = 0;
  int runtime_ready_count = 0;
  std::shared_ptr<const RuntimeSnapshot> latest;
  std::shared_ptr<const RuntimeStyleState> active_style;
};

class BlockingUiSink final : public RuntimeSnapshotSink {
public:
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    (void)snapshot;
    std::unique_lock lock(mutex_);
    if (entered_value_)
      return;
    entered_value_ = true;
    entered_.notify_all();
    released_.wait(lock, [&] { return released_value_; });
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return entered_.wait_for(lock, timeout, [&] { return entered_value_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_value_ = true;
    }
    released_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable entered_;
  std::condition_variable released_;
  bool entered_value_ = false;
  bool released_value_ = false;
};

class BlockingReadySink final : public RuntimeSnapshotSink {
public:
  void Publish(
      std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override {
    (void)snapshot;
  }

  void PrepareForRuntimeReady() noexcept override {
    std::unique_lock lock(mutex_);
    entered_value_ = true;
    entered_.notify_all();
    released_.wait(lock, [&] { return released_value_; });
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return entered_.wait_for(lock, timeout, [&] { return entered_value_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_value_ = true;
    }
    released_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable entered_;
  std::condition_variable released_;
  bool entered_value_ = false;
  bool released_value_ = false;
};

class ScopedUserDataLockRoot {
public:
  explicit ScopedUserDataLockRoot(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~ScopedUserDataLockRoot() {
    (void)_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA", L"");
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

Frame Request(Command command, uint64_t sequence) {
  Frame frame;
  frame.command = command;
  frame.correlation = {11, 12, g_connection_generation, 14, 15, sequence};
  return frame;
}

std::string ReadFileText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

int main() {
  ScopedUserDataLockRoot lock_root(
      std::filesystem::temp_directory_path() /
      ("famo-runtime-lock-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64())));
  std::filesystem::create_directories(lock_root.path());
  CHECK(_wputenv_s(L"FAMO_TEST_USER_DATA_LOCK_LOCALAPPDATA",
                   lock_root.path().c_str()) == 0);
  constexpr uint32_t kRimeShiftLeft = 0xffe1;
  constexpr uint32_t kRimeShiftRight = 0xffe2;
  CHECK(IsShiftModeSwitch({kRimeShiftLeft, 0, 0, 0, 1}, 0,
                          FAMO_STATUS_ASCII_MODE));
  CHECK(IsShiftModeSwitch({kRimeShiftRight, 0, 0, 0, 1},
                          FAMO_STATUS_ASCII_MODE, 0));
  CHECK(!IsShiftModeSwitch({kRimeShiftLeft, 0, 0, 1, 1}, 0,
                           FAMO_STATUS_ASCII_MODE));
  CHECK(!IsShiftModeSwitch({'A', 0, 0, 0, 1}, 0,
                           FAMO_STATUS_ASCII_MODE));
  CHECK(!IsShiftModeSwitch({kRimeShiftLeft, 0, 0, 0, 1}, 0, 0));

  {
    UserDataTransactionLock transaction;
    std::string lock_error;
    if (!transaction.Acquire(&lock_error))
      std::fprintf(stderr, "user-data lock failed: %s\n",
                   lock_error.c_str());
    CHECK(transaction.held());
  }

  {
    RuntimeService allocation_start;
    TestSink allocation_sink;
    allocation_start.SetSnapshotSink(&allocation_sink);
    std::string allocation_error;
    CHECK(_wputenv_s(
              L"FAMO_TEST_RUNTIME_START_PARAMETER_ALLOCATION_FAILURE",
              L"1") == 0);
    CHECK(!allocation_start.Start(L"FamoTestEngine.dll", "",
                                  &allocation_error));
    CHECK(allocation_start.readiness() == RuntimeReadiness::Unavailable);
    CHECK(_wputenv_s(
              L"FAMO_TEST_RUNTIME_START_PARAMETER_ALLOCATION_FAILURE",
              L"") == 0);
    CHECK(allocation_start.Start(L"FamoTestEngine.dll", "",
                                 &allocation_error));
    CHECK(allocation_start.InitializeControlState() == ControlError::None);
    allocation_start.Stop();
  }

  // Deploy may only enter librime deployment after every old context is
  // physically gone. A transient destroy failure retains the exact pointer for
  // the next control attempt and makes the runtime unavailable in between.
  {
    RuntimeService destroy_retry;
    std::string destroy_error;
    CHECK(destroy_retry.Start(L"FamoTestEngine.dll", "", &destroy_error));
    CHECK(destroy_retry.InitializeControlState() == ControlError::None);
    Frame hello;
    hello.command = Command::Hello;
    hello.correlation = {700, 701, 702, 0, 0, 0};
    CHECK(destroy_retry.Dispatch(hello).status == Status::Ok);
    Frame open;
    open.command = Command::OpenSession;
    open.correlation = {700, 701, 702, 703, 704, 1};
    CHECK(EncodeOpenSession("test", &open.payload, &destroy_error));
    CHECK(destroy_retry.Dispatch(open).status == Status::Ok);
    const uint64_t before = destroy_retry.engine_generation();
    CHECK(_putenv_s("FAMO_TEST_DESTROY_RETRY_OBSERVED", "") == 0);
    CHECK(_putenv_s("FAMO_TEST_FAIL_DESTROY", "1") == 0);
    CHECK(destroy_retry.ExecuteControl(Command::ControlDeploy) ==
          ControlError::Engine);
    CHECK(destroy_retry.readiness() == RuntimeReadiness::Unavailable);
    CHECK(destroy_retry.engine_generation() == before);
    CHECK(RuntimeServiceTestAccess::SessionCount(destroy_retry) == 0);
    CHECK(RuntimeServiceTestAccess::RetiredContextCount(destroy_retry) == 1);
    CHECK(_putenv_s("FAMO_TEST_FAIL_DESTROY", "") == 0);
    CHECK(destroy_retry.ExecuteControl(Command::ControlDeploy) ==
          ControlError::None);
    CHECK(destroy_retry.readiness() == RuntimeReadiness::Ready);
    CHECK(RuntimeServiceTestAccess::RetiredContextCount(destroy_retry) == 0);
    char retried[2]{};
    CHECK(GetEnvironmentVariableA("FAMO_TEST_DESTROY_RETRY_OBSERVED", retried,
                                  static_cast<DWORD>(std::size(retried))) ==
          1);
    CHECK(retried[0] == '1');
    CHECK(_putenv_s("FAMO_TEST_DESTROY_RETRY_OBSERVED", "") == 0);
    destroy_retry.Stop();
  }

  const std::filesystem::path data_root =
      std::filesystem::temp_directory_path() /
      ("famo-runtime-service-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(data_root);
  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\n  emoji: true\n";
  std::ofstream(data_root / "famo-select-schema.txt") << "test\n";
  std::ofstream(data_root / "famo-style.yaml")
      << "style:\n"
         "  color_scheme: wuda\n"
         "  inline_preedit: true\n"
         "  show_preedit: true\n"
         "  preview_pages: true\n"
         "  preview_rows: 2\n"
         "  preedit_type: preview\n"
         "  famo_auto_pair: true\n"
         "  famo_cjk_english_spacing: true\n"
         "  famo_cjk_number_spacing: true\n";

  RuntimeService service;
  CHECK(!RuntimeServiceTestAccess::CopyInvalidResultWithoutError(service));
  TestSink sink;
  service.SetSnapshotSink(&sink);
  std::string error;
  const std::string data_root_utf8 = data_root.string();
  CHECK(service.Start(L"FamoTestEngine.dll", data_root_utf8.c_str(), &error));
  CHECK(service.readiness() == RuntimeReadiness::Starting);
  const ControlError initialized = service.InitializeControlState();
  if (initialized != ControlError::None)
    std::fprintf(stderr, "InitializeControlState failed: %u\n",
                 static_cast<unsigned>(initialized));
  CHECK(initialized == ControlError::None);
  CHECK(sink.runtime_ready_count == 1);
  CHECK(sink.reloads == 1);
  CHECK(sink.active_style);
  CHECK(sink.active_style->host_behavior_flags ==
        (kHostInlinePreedit | kHostCandidatePreview | kHostAutoPair |
         kHostCjkEnglishSpacing | kHostCjkNumberSpacing |
         kHostPreviewPages | kHostPreviewRowsTwo));
  const auto active_style_text = std::static_pointer_cast<const std::string>(
      sink.active_style->presentation);
  CHECK(active_style_text &&
        active_style_text->find("color_scheme: wuda") != std::string::npos);
  CHECK(service.engine_generation() == 2);

  CHECK(service.Start(L"FamoTestEngine.dll", data_root_utf8.c_str(), &error));
  CHECK(service.readiness() == RuntimeReadiness::Ready);
  CHECK(service.engine_generation() == 2);

  Frame previous_version_hello = Request(Command::Hello, 0);
  previous_version_hello.correlation.client_id = 10;
  previous_version_hello.correlation.session_id = 0;
  previous_version_hello.correlation.session_generation = 0;
  previous_version_hello.wire_version = kMinSupportedProtocolVersion;
  const Frame previous_version_hello_reply =
      service.Dispatch(previous_version_hello);
  CHECK(previous_version_hello_reply.status == Status::Ok);
  CHECK(previous_version_hello_reply.wire_version ==
        kMinSupportedProtocolVersion);
  Frame previous_version_open = Request(Command::OpenSession, 1);
  previous_version_open.correlation.client_id =
      previous_version_hello.correlation.client_id;
  previous_version_open.wire_version = kMinSupportedProtocolVersion;
  CHECK(EncodeOpenSession("legacy-v2", &previous_version_open.payload,
                          &error));
  const Frame previous_version_open_reply =
      service.Dispatch(previous_version_open);
  CHECK(previous_version_open_reply.status == Status::Ok);
  CHECK(previous_version_open_reply.wire_version ==
        kMinSupportedProtocolVersion);
  Frame previous_version_key = Request(Command::ProcessKey, 2);
  previous_version_key.correlation.client_id =
      previous_version_hello.correlation.client_id;
  previous_version_key.wire_version = kMinSupportedProtocolVersion;
  CHECK(EncodeKeyEvent(
      {static_cast<uint32_t>('N'), 0, 0, 1, 1},
      &previous_version_key.payload));
  const Frame previous_version_key_reply =
      service.Dispatch(previous_version_key);
  CHECK(previous_version_key_reply.status == Status::Ok);
  CHECK(previous_version_key_reply.wire_version ==
        kMinSupportedProtocolVersion);
  Composition previous_version_composition;
  CHECK(DecodeComposition(previous_version_key_reply.payload,
                          &previous_version_composition, &error));
  CHECK(previous_version_composition.handled &&
        previous_version_composition.preedit == "n");

  Frame hello = Request(Command::Hello, 0);
  hello.correlation.session_id = 0;
  hello.correlation.session_generation = 0;
  const HelloRequest hello_offer{
      kMinSupportedProtocolVersion, kProtocolVersion, 1};
  CHECK(EncodeHelloRequest(hello_offer, &hello.payload, &error));
  const Frame hello_reply = service.Dispatch(hello);
  CHECK(hello_reply.status == Status::Ok);
  HelloResponse hello_selection;
  CHECK(DecodeHelloResponse(hello_reply.payload, &hello_selection, &error));
  CHECK(hello_selection.selected_protocol_version == kProtocolVersion);
  CHECK(service.Dispatch(hello).status == Status::Ok);

  Frame open = Request(Command::OpenSession, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::Ok);
  CHECK(service.Dispatch(open).status == Status::Ok);

  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\noutside: true\n";
  CHECK(service.ExecuteControl(Command::ControlReloadOptions) ==
        ControlError::Config);
  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\n  emoji: true\n";
  _putenv_s("FAMO_TEST_FAIL_OPTION", "emoji");
  CHECK(service.ExecuteControl(Command::ControlReloadOptions) ==
        ControlError::Engine);
  _putenv_s("FAMO_TEST_FAIL_OPTION", "");
  Frame option_failure_key = Request(Command::ProcessKey, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('A'), 0, 0, 1, 1},
                       &option_failure_key.payload));
  Frame option_failure_reply = service.Dispatch(option_failure_key);
  CHECK(option_failure_reply.status == Status::Ok);
  Composition option_failure_composition;
  CHECK(DecodeComposition(option_failure_reply.payload,
                          &option_failure_composition, &error));
  const uint32_t all_host_flags = kHostInlinePreedit | kHostCandidatePreview |
                                  kHostAutoPair | kHostCjkEnglishSpacing |
                                  kHostCjkNumberSpacing | kHostPreviewPages |
                                  kHostPreviewRowsTwo;
  CHECK((option_failure_composition.state_flags & all_host_flags) ==
        all_host_flags);
  Frame clear_after_option_failure = Request(Command::ClearComposition, 3);
  CHECK(service.Dispatch(clear_after_option_failure).status == Status::Ok);
  _putenv_s("FAMO_TEST_SIMPLIFIED", "1");
  CHECK(service.ExecuteControl(Command::ControlReloadOptions) ==
        ControlError::None);
  CHECK(sink.latest &&
        (sink.latest->composition.status_flags & FAMO_STATUS_SIMPLIFIED) != 0);
  _putenv_s("FAMO_TEST_SIMPLIFIED", "");

  std::atomic<bool> runtime_running{true};
  RuntimeControlService control(&service, &runtime_running);
  CHECK(control.Start());
  Frame previous_version_control_hello;
  previous_version_control_hello.command = Command::Hello;
  previous_version_control_hello.correlation = {100, 1, 1, 0, 0, 0};
  previous_version_control_hello.wire_version =
      kMinSupportedProtocolVersion;
  const Frame previous_version_control_reply =
      control.Dispatch(previous_version_control_hello);
  CHECK(previous_version_control_reply.status == Status::Ok);
  CHECK(previous_version_control_reply.wire_version ==
        kMinSupportedProtocolVersion);

  Frame negotiated_control_hello;
  negotiated_control_hello.command = Command::Hello;
  negotiated_control_hello.correlation = {102, 1, 1, 0, 0, 0};
  CHECK(EncodeHelloRequest(hello_offer, &negotiated_control_hello.payload,
                           &error));
  const Frame control_hello_reply =
      control.Dispatch(negotiated_control_hello);
  CHECK(control_hello_reply.status == Status::Ok);
  CHECK(DecodeHelloResponse(control_hello_reply.payload, &hello_selection,
                            &error));
  CHECK(hello_selection.selected_protocol_version == kProtocolVersion);

  Frame control_hello;
  control_hello.command = Command::Hello;
  control_hello.correlation = {101, 1, 1, 0, 0, 0};
  CHECK(control.Dispatch(control_hello).status == Status::Ok);
  Frame invalid_control = control_hello;
  invalid_control.command = Command::ProcessKey;
  invalid_control.correlation.sequence = 1;
  CHECK(control.Dispatch(invalid_control).status == Status::InvalidFrame);
  CHECK(control.Dispatch(control_hello).status == Status::Ok);
  CHECK(control.Dispatch(invalid_control).status == Status::StaleRequest);

  _putenv_s("FAMO_TEST_DEPLOY_DELAY_MS", "200");
  Frame deploy = control_hello;
  deploy.command = Command::ControlDeploy;
  deploy.correlation.sequence = 2;
  Frame deploy_reply = control.Dispatch(deploy);
  CHECK(deploy_reply.status == Status::Ok);
  ControlResult deploy_result;
  CHECK(DecodeControlResult(deploy_reply.payload, &deploy_result, &error));
  CHECK(deploy_result.state == ControlState::Pending);

  const auto maintenance_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (service.readiness() != RuntimeReadiness::Maintenance &&
         std::chrono::steady_clock::now() < maintenance_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(service.readiness() == RuntimeReadiness::Maintenance);
  const auto fail_open_started = std::chrono::steady_clock::now();
  Frame maintenance_key = Request(Command::ProcessKey, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 1},
                       &maintenance_key.payload));
  CHECK(service.Dispatch(maintenance_key).status == Status::Unavailable);
  CHECK(std::chrono::steady_clock::now() - fail_open_started <
        std::chrono::milliseconds(20));

  Frame maintenance_status = control_hello;
  maintenance_status.command = Command::ControlStatus;
  maintenance_status.correlation.sequence = 3;
  CHECK(EncodeControlOperationId(deploy_result.operation_id,
                                 &maintenance_status.payload));
  Frame maintenance_status_reply = control.Dispatch(maintenance_status);
  CHECK(maintenance_status_reply.status == Status::Ok);
  CHECK(DecodeControlResult(maintenance_status_reply.payload, &deploy_result,
                            &error));
  CHECK(deploy_result.state == ControlState::Running);
  CHECK(deploy_result.readiness == RuntimeReadiness::Maintenance);

  uint64_t control_sequence = 4;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(deploy_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &deploy_result, &error));
  } while (deploy_result.state == ControlState::Pending ||
           deploy_result.state == ControlState::Running);
  CHECK(deploy_result.state == ControlState::Succeeded);
  CHECK(deploy_result.readiness == RuntimeReadiness::Ready);
  CHECK(deploy_result.engine_generation == 3);

  _putenv_s("FAMO_TEST_DEPLOY_FAIL", "1");
  Frame failed_deploy = control_hello;
  failed_deploy.command = Command::ControlDeploy;
  failed_deploy.correlation.sequence = control_sequence++;
  Frame failed_deploy_reply = control.Dispatch(failed_deploy);
  CHECK(
      DecodeControlResult(failed_deploy_reply.payload, &deploy_result, &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(deploy_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &deploy_result, &error));
  } while (deploy_result.state == ControlState::Pending ||
           deploy_result.state == ControlState::Running);
  CHECK(deploy_result.state == ControlState::Failed);
  CHECK(deploy_result.error == ControlError::Engine);
  CHECK(deploy_result.retryable);
  CHECK(deploy_result.readiness == RuntimeReadiness::Unavailable);
  CHECK(deploy_result.engine_generation == 3);

  _putenv_s("FAMO_TEST_DEPLOY_FAIL", "");
  Frame retry_deploy = control_hello;
  retry_deploy.command = Command::ControlDeploy;
  retry_deploy.correlation.sequence = control_sequence++;
  Frame retry_deploy_reply = control.Dispatch(retry_deploy);
  CHECK(
      DecodeControlResult(retry_deploy_reply.payload, &deploy_result, &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(deploy_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &deploy_result, &error));
  } while (deploy_result.state == ControlState::Pending ||
           deploy_result.state == ControlState::Running);
  CHECK(deploy_result.state == ControlState::Succeeded);
  CHECK(deploy_result.readiness == RuntimeReadiness::Ready);
  CHECK(deploy_result.engine_generation == 4);

  Frame stale_status = control_hello;
  stale_status.command = Command::ControlStatus;
  stale_status.correlation.sequence = control_sequence++;
  CHECK(EncodeControlOperationId(999999, &stale_status.payload));
  CHECK(control.Dispatch(stale_status).status == Status::StaleRequest);

  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: maybe\n";
  Frame invalid_options = control_hello;
  invalid_options.command = Command::ControlReloadOptions;
  invalid_options.correlation.sequence = control_sequence++;
  Frame invalid_options_reply = control.Dispatch(invalid_options);
  ControlResult options_result;
  CHECK(DecodeControlResult(invalid_options_reply.payload, &options_result,
                            &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(options_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &options_result, &error));
  } while (options_result.state == ControlState::Pending ||
           options_result.state == ControlState::Running);
  CHECK(options_result.state == ControlState::Failed);
  CHECK(options_result.error == ControlError::Config);
  CHECK(options_result.retryable);

  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: true\n";
  Frame retry_options = control_hello;
  retry_options.command = Command::ControlReloadOptions;
  retry_options.correlation.sequence = control_sequence++;
  Frame retry_options_reply = control.Dispatch(retry_options);
  CHECK(DecodeControlResult(retry_options_reply.payload, &options_result,
                            &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(options_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &options_result, &error));
  } while (options_result.state == ControlState::Pending ||
           options_result.state == ControlState::Running);
  CHECK(options_result.state == ControlState::Succeeded);
  CHECK(!options_result.retryable);

  _putenv_s("FAMO_TEST_CONTROL_THROW", "1");
  Frame throwing_options = control_hello;
  throwing_options.command = Command::ControlReloadOptions;
  throwing_options.correlation.sequence = control_sequence++;
  Frame throwing_reply = control.Dispatch(throwing_options);
  CHECK(DecodeControlResult(throwing_reply.payload, &options_result,
                            &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(options_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &options_result, &error));
  } while (options_result.state == ControlState::Pending ||
           options_result.state == ControlState::Running);
  CHECK(options_result.state == ControlState::Failed);
  CHECK(options_result.error == ControlError::Runtime);
  CHECK(options_result.readiness == RuntimeReadiness::Unavailable);
  _putenv_s("FAMO_TEST_CONTROL_THROW", "");

  Frame worker_recovery = control_hello;
  worker_recovery.command = Command::ControlReloadOptions;
  worker_recovery.correlation.sequence = control_sequence++;
  Frame worker_recovery_reply = control.Dispatch(worker_recovery);
  CHECK(DecodeControlResult(worker_recovery_reply.payload, &options_result,
                            &error));
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Frame status = control_hello;
    status.command = Command::ControlStatus;
    status.correlation.sequence = control_sequence++;
    CHECK(
        EncodeControlOperationId(options_result.operation_id, &status.payload));
    Frame status_reply = control.Dispatch(status);
    CHECK(status_reply.status == Status::Ok);
    CHECK(DecodeControlResult(status_reply.payload, &options_result, &error));
  } while (options_result.state == ControlState::Pending ||
           options_result.state == ControlState::Running);
  CHECK(options_result.state == ControlState::Succeeded);
  CHECK(options_result.readiness == RuntimeReadiness::Ready);

  _putenv_s("FAMO_TEST_DEPLOY_DELAY_MS", "");
  control.Stop();

  std::filesystem::create_directories(data_root / "rime_ice.userdb");
  std::filesystem::create_directories(data_root / "wubi86_jidian.userdb");
  std::ofstream(data_root / "rime_ice.userdb" / "CURRENT") << "rime-marker";
  std::ofstream(data_root / "wubi86_jidian.userdb" / "CURRENT") << "wubi-marker";
  std::ofstream(data_root / "quick-phrases.json") << "keep-me";
  const uint64_t generation_before_reset = service.engine_generation();
  const ControlError first_reset =
      service.ExecuteControl(Command::ControlResetUserDictionary);
  CHECK(first_reset == ControlError::None);
  CHECK(service.readiness() == RuntimeReadiness::Ready);
  CHECK(service.engine_generation() == generation_before_reset + 1);
  CHECK(!std::filesystem::exists(data_root / "rime_ice.userdb"));
  CHECK(!std::filesystem::exists(data_root / "wubi86_jidian.userdb"));
  CHECK(std::filesystem::exists(data_root / "quick-phrases.json"));
  std::vector<std::filesystem::path> reset_backups;
  for (const auto &entry :
       std::filesystem::directory_iterator(data_root / ".famo-backup")) {
    if (entry.is_directory() &&
        entry.path().filename().wstring().starts_with(L"userdb-reset-"))
      reset_backups.push_back(entry.path());
  }
  CHECK(reset_backups.size() == 1);
  CHECK(std::filesystem::exists(reset_backups[0] / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(reset_backups[0] / "wubi86_jidian.userdb" / "CURRENT"));
  CHECK(service.ExecuteControl(Command::ControlResetUserDictionary) ==
        ControlError::None);
  CHECK(service.engine_generation() == generation_before_reset + 1);

  std::filesystem::create_directories(data_root / "rime_ice.userdb");
  std::filesystem::create_directories(data_root / "wubi86_jidian.userdb");
  std::ofstream(data_root / "rime_ice.userdb" / "CURRENT") << "rime-marker";
  std::ofstream(data_root / "wubi86_jidian.userdb" / "CURRENT") << "wubi-marker";
  _putenv_s("FAMO_TEST_USERDB_ENUMERATION_DENIED", "1");
  CHECK(service.ExecuteControl(Command::ControlResetUserDictionary) ==
        ControlError::UserDictionaryEnumeration);
  CHECK(std::filesystem::exists(data_root / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(data_root / "wubi86_jidian.userdb" / "CURRENT"));
  _putenv_s("FAMO_TEST_USERDB_ENUMERATION_DENIED", "");

  const std::filesystem::path outside_userdb =
      std::filesystem::temp_directory_path() /
      ("famo-runtime-userdb-outside-" +
       std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(outside_userdb);
  std::ofstream(outside_userdb / "CURRENT") << "outside-marker";
  const std::filesystem::path linked_userdb =
      data_root / "linked.userdb";
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(
      outside_userdb, linked_userdb, symlink_error);
  if (!symlink_error) {
    CHECK(service.ExecuteControl(Command::ControlResetUserDictionary) ==
          ControlError::UserDictionaryEnumeration);
    CHECK(std::filesystem::exists(outside_userdb / "CURRENT"));
    std::error_code remove_link_error;
    CHECK(std::filesystem::remove(linked_userdb, remove_link_error));
    CHECK(!remove_link_error);
  }
  std::filesystem::remove_all(outside_userdb);

  const std::filesystem::path collision_backup =
      data_root / ".famo-backup" / "userdb-reset-collision";
  std::filesystem::create_directories(collision_backup);
  std::ofstream(collision_backup / "sentinel") << "older-backup";
  _putenv_s("FAMO_TEST_USERDB_BACKUP_NAME", "userdb-reset-collision");
  CHECK(service.ExecuteControl(Command::ControlResetUserDictionary) ==
        ControlError::Runtime);
  CHECK(std::filesystem::exists(collision_backup / "sentinel"));
  CHECK(std::filesystem::exists(data_root / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(data_root /
                                "wubi86_jidian.userdb" / "CURRENT"));
  _putenv_s("FAMO_TEST_USERDB_BACKUP_NAME", "");
  std::filesystem::remove_all(collision_backup);

  _putenv_s("FAMO_TEST_USERDB_PARTIAL_DELETE_FAILURE", "1");
  const ControlError partial_reset =
      service.ExecuteControl(Command::ControlResetUserDictionary);
  if (partial_reset != ControlError::Runtime)
    std::fprintf(stderr, "partial userdb reset failed: %u readiness=%u\n",
                 static_cast<unsigned>(partial_reset),
                 static_cast<unsigned>(service.readiness()));
  CHECK(partial_reset == ControlError::Runtime);
  CHECK(std::filesystem::exists(data_root / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(data_root / "wubi86_jidian.userdb" / "CURRENT"));

  _putenv_s("FAMO_TEST_USERDB_RESTORE_FAILURE", "1");
  CHECK(service.ExecuteControl(Command::ControlResetUserDictionary) ==
        ControlError::UserDictionaryRollback);
  CHECK(service.readiness() == RuntimeReadiness::Unavailable);
  CHECK(!std::filesystem::exists(data_root / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(data_root / "wubi86_jidian.userdb" / "CURRENT"));
  _putenv_s("FAMO_TEST_USERDB_PARTIAL_DELETE_FAILURE", "");
  _putenv_s("FAMO_TEST_USERDB_RESTORE_FAILURE", "");
  CHECK(service.ExecuteControl(Command::ControlDeploy) ==
        ControlError::None);
  CHECK(service.readiness() == RuntimeReadiness::Ready);
  CHECK(std::filesystem::exists(data_root / "rime_ice.userdb" / "CURRENT"));
  CHECK(std::filesystem::exists(data_root /
                                "wubi86_jidian.userdb" / "CURRENT"));

  Frame invalidated_key = Request(Command::ProcessKey, 4);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 1},
                       &invalidated_key.payload));
  CHECK(service.Dispatch(invalidated_key).status == Status::StaleRequest);
  g_connection_generation = 14;
  Frame reconnected_hello = Request(Command::Hello, 0);
  reconnected_hello.correlation.session_id = 0;
  reconnected_hello.correlation.session_generation = 0;
  CHECK(service.Dispatch(reconnected_hello).status == Status::Ok);
  Frame reconnected_open = Request(Command::OpenSession, 1);
  CHECK(EncodeOpenSession("test", &reconnected_open.payload, &error));
  CHECK(service.Dispatch(reconnected_open).status == Status::Ok);

  _putenv_s("FAMO_TEST_MULTIPAGE", "1");
  Frame process_n = Request(Command::ProcessKey, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &process_n.payload));
  Frame reply = service.Dispatch(process_n);
  CHECK(reply.status == Status::Ok);
  Composition composition;
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");
  CHECK(service.Dispatch(process_n).status == Status::StaleRequest);

  Frame process_i = Request(Command::ProcessKey, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &process_i.payload));
  reply = service.Dispatch(process_i);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "ni");
  CHECK(!composition.candidates.empty());
  CHECK(composition.candidates[0].text == "\xe4\xbd\xa0");
  CHECK(composition.preview_candidates.empty());
  CHECK(sink.latest && sink.latest->composition.preview_candidates.size() == 2);
  CHECK(sink.latest->composition.preview_candidates[0].text == "\xe5\xb0\xbc");

  // A PEEK result is bounded by this request's count, not merely by the
  // runtime-wide candidate cap. An over-return is ignored instead of leaking
  // extra candidates into the UI snapshot.
  _putenv_s("FAMO_TEST_PEEK_OVERRUN", "1");
  Frame highlight = Request(Command::HighlightCandidate, 4);
  CHECK(EncodeCandidateIndex(1, &highlight.payload));
  reply = service.Dispatch(highlight);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.preedit == "ni");
  CHECK(sink.latest && sink.latest->composition.preview_candidates.empty());
  _putenv_s("FAMO_TEST_PEEK_OVERRUN", "");
  _putenv_s("FAMO_TEST_MULTIPAGE", "");

  Frame page = Request(Command::ChangePage, 5);
  CHECK(EncodePageDirection(false, &page.payload));
  reply = service.Dispatch(page);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.preedit == "ni");

  Frame clear = Request(Command::ClearComposition, 6);
  reply = service.Dispatch(clear);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.preedit.empty());

  Frame process_n_again = Request(Command::ProcessKey, 7);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 3},
                       &process_n_again.payload));
  CHECK(service.Dispatch(process_n_again).status == Status::Ok);

  Frame commit = Request(Command::CommitComposition, 8);
  _putenv_s("FAMO_TEST_FORMAT_COMMIT", "1");
  reply = service.Dispatch(commit);
  _putenv_s("FAMO_TEST_FORMAT_COMMIT", "");
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.preedit.empty());
  CHECK(composition.handled &&
        composition.commit == "\xe3\x80\x8c" "n" "\xe3\x80\x8d");  // 「n」

  Frame after_commit = Request(Command::ProcessKey, 9);
  CHECK(EncodeKeyEvent({0x7b, 0, 0, 1, 3}, &after_commit.payload));
  reply = service.Dispatch(after_commit);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(!composition.handled && composition.commit.empty());

  Frame update_ui = Request(Command::UpdateUiState, 100);
  CHECK(EncodeUiState({{1, 2, 3, 4}, {0, 0, 1920, 1080}, 96, true, true, true,
                       {1, 100}},
                      &update_ui.payload, &error));
  CHECK(service.Dispatch(update_ui).status == Status::Ok);
  CHECK(sink.latest && sink.latest->ui_sequence == 100);
  CHECK(sink.latest->ui_state.caret.left == 1);

  BlockingUiSink blocking_ui_sink;
  service.SetSnapshotSink(&blocking_ui_sink);
  Frame blocking_ui = Request(Command::UpdateUiState, 101);
  CHECK(EncodeUiState({{5, 6, 7, 8}, {0, 0, 1920, 1080}, 96, true, true,
                       true, {2, 101}},
                      &blocking_ui.payload, &error));
  Frame process_n_final = Request(Command::ProcessKey, 10);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 4},
                       &process_n_final.payload));
  std::atomic<Status> blocking_ui_status{Status::Unavailable};
  std::thread ui_thread([&] {
    blocking_ui_status.store(service.Dispatch(blocking_ui).status);
  });
  const bool ui_publish_entered =
      blocking_ui_sink.WaitUntilEntered(std::chrono::milliseconds(250));
  const Frame process_while_ui_blocked = service.Dispatch(process_n_final);
  blocking_ui_sink.Release();
  ui_thread.join();
  service.SetSnapshotSink(&sink);
  CHECK(ui_publish_entered);
  CHECK(blocking_ui_status.load() == Status::Ok);
  CHECK(process_while_ui_blocked.status == Status::Ok);

  Frame process_ni = Request(Command::ProcessKey, 11);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 4},
                       &process_ni.payload));
  CHECK(service.Dispatch(process_ni).status == Status::Ok);

  Frame unhandled_select = Request(Command::SelectCandidate, 12);
  CHECK(EncodeCandidateIndex(0, &unhandled_select.payload));
  _putenv_s("FAMO_TEST_UNHANDLED_SELECTION", "1");
  reply = service.Dispatch(unhandled_select);
  _putenv_s("FAMO_TEST_UNHANDLED_SELECTION", "");
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(!composition.handled && composition.commit.empty());
  CHECK(composition.preedit == "ni");

  Frame select = Request(Command::SelectCandidate, 13);
  CHECK(EncodeCandidateIndex(0, &select.payload));
  _putenv_s("FAMO_TEST_DEFER_SELECTION_COMMIT", "1");
  reply = service.Dispatch(select);
  _putenv_s("FAMO_TEST_DEFER_SELECTION_COMMIT", "");
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(composition.handled && composition.commit == "\xe4\xbd\xa0");

  Frame stale_ui = Request(Command::UpdateUiState, 99);
  CHECK(EncodeUiState({{100, 200, 300, 400}, {0, 0, 1920, 1080}, 96, true,
                       true, true, {3, 99}},
                      &stale_ui.payload, &error));
  CHECK(service.Dispatch(stale_ui).status == Status::StaleRequest);
  CHECK(sink.latest && sink.latest->ui_sequence == 101);
  CHECK(sink.latest->ui_state.caret.left == 5);

  Frame stale_activation = Request(Command::ProcessKey, 14);
  --stale_activation.correlation.activation_generation;
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 3},
                       &stale_activation.payload));
  reply = service.Dispatch(stale_activation);
  CHECK(reply.status == Status::StaleRequest && reply.payload.empty());

  Frame stale_session = Request(Command::ProcessKey, 14);
  --stale_session.correlation.session_generation;
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 3},
                       &stale_session.payload));
  reply = service.Dispatch(stale_session);
  CHECK(reply.status == Status::StaleRequest && reply.payload.empty());

  Frame stale_connection = Request(Command::ProcessKey, 14);
  --stale_connection.correlation.connection_generation;
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 3},
                       &stale_connection.payload));
  reply = service.Dispatch(stale_connection);
  CHECK(reply.status == Status::StaleRequest && reply.payload.empty());

  Frame unhandled = Request(Command::ProcessKey, 14);
  CHECK(EncodeKeyEvent({0x7b, 0, 0, 1, 4}, &unhandled.payload));
  reply = service.Dispatch(unhandled);
  CHECK(reply.status == Status::Ok);
  CHECK(DecodeComposition(reply.payload, &composition, &error));
  CHECK(!composition.handled && composition.commit.empty());

  Frame close = Request(Command::CloseSession, 15);
  CHECK(service.Dispatch(close).status == Status::Ok);
  CHECK(service.Dispatch(close).status == Status::StaleRequest);

  std::atomic<bool> schema_finished{false};
  ControlError locked_schema_result = ControlError::Runtime;
  std::thread locked_schema_worker;
  bool schema_waited_for_lock = false;
  {
    UserDataTransactionLock held;
    CHECK(held.Acquire());
    locked_schema_worker = std::thread([&] {
      locked_schema_result = service.SelectSchemaAndPersist("next");
      schema_finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    schema_waited_for_lock = !schema_finished.load();
  }
  locked_schema_worker.join();
  CHECK(schema_waited_for_lock);
  CHECK(locked_schema_result == ControlError::None);
  CHECK(ReadFileText(data_root / "famo-select-schema.txt") == "next");
  CHECK(!std::filesystem::exists(
      data_root / "famo-select-schema.txt.famo-write.tmp"));
  CHECK(service.SelectSchemaAndPersist("test") == ControlError::None);

  _putenv_s("FAMO_TEST_FAIL_SCHEMA", "missing");
  CHECK(service.SelectSchemaAndPersist("missing") == ControlError::Engine);
  CHECK(service.readiness() == RuntimeReadiness::Ready);
  CHECK(ReadFileText(data_root / "famo-select-schema.txt") == "test");
  _putenv_s("FAMO_TEST_SCHEMA_ROLLBACK_FAILURE", "1");
  CHECK(service.SelectSchemaAndPersist("missing") == ControlError::Runtime);
  CHECK(service.readiness() == RuntimeReadiness::Unavailable);
  CHECK(ReadFileText(data_root / "famo-select-schema.txt") == "missing");
  _putenv_s("FAMO_TEST_SCHEMA_ROLLBACK_FAILURE", "");
  _putenv_s("FAMO_TEST_FAIL_SCHEMA", "");
  std::ofstream(data_root / "famo-select-schema.txt", std::ios::binary)
      << "test";
  CHECK(service.ExecuteControl(Command::ControlDeploy) ==
        ControlError::None);
  CHECK(service.readiness() == RuntimeReadiness::Ready);

  std::ofstream(data_root / "famo-select-schema.txt") << "missing\n";
  _putenv_s("FAMO_TEST_FAIL_SCHEMA", "missing");
  CHECK(service.ExecuteControl(Command::ControlSelectSchema) ==
        ControlError::Engine);
  _putenv_s("FAMO_TEST_FAIL_SCHEMA", "");
  std::ofstream(data_root / "famo-select-schema.txt") << "test\n";
  CHECK(service.ExecuteControl(Command::ControlSelectSchema) ==
        ControlError::None);

  Frame newer = hello;
  newer.correlation.connection_generation = 15;
  CHECK(service.Dispatch(newer).status == Status::Ok);
  CHECK(service.Dispatch(hello).status == Status::StaleRequest);
  CHECK(service.Dispatch(open).status == Status::StaleRequest);

  service.Stop();
  const int ready_before_recovery = sink.runtime_ready_count;

  RuntimeService gated_readiness;
  BlockingReadySink gated_sink;
  gated_readiness.SetSnapshotSink(&gated_sink);
  CHECK(gated_readiness.Start(L"FamoTestEngine.dll", "", &error));
  ControlError gated_result = ControlError::Runtime;
  std::thread gated_worker([&] {
    gated_result = gated_readiness.InitializeControlState();
  });
  const bool ready_callback_entered =
      gated_sink.WaitUntilEntered(std::chrono::seconds(2));
  const RuntimeReadiness readiness_during_callback = gated_readiness.readiness();
  Frame gated_hello;
  gated_hello.command = Command::Hello;
  gated_hello.correlation = {91, 92, 93, 0, 0, 0};
  const Status status_during_callback = gated_readiness.Dispatch(gated_hello).status;
  gated_sink.Release();
  gated_worker.join();
  CHECK(ready_callback_entered);
  CHECK(readiness_during_callback == RuntimeReadiness::Starting);
  CHECK(status_during_callback == Status::Unavailable);
  CHECK(gated_result == ControlError::None);
  CHECK(gated_readiness.readiness() == RuntimeReadiness::Ready);
  gated_readiness.Stop();

  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\nnot_options: true\n";
  RuntimeService startup_failure;
  startup_failure.SetSnapshotSink(&sink);
  CHECK(startup_failure.Start(L"FamoTestEngine.dll", data_root_utf8.c_str(),
                              &error));
  CHECK(startup_failure.readiness() == RuntimeReadiness::Starting);
  CHECK(startup_failure.InitializeControlState() == ControlError::Config);
  CHECK(startup_failure.readiness() == RuntimeReadiness::Unavailable);
  CHECK(sink.runtime_ready_count == ready_before_recovery);
  Frame unavailable_hello = Request(Command::Hello, 0);
  unavailable_hello.correlation.session_id = 0;
  unavailable_hello.correlation.session_generation = 0;
  CHECK(startup_failure.Dispatch(unavailable_hello).status ==
        Status::Unavailable);
  const uint64_t failed_start_generation = startup_failure.engine_generation();
  CHECK(startup_failure.ExecuteControl(Command::ControlDeploy) ==
        ControlError::Config);
  CHECK(startup_failure.readiness() == RuntimeReadiness::Unavailable);
  CHECK(sink.runtime_ready_count == ready_before_recovery);
  CHECK(startup_failure.engine_generation() == failed_start_generation);
  std::ofstream(data_root / "famo-options.yaml")
      << "options:\n  ascii_mode: false\n";
  CHECK(startup_failure.ExecuteControl(Command::ControlReloadOptions) ==
        ControlError::None);
  CHECK(startup_failure.readiness() == RuntimeReadiness::Ready);
  CHECK(sink.runtime_ready_count == ready_before_recovery + 1);
  startup_failure.Stop();
  std::filesystem::remove_all(data_root);
  std::printf("runtime_service_selfcheck: OK\n");
  return 0;
}
