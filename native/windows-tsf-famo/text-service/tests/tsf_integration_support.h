#pragma once

#include <cstdint>
#include <string_view>

#include <msctf.h>
#include <windows.h>

namespace famo::tsf::test {

class RuntimeProcess {
public:
  ~RuntimeProcess();

  bool Start(const wchar_t *path, std::wstring_view fault = L"none",
             uint32_t fault_after = 0, uint32_t connections = 1,
             bool inline_preedit = true);
  bool Finish();

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

private:
  using CanUnloadFn = HRESULT(STDAPICALLTYPE *)();
  using CreateForTestFn = HRESULT(STDAPICALLTYPE *)(
      ITfThreadMgr *, TfClientId, const wchar_t *, ITfTextInputProcessorEx **);
  using ReactivateForTestFn = HRESULT(STDAPICALLTYPE *)(
      ITfTextInputProcessorEx *, ITfThreadMgr *, TfClientId);

  HMODULE module_ = nullptr;
  CanUnloadFn can_unload_ = nullptr;
  CreateForTestFn create_for_test_ = nullptr;
  ReactivateForTestFn reactivate_for_test_ = nullptr;
};

} // namespace famo::tsf::test
