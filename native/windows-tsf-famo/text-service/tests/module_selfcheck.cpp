#include <cstdio>
#include <string>

#include <msctf.h>

#include "famo_guids.h"

namespace {
DWORD RunLoadedProbe(const wchar_t *tool, const wchar_t *dll) {
  std::wstring command = L"\"" + std::wstring(tool) + L"\" loaded \"" +
                         dll + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process))
    return static_cast<DWORD>(-1);
  const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
  DWORD exit_code = static_cast<DWORD>(-1);
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exit_code;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 3)
    return 2;
  HMODULE module = LoadLibraryW(argv[1]);
  if (!module)
    return 1;
  using GetClassObject = HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, void **);
  using CanUnload = HRESULT(STDAPICALLTYPE *)();
  auto get_class = reinterpret_cast<GetClassObject>(
      GetProcAddress(module, "DllGetClassObject"));
  auto can_unload = reinterpret_cast<CanUnload>(
      GetProcAddress(module, "DllCanUnloadNow"));
  if (!get_class || !can_unload) {
    FreeLibrary(module);
    return 1;
  }
  IClassFactory *factory = nullptr;
  HRESULT result = get_class(famo::tsf::kTextServiceClsid, IID_IClassFactory,
                             reinterpret_cast<void **>(&factory));
  if (FAILED(result) || !factory) {
    FreeLibrary(module);
    return 1;
  }
  ITfTextInputProcessorEx *service = nullptr;
  result = factory->CreateInstance(
      nullptr, IID_ITfTextInputProcessorEx,
      reinterpret_cast<void **>(&service));
  factory->Release();
  if (FAILED(result) || !service) {
    FreeLibrary(module);
    return 1;
  }
  service->Release();
  const bool unloadable = can_unload() == S_OK;
  const bool loaded_detected = RunLoadedProbe(argv[2], argv[1]) == 0;
  FreeLibrary(module);
  const bool unloaded_detected = RunLoadedProbe(argv[2], argv[1]) == 1;
  if (!unloadable || !loaded_detected || !unloaded_detected)
    return 1;
  std::printf("module_selfcheck: OK\n");
  return 0;
}
