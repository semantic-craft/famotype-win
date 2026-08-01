#include "tsf_integration_support.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "famo_runtime_pipe.h"

namespace famo::tsf::test {
namespace {

constexpr wchar_t kTestRuntimePreviewSourceClass[] =
    L"FamoTestRuntimePreviewSource";

} // namespace

const std::wstring &TestEndpointSuffix() {
  static const std::wstring suffix =
      L"dev-test-engine-" + std::to_wstring(GetCurrentProcessId());
  return suffix;
}

RuntimeProcess::~RuntimeProcess() { Stop(); }

bool RuntimeProcess::Start(const wchar_t *path, std::wstring_view fault,
                           uint32_t fault_after, uint32_t connections,
                           bool inline_preedit, uint32_t preview_rows,
                           int32_t expected_terminal_abandons,
                           int32_t expected_clients,
                           int32_t expected_sessions) {
  std::wstring command =
      L"\"" + std::wstring(path) + L"\" --endpoint-suffix " +
      TestEndpointSuffix() + L" --fault " + std::wstring(fault) +
      L" --fault-after " + std::to_wstring(fault_after) +
      L" --connections " + std::to_wstring(connections) +
      L" --inline-preedit " + (inline_preedit ? L"true" : L"false") +
      L" --preview-rows " + std::to_wstring(preview_rows) +
      L" --expected-terminal-abandons " +
      std::to_wstring(expected_terminal_abandons) +
      L" --expected-clients " + std::to_wstring(expected_clients) +
      L" --expected-sessions " + std::to_wstring(expected_sessions);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_)) {
    return false;
  }

  famo::runtime::PipeEndpoint endpoint;
  std::string error;
  if (!famo::runtime::BuildCurrentPipeEndpoint(TestEndpointSuffix(), &endpoint,
                                               &error)) {
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (WaitNamedPipeW(endpoint.name.c_str(), 20))
      return true;
    if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0)
      return false;
    Sleep(10);
  }
  return false;
}

bool RuntimeProcess::Finish() {
  if (!process_.hProcess)
    return false;
  CloseHandle(process_.hThread);
  process_.hThread = nullptr;
  const DWORD wait = WaitForSingleObject(process_.hProcess, 3000);
  DWORD exit_code = STILL_ACTIVE;
  GetExitCodeProcess(process_.hProcess, &exit_code);
  CloseHandle(process_.hProcess);
  process_.hProcess = nullptr;
  const bool finished = wait == WAIT_OBJECT_0 && exit_code == 0;
  if (!finished) {
    std::fprintf(stderr, "runtime finish failed: wait=%lu exit=%lu\n",
                 static_cast<unsigned long>(wait),
                 static_cast<unsigned long>(exit_code));
  }
  return finished;
}

HWND RuntimeProcess::PreviewSourceWindow() const {
  if (!process_.dwProcessId)
    return nullptr;
  const std::wstring title = std::to_wstring(process_.dwProcessId);
  HWND after = nullptr;
  while ((after = FindWindowExW(HWND_MESSAGE, after,
                                kTestRuntimePreviewSourceClass,
                                title.c_str()))) {
    DWORD process_id = 0;
    if (GetWindowThreadProcessId(after, &process_id) != 0 &&
        process_id == process_.dwProcessId) {
      return after;
    }
  }
  return nullptr;
}

void RuntimeProcess::Stop() {
  if (process_.hThread) {
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;
  }
  if (process_.hProcess) {
    if (WaitForSingleObject(process_.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(process_.hProcess, 9);
      WaitForSingleObject(process_.hProcess, 1000);
    }
    CloseHandle(process_.hProcess);
    process_.hProcess = nullptr;
  }
}

TextServiceModule::~TextServiceModule() {
  if (module_)
    FreeLibrary(module_);
}

bool TextServiceModule::Load(const wchar_t *path) {
  module_ = LoadLibraryW(path);
  if (!module_)
    return false;
  can_unload_ = reinterpret_cast<CanUnloadFn>(
      GetProcAddress(module_, "DllCanUnloadNow"));
  create_for_test_ = reinterpret_cast<CreateForTestFn>(
      GetProcAddress(module_, "FamoCreateTextServiceForTest"));
  reactivate_for_test_ = reinterpret_cast<ReactivateForTestFn>(
      GetProcAddress(module_, "FamoReactivateTextServiceForTest"));
  preview_selection_state_for_test_ =
      reinterpret_cast<PreviewSelectionStateForTestFn>(
          GetProcAddress(module_, "FamoGetPreviewSelectionStateForTest"));
  recovery_prepared_claims_for_test_ =
      reinterpret_cast<RecoveryPreparedClaimsForTestFn>(GetProcAddress(
          module_, "FamoGetRecoveryPreparedClaimsForTest"));
  recovery_execute_attempts_for_test_ =
      reinterpret_cast<RecoveryExecuteAttemptsForTestFn>(GetProcAddress(
          module_, "FamoGetRecoveryExecuteAttemptsForTest"));
  terminal_publication_ready_for_test_ =
      reinterpret_cast<TerminalPublicationReadyForTestFn>(GetProcAddress(
          module_, "FamoGetTerminalPublicationReadyForTest"));
  terminal_retired_sessions_for_test_ =
      reinterpret_cast<TerminalRetiredSessionsForTestFn>(GetProcAddress(
          module_, "FamoGetTerminalRetiredSessionsForTest"));
  terminal_cleanup_connect_attempts_for_test_ =
      reinterpret_cast<TerminalCleanupConnectAttemptsForTestFn>(GetProcAddress(
          module_, "FamoGetTerminalCleanupConnectAttemptsForTest"));
  return can_unload_ && create_for_test_ && reactivate_for_test_ &&
         preview_selection_state_for_test_ &&
         recovery_prepared_claims_for_test_ &&
         recovery_execute_attempts_for_test_ &&
         terminal_publication_ready_for_test_ &&
         terminal_retired_sessions_for_test_ &&
         terminal_cleanup_connect_attempts_for_test_;
}

bool TextServiceModule::CanUnload() const { return can_unload_() == S_OK; }

uint32_t TextServiceModule::TerminalCleanupConnectAttemptsForTest() const {
  return terminal_cleanup_connect_attempts_for_test_
             ? terminal_cleanup_connect_attempts_for_test_()
             : 0;
}

uint32_t TextServiceModule::RecoveryPreparedClaimsForTest() const {
  return recovery_prepared_claims_for_test_
             ? recovery_prepared_claims_for_test_()
             : 0;
}

uint32_t TextServiceModule::RecoveryExecuteAttemptsForTest() const {
  return recovery_execute_attempts_for_test_
             ? recovery_execute_attempts_for_test_()
             : 0;
}

uint32_t TextServiceModule::TerminalPublicationReadyForTest() const {
  return terminal_publication_ready_for_test_
             ? terminal_publication_ready_for_test_()
             : 0;
}

uint32_t TextServiceModule::TerminalRetiredSessionsForTest() const {
  return terminal_retired_sessions_for_test_
             ? terminal_retired_sessions_for_test_()
             : 0;
}

HRESULT TextServiceModule::CreateForTest(
    ITfThreadMgr *thread_manager, TfClientId client_id,
    ITfTextInputProcessorEx **service) const {
  return create_for_test_(thread_manager, client_id,
                          TestEndpointSuffix().c_str(), service);
}

HRESULT TextServiceModule::ReactivateForTest(
    ITfTextInputProcessorEx *service, ITfThreadMgr *thread_manager,
    TfClientId client_id) const {
  return reactivate_for_test_(service, thread_manager, client_id);
}

bool TextServiceModule::PreviewSelectionStateForTest(
    ITfTextInputProcessorEx *service, HWND *target,
    runtime::PreviewSelectionRequest *request) const {
  return preview_selection_state_for_test_(service, target, request) != FALSE;
}

} // namespace famo::tsf::test
