#include "registration.h"

#include <string>

#include <msctf.h>

#include "com_ptr.h"
#include "famo_guids.h"
#include "module_state.h"

namespace famo::tsf {

namespace {

class ComScope {
public:
  ComScope() {
    result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    owns_ = SUCCEEDED(result_);
    if (result_ == RPC_E_CHANGED_MODE)
      result_ = S_OK;
  }
  ~ComScope() {
    if (owns_)
      CoUninitialize();
  }
  HRESULT result() const { return result_; }

private:
  HRESULT result_ = E_FAIL;
  bool owns_ = false;
};

std::wstring GuidText(REFGUID guid) {
  wchar_t value[40]{};
  const int length = StringFromGUID2(guid, value, ARRAYSIZE(value));
  return length > 1 ? std::wstring(value, static_cast<size_t>(length - 1))
                    : std::wstring();
}

HRESULT SetString(HKEY root, const std::wstring &path, const wchar_t *name,
                  std::wstring_view value) {
  HKEY key = nullptr;
  const LSTATUS created = RegCreateKeyExW(
      root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS)
    return HRESULT_FROM_WIN32(created);
  const LSTATUS written = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.data()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return written == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(written);
}

std::wstring ComKey() {
  return L"Software\\Classes\\CLSID\\" + GuidText(kTextServiceClsid);
}

std::wstring TipKey() {
  return L"Software\\Microsoft\\CTF\\TIP\\" +
         GuidText(kTextServiceClsid);
}

HRESULT RegisterComServer() {
  const std::wstring module = ModulePath();
  if (module.empty())
    return HRESULT_FROM_WIN32(GetLastError());
  const std::wstring root = ComKey();
  HRESULT result = SetString(HKEY_CURRENT_USER, root, nullptr, kProfileName);
  if (FAILED(result))
    return result;
  result = SetString(HKEY_CURRENT_USER, root + L"\\InprocServer32", nullptr,
                     module);
  if (FAILED(result))
    return result;
  return SetString(HKEY_CURRENT_USER, root + L"\\InprocServer32",
                   L"ThreadingModel", L"Apartment");
}

void UnregisterComServer() {
  const LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, ComKey().c_str());
  (void)result;
}

HRESULT CreateProfiles(ComPtr<ITfInputProcessorProfiles> *profiles) {
  return CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,
      reinterpret_cast<void **>(profiles->put()));
}

HRESULT CreateCategories(ComPtr<ITfCategoryMgr> *categories) {
  return CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr,
                          reinterpret_cast<void **>(categories->put()));
}

HRESULT RegisterTsfProfile() {
  ComScope com;
  if (FAILED(com.result()))
    return com.result();
  ComPtr<ITfInputProcessorProfiles> profiles;
  HRESULT result = CreateProfiles(&profiles);
  if (FAILED(result))
    return result;
  result = profiles->Register(kTextServiceClsid);
  if (FAILED(result))
    return result;
  result = profiles->AddLanguageProfile(
      kTextServiceClsid, kLanguageId, kLanguageProfileGuid, kProfileName,
      static_cast<ULONG>(std::size(kProfileName) - 1), L"", 0, 0);
  if (FAILED(result))
    return result;
  result = profiles->EnableLanguageProfile(
      kTextServiceClsid, kLanguageId, kLanguageProfileGuid, TRUE);
  if (FAILED(result))
    return result;

  ComPtr<ITfCategoryMgr> categories;
  result = CreateCategories(&categories);
  if (FAILED(result))
    return result;
  return categories->RegisterCategory(kTextServiceClsid,
                                      GUID_TFCAT_TIP_KEYBOARD,
                                      kTextServiceClsid);
}

void UnregisterTsfProfile() {
  {
    ComScope com;
    if (SUCCEEDED(com.result())) {
      ComPtr<ITfCategoryMgr> categories;
      if (SUCCEEDED(CreateCategories(&categories)))
        categories->UnregisterCategory(kTextServiceClsid,
                                       GUID_TFCAT_TIP_KEYBOARD,
                                       kTextServiceClsid);
      ComPtr<ITfInputProcessorProfiles> profiles;
      if (SUCCEEDED(CreateProfiles(&profiles))) {
        profiles->EnableLanguageProfile(kTextServiceClsid, kLanguageId,
                                        kLanguageProfileGuid, FALSE);
        profiles->RemoveLanguageProfile(kTextServiceClsid, kLanguageId,
                                        kLanguageProfileGuid);
        profiles->Unregister(kTextServiceClsid);
      }
    }
  }
  // Windows can retain a disabled per-user preference after profile removal.
  // Delete it only after the TSF manager has been released; otherwise its
  // destructor can write the disabled preference back. The key is scoped to
  // this development CLSID, so unregister restores the exact prior state.
  RegDeleteTreeW(HKEY_CURRENT_USER, TipKey().c_str());
}

} // namespace

HRESULT RegisterDevelopmentProfile() {
  HRESULT result = RegisterComServer();
  if (FAILED(result))
    return result;
  result = RegisterTsfProfile();
  if (FAILED(result)) {
    UnregisterTsfProfile();
    UnregisterComServer();
  }
  return result;
}

HRESULT UnregisterDevelopmentProfile() {
  UnregisterTsfProfile();
  UnregisterComServer();
  return S_OK;
}

} // namespace famo::tsf

STDAPI DllRegisterServer() {
  return famo::tsf::RegisterDevelopmentProfile();
}

STDAPI DllUnregisterServer() {
  return famo::tsf::UnregisterDevelopmentProfile();
}
