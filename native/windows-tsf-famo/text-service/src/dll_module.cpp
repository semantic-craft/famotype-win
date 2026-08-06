#include <atomic>
#include <new>

#include <windows.h>

#include "abi_boundary.h"
#include "famo_guids.h"
#include "module_state.h"
#include "text_service.h"

namespace famo::tsf {

class ClassFactory final : public IClassFactory {
public:
  ClassFactory() { AddModuleObject(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_IClassFactory)
      *object = static_cast<IClassFactory *>(this);
    if (!*object)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0)
      delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid,
                                           void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (outer)
      return CLASS_E_NOAGGREGATION;
    return ComBoundary([&] { return CreateTextServiceInstance(iid, object); });
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    lock ? AddServerLock() : RemoveServerLock();
    return S_OK;
  }

private:
  ~ClassFactory() { RemoveModuleObject(); }
  std::atomic<ULONG> references_{1};
};

} // namespace famo::tsf

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) {
    famo::tsf::SetModuleHandle(instance);
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllCanUnloadNow() {
  return famo::tsf::ModuleCanUnload() ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (clsid != famo::tsf::kTextServiceClsid)
    return CLASS_E_CLASSNOTAVAILABLE;
  return famo::tsf::ComBoundary([&] {
    auto *factory = new (std::nothrow) famo::tsf::ClassFactory();
    if (!factory)
      return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
  });
}

extern "C" HRESULT STDAPICALLTYPE FamoCreateTextServiceForTest(
    ITfThreadMgr *thread_manager, TfClientId client_id,
    const wchar_t *runtime_endpoint_suffix, ITfTextInputProcessorEx **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (!runtime_endpoint_suffix || !*runtime_endpoint_suffix)
    return E_INVALIDARG;
  return famo::tsf::ComBoundary([&] {
    auto *service = new (std::nothrow)
        famo::tsf::TextService(runtime_endpoint_suffix);
    if (!service)
      return E_OUTOFMEMORY;
    HRESULT result = service->ActivateForTest(thread_manager, client_id);
    if (SUCCEEDED(result)) {
      result = service->QueryInterface(
          IID_ITfTextInputProcessorEx, reinterpret_cast<void **>(object));
    }
    service->Release();
    return result;
  });
}

extern "C" HRESULT STDAPICALLTYPE FamoReactivateTextServiceForTest(
    ITfTextInputProcessorEx *object, ITfThreadMgr *thread_manager,
    TfClientId client_id) {
  if (!object || !thread_manager)
    return E_INVALIDARG;
  return famo::tsf::ComBoundary(
      [&] {
        return static_cast<famo::tsf::TextService *>(object)->ActivateForTest(
            thread_manager, client_id);
      });
}

extern "C" BOOL STDAPICALLTYPE FamoGetPreviewSelectionStateForTest(
    ITfTextInputProcessorEx *object, HWND *target,
    famo::runtime::PreviewSelectionRequest *request) {
  if (target)
    *target = nullptr;
  if (request)
    *request = {};
  if (!object || !target || !request)
    return FALSE;
  return famo::tsf::BoundaryOr<BOOL>(FALSE, [&] {
    return static_cast<famo::tsf::TextService *>(object)
                   ->PreviewSelectionStateForTest(target, request)
               ? TRUE
               : FALSE;
  });
}

extern "C" BOOL STDAPICALLTYPE
FamoGetUiStateForTest(ITfTextInputProcessorEx *object, ITfContext *context,
                      famo::runtime::UiState *state) {
  if (state)
    *state = {};
  if (!object || !context || !state)
    return FALSE;
  return famo::tsf::BoundaryOr<BOOL>(FALSE, [&] {
    return static_cast<famo::tsf::TextService *>(object)->UiStateForTest(
               context, state)
               ? TRUE
               : FALSE;
  });
}

extern "C" uint32_t STDAPICALLTYPE
FamoGetTerminalCleanupConnectAttemptsForTest() {
  return famo::tsf::TerminalCleanupConnectAttemptsForTest();
}

extern "C" uint32_t STDAPICALLTYPE FamoGetRecoveryPreparedClaimsForTest() {
  return famo::tsf::RecoveryPreparedClaimsForTest();
}

extern "C" uint32_t STDAPICALLTYPE FamoGetRecoveryExecuteAttemptsForTest() {
  return famo::tsf::RecoveryExecuteAttemptsForTest();
}

extern "C" uint32_t STDAPICALLTYPE FamoGetTerminalPublicationReadyForTest() {
  return famo::tsf::TerminalPublicationReadyForTest();
}

extern "C" uint32_t STDAPICALLTYPE FamoGetTerminalRetiredSessionsForTest() {
  return famo::tsf::TerminalRetiredSessionsForTest();
}
