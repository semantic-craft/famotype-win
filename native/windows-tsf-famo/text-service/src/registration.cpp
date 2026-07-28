#include "registration.h"

#include <string>

#include <msctf.h>

#include "abi_boundary.h"
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

HRESULT SetDword(HKEY root, const std::wstring &path, const wchar_t *name,
                 DWORD value) {
  HKEY key = nullptr;
  const LSTATUS created = RegCreateKeyExW(
      root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS)
    return HRESULT_FROM_WIN32(created);
  const LSTATUS written =
      RegSetValueExW(key, name, 0, REG_DWORD,
                     reinterpret_cast<const BYTE *>(&value), sizeof(value));
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

// System-wide COM registration (HKLM). A per-user (HKCU) registration keeps
// the TIP loadable, but Win11's immersive switcher only lists IMEs whose
// registration is machine-scoped — an HKCU-only Famo is invisible in
// Win+Space even with the profile enabled. Registration therefore requires
// elevation; the installer runs it elevated.
HRESULT RegisterComServer(bool cleanup_current_user) {
  const std::wstring module = ModulePath();
  if (module.empty())
    return HRESULT_FROM_WIN32(GetLastError());
  const std::wstring root = ComKey();
  HRESULT result = SetString(HKEY_LOCAL_MACHINE, root, nullptr, kProfileName);
  if (FAILED(result)) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str());
    return result;
  }
  result = SetString(HKEY_LOCAL_MACHINE, root + L"\\InprocServer32", nullptr,
                     module);
  if (FAILED(result)) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str());
    return result;
  }
  result = SetString(HKEY_LOCAL_MACHINE, root + L"\\InprocServer32",
                     L"ThreadingModel", L"Apartment");
  if (FAILED(result)) {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str());
    return result;
  }
  if (cleanup_current_user) {
    const LSTATUS removed =
        RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
      RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str());
      return HRESULT_FROM_WIN32(removed);
    }
  }
  return S_OK;
}

void UnregisterComServer(bool cleanup_current_user) {
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, ComKey().c_str());
  if (cleanup_current_user)
    RegDeleteTreeW(HKEY_CURRENT_USER, ComKey().c_str());
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

// Capability categories a modern keyboard TIP must declare. Registering only
// GUID_TFCAT_TIP_KEYBOARD leaves the profile invisible in the Win+Space
// immersive switcher on Win8+ — GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT is the one
// that surfaces it there; the rest let it run in secure fields, the systray,
// COM-less hosts, and provide display attributes. This mirrors the set a
// shipping IME (e.g. WeType) registers.
const GUID kProfileCategories[] = {
    GUID_TFCAT_TIP_KEYBOARD,
    GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    GUID_TFCAT_TIPCAP_SECUREMODE,
    GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    GUID_TFCAT_TIPCAP_COMLESS,
    GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
};

HRESULT RegisterTsfProfile(bool enable_current_user) {
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
  if (enable_current_user) {
    result = profiles->EnableLanguageProfile(
        kTextServiceClsid, kLanguageId, kLanguageProfileGuid, TRUE);
    if (FAILED(result))
      return result;
  }
  // Machine-default enable flag on the HKLM profile key. EnableLanguageProfile
  // above only writes the calling user's HKCU preference; without this value a
  // fresh user account sees the profile disabled, and Win11 treats the profile
  // as not offerable. Shipping IMEs (e.g. WeType) carry Enable=1 here.
  {
    wchar_t langId[16]{};
    swprintf(langId, ARRAYSIZE(langId), L"0x%08x",
             static_cast<unsigned>(kLanguageId));
    result = SetDword(HKEY_LOCAL_MACHINE,
                      TipKey() + L"\\LanguageProfile\\" + langId + L"\\" +
                          GuidText(kLanguageProfileGuid),
                      L"Enable", 1);
    if (FAILED(result))
      return result;
  }

  ComPtr<ITfCategoryMgr> categories;
  result = CreateCategories(&categories);
  if (FAILED(result))
    return result;
  for (const GUID &category : kProfileCategories) {
    result =
        categories->RegisterCategory(kTextServiceClsid, category, kTextServiceClsid);
    if (FAILED(result))
      return result;
  }
  return S_OK;
}

void UnregisterTsfProfile(bool cleanup_current_user) {
  {
    ComScope com;
    if (SUCCEEDED(com.result())) {
      ComPtr<ITfCategoryMgr> categories;
      if (SUCCEEDED(CreateCategories(&categories))) {
        for (const GUID &category : kProfileCategories)
          categories->UnregisterCategory(kTextServiceClsid, category,
                                         kTextServiceClsid);
      }
      ComPtr<ITfInputProcessorProfiles> profiles;
      if (SUCCEEDED(CreateProfiles(&profiles))) {
        if (cleanup_current_user) {
          profiles->EnableLanguageProfile(kTextServiceClsid, kLanguageId,
                                          kLanguageProfileGuid, FALSE);
        }
        profiles->RemoveLanguageProfile(kTextServiceClsid, kLanguageId,
                                        kLanguageProfileGuid);
        profiles->Unregister(kTextServiceClsid);
      }
    }
  }
  if (cleanup_current_user) {
    // Delete only after the TSF manager has been released; its destructor can
    // otherwise write the disabled preference back.
    RegDeleteTreeW(HKEY_CURRENT_USER, TipKey().c_str());
  }
}

} // namespace

HRESULT RegisterDevelopmentProfile() {
  HRESULT result = RegisterComServer(true);
  if (FAILED(result))
    return result;
  result = RegisterTsfProfile(true);
  if (FAILED(result)) {
    UnregisterTsfProfile(true);
    UnregisterComServer(true);
  }
  return result;
}

HRESULT RegisterMachineProfile() {
  HRESULT result = RegisterComServer(false);
  if (FAILED(result))
    return result;
  result = RegisterTsfProfile(false);
  if (FAILED(result)) {
    UnregisterTsfProfile(false);
    UnregisterComServer(false);
  }
  return result;
}

HRESULT UnregisterDevelopmentProfile() {
  UnregisterTsfProfile(true);
  UnregisterComServer(true);
  return S_OK;
}

HRESULT UnregisterMachineProfile() {
  UnregisterTsfProfile(false);
  UnregisterComServer(false);
  return S_OK;
}

} // namespace famo::tsf

STDAPI DllRegisterServer() {
  return famo::tsf::ComBoundary(
      [] { return famo::tsf::RegisterDevelopmentProfile(); });
}

STDAPI DllRegisterMachine() {
  return famo::tsf::ComBoundary(
      [] { return famo::tsf::RegisterMachineProfile(); });
}

STDAPI DllUnregisterServer() {
  return famo::tsf::ComBoundary(
      [] { return famo::tsf::UnregisterDevelopmentProfile(); });
}

STDAPI DllUnregisterMachine() {
  return famo::tsf::ComBoundary(
      [] { return famo::tsf::UnregisterMachineProfile(); });
}
