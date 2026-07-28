#pragma once

#include <cstdint>
#include <string_view>

#include <msctf.h>
#include <windows.h>

#include "famo_runtime_protocol.h"

namespace famo::tsf::test {

class RuntimeProcess {
public:
  ~RuntimeProcess();

  bool Start(const wchar_t *path, std::wstring_view fault = L"none",
             uint32_t fault_after = 0, uint32_t connections = 1,
             bool inline_preedit = true, uint32_t preview_rows = 0,
             int32_t expected_terminal_abandons = -1,
             int32_t expected_clients = -1,
             int32_t expected_sessions = -1);
  bool Finish();
  HWND PreviewSourceWindow() const;

private:
  void Stop();

  PROCESS_INFORMATION process_{};
};

class TextServiceModule {
public:
  ~TextServiceModule();

  bool Load(const wchar_t *path);
  bool CanUnload() const;
  HRESULT CreateForTest(ITfThreadMgr *thread_manager, TfClientId client_id,
                        ITfTextInputProcessorEx **service) const;
  HRESULT ReactivateForTest(ITfTextInputProcessorEx *service,
                            ITfThreadMgr *thread_manager,
                            TfClientId client_id) const;
  bool PreviewSelectionStateForTest(
      ITfTextInputProcessorEx *service, HWND *target,
      runtime::PreviewSelectionRequest *request) const;
  uint32_t TerminalCleanupConnectAttemptsForTest() const;

private:
  using CanUnloadFn = HRESULT(STDAPICALLTYPE *)();
  using CreateForTestFn = HRESULT(STDAPICALLTYPE *)(
      ITfThreadMgr *, TfClientId, const wchar_t *, ITfTextInputProcessorEx **);
  using ReactivateForTestFn = HRESULT(STDAPICALLTYPE *)(
      ITfTextInputProcessorEx *, ITfThreadMgr *, TfClientId);
  using PreviewSelectionStateForTestFn = BOOL(STDAPICALLTYPE *)(
      ITfTextInputProcessorEx *, HWND *, runtime::PreviewSelectionRequest *);
  using TerminalCleanupConnectAttemptsForTestFn =
      uint32_t(STDAPICALLTYPE *)();

  HMODULE module_ = nullptr;
  CanUnloadFn can_unload_ = nullptr;
  CreateForTestFn create_for_test_ = nullptr;
  ReactivateForTestFn reactivate_for_test_ = nullptr;
  PreviewSelectionStateForTestFn preview_selection_state_for_test_ = nullptr;
  TerminalCleanupConnectAttemptsForTestFn
      terminal_cleanup_connect_attempts_for_test_ = nullptr;
};

} // namespace famo::tsf::test
