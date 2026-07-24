#include "tsf_integration_support.h"

#include <chrono>
#include <string>

#include "famo_runtime_pipe.h"

namespace famo::tsf::test {
namespace {

const std::wstring &TestEndpointSuffix() {
  static const std::wstring suffix =
      L"dev-test-engine-" + std::to_wstring(GetCurrentProcessId());
  return suffix;
}

} // namespace

RuntimeProcess::~RuntimeProcess() { Stop(); }

bool RuntimeProcess::Start(const wchar_t *path, std::wstring_view fault,
                           uint32_t fault_after, uint32_t connections,
                           bool inline_preedit) {
  std::wstring command =
      L"\"" + std::wstring(path) + L"\" --endpoint-suffix " +
      TestEndpointSuffix() + L" --fault " + std::wstring(fault) +
      L" --fault-after " + std::to_wstring(fault_after) +
      L" --connections " + std::to_wstring(connections) +
      L" --inline-preedit " + (inline_preedit ? L"true" : L"false");
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
  return wait == WAIT_OBJECT_0 && exit_code == 0;
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
  return can_unload_ && create_for_test_ && reactivate_for_test_;
}

bool TextServiceModule::CanUnload() const { return can_unload_() == S_OK; }

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

} // namespace famo::tsf::test
