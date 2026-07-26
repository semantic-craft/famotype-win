#include <cstdio>
#include <string>
#include <string_view>

#include <msctf.h>
#include <restartmanager.h>

#include "famo_guids.h"

namespace {

bool ProfileActive();
bool ProfileRegistered();
HRESULT SetProfileEnabled(BOOL enabled);
HRESULT SwitchAwayFromProfile();

std::wstring ModuleDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

HRESULT EnablePrivilege(const wchar_t *name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    return HRESULT_FROM_WIN32(GetLastError());
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
    const DWORD error = GetLastError();
    CloseHandle(token);
    return HRESULT_FROM_WIN32(error);
  }
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Luid = luid;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(
      token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
  const DWORD error = GetLastError();
  CloseHandle(token);
  return adjusted ? HRESULT_FROM_WIN32(error)
                  : HRESULT_FROM_WIN32(error == ERROR_SUCCESS
                                           ? ERROR_PRIVILEGE_NOT_HELD
                                           : error);
}

HRESULT RunAsDesktopUser(const std::wstring &executable,
                         const std::wstring &arguments, bool wait) {
  const HRESULT privilege = EnablePrivilege(L"SeImpersonatePrivilege");
  if (FAILED(privilege))
    return privilege;

  const HWND shell = GetShellWindow();
  if (!shell)
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  DWORD shell_process_id = 0;
  GetWindowThreadProcessId(shell, &shell_process_id);
  if (shell_process_id == 0)
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  HANDLE shell_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     shell_process_id);
  if (!shell_process)
    return HRESULT_FROM_WIN32(GetLastError());
  HANDLE shell_token = nullptr;
  if (!OpenProcessToken(shell_process, TOKEN_QUERY | TOKEN_DUPLICATE,
                        &shell_token)) {
    const DWORD error = GetLastError();
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(error);
  }
  HANDLE primary_token = nullptr;
  if (!DuplicateTokenEx(shell_token, MAXIMUM_ALLOWED, nullptr,
                        SecurityImpersonation, TokenPrimary, &primary_token)) {
    const DWORD error = GetLastError();
    CloseHandle(shell_token);
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(error);
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::wstring command_line = L"\"" + executable + L"\"";
  if (!arguments.empty())
    command_line += L" " + arguments;
  const BOOL created = CreateProcessWithTokenW(
      primary_token, LOGON_WITH_PROFILE, executable.c_str(),
      command_line.data(), 0, nullptr, ModuleDirectory().c_str(), &startup,
      &process);
  DWORD error = created ? ERROR_SUCCESS : GetLastError();
  if (created) {
    if (wait) {
      const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
      DWORD exit_code = 1;
      if (waited != WAIT_OBJECT_0)
        error = GetLastError();
      else if (!GetExitCodeProcess(process.hProcess, &exit_code))
        error = GetLastError();
      else if (exit_code != 0)
        error = ERROR_PROCESS_ABORTED;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  CloseHandle(primary_token);
  CloseHandle(shell_token);
  CloseHandle(shell_process);
  return HRESULT_FROM_WIN32(error);
}

HRESULT StartRuntimeAsDesktopUser() {
  const std::wstring directory = ModuleDirectory();
  if (directory.empty())
    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  return RunAsDesktopUser(directory + L"\\FamoRuntime.exe", L"", false);
}

std::wstring TextServiceGuidText() {
  wchar_t guid[40]{};
  StringFromGUID2(famo::tsf::kTextServiceClsid, guid, ARRAYSIZE(guid));
  return guid;
}

bool UserKeyPresent(const std::wstring &path) {
  HKEY key = nullptr;
  const LSTATUS opened =
      RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
  if (opened == ERROR_SUCCESS)
    RegCloseKey(key);
  return opened == ERROR_SUCCESS;
}

HRESULT CleanupCurrentUserProfileState() {
  HRESULT result = SwitchAwayFromProfile();
  if (SUCCEEDED(result) && ProfileRegistered())
    result = SetProfileEnabled(FALSE);

  const std::wstring guid = TextServiceGuidText();
  const std::wstring tip = L"Software\\Microsoft\\CTF\\TIP\\" + guid;
  const std::wstring com = L"Software\\Classes\\CLSID\\" + guid;
  const LSTATUS tip_removed = RegDeleteTreeW(HKEY_CURRENT_USER, tip.c_str());
  const LSTATUS com_removed = RegDeleteTreeW(HKEY_CURRENT_USER, com.c_str());
  if (FAILED(result))
    return result;
  if (tip_removed != ERROR_SUCCESS && tip_removed != ERROR_FILE_NOT_FOUND)
    return HRESULT_FROM_WIN32(tip_removed);
  if (com_removed != ERROR_SUCCESS && com_removed != ERROR_FILE_NOT_FOUND)
    return HRESULT_FROM_WIN32(com_removed);
  return !ProfileActive() && !UserKeyPresent(tip) && !UserKeyPresent(com)
             ? S_OK
             : E_FAIL;
}

HRESULT CleanupDesktopUser() {
  const std::wstring directory = ModuleDirectory();
  if (directory.empty())
    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);

  RunAsDesktopUser(directory + L"\\FamoRuntime.exe", L"--control shutdown",
                   true);
  HRESULT result = RunAsDesktopUser(
      directory + L"\\FamoProfileTool.exe", L"cleanup-user-state", true);
  if (FAILED(result))
    return result;
  return RunAsDesktopUser(directory + L"\\settings\\FamoSettings.exe",
                          L"--remove-input-tip", true);
}

using RegistrationEntry = HRESULT(STDAPICALLTYPE *)();

HRESULT InvokeRegistration(const char *export_name) {
  const std::wstring dll = ModuleDirectory() + L"\\FamoTextService.dll";
  HMODULE module = LoadLibraryW(dll.c_str());
  if (!module)
    return HRESULT_FROM_WIN32(GetLastError());
  auto entry = reinterpret_cast<RegistrationEntry>(
      GetProcAddress(module, export_name));
  const HRESULT result = entry ? entry() : HRESULT_FROM_WIN32(GetLastError());
  FreeLibrary(module);
  return result;
}

bool RegistryPresentAt(HKEY root) {
  wchar_t guid[40]{};
  StringFromGUID2(famo::tsf::kTextServiceClsid, guid, ARRAYSIZE(guid));
  const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") +
                            guid + L"\\InprocServer32";
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    return false;
  RegCloseKey(key);
  return true;
}

bool RegistryPresent() {
  // COM registration moved to HKLM (system-wide) so the Win11 switcher lists
  // the IME; still accept HKCU so the probe recognises legacy per-user
  // development registrations during upgrades.
  return RegistryPresentAt(HKEY_LOCAL_MACHINE) ||
         RegistryPresentAt(HKEY_CURRENT_USER);
}

bool ProfileEnabled() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  ITfInputProcessorProfiles *profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&profiles));
  BOOL enabled = FALSE;
  if (SUCCEEDED(result)) {
    result = profiles->IsEnabledLanguageProfile(
        famo::tsf::kTextServiceClsid, famo::tsf::kLanguageId,
        famo::tsf::kLanguageProfileGuid, &enabled);
    profiles->Release();
  }
  if (owns_com)
    CoUninitialize();
  return SUCCEEDED(result) && enabled;
}

HRESULT SetProfileEnabled(BOOL enabled) {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    return initialized;
  ITfInputProcessorProfiles *profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&profiles));
  if (SUCCEEDED(result)) {
    result = profiles->EnableLanguageProfile(
        famo::tsf::kTextServiceClsid, famo::tsf::kLanguageId,
        famo::tsf::kLanguageProfileGuid, enabled);
    profiles->Release();
  }
  if (owns_com)
    CoUninitialize();
  return result;
}

bool ProfileRegistered() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  ITfInputProcessorProfileMgr *profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr,
      reinterpret_cast<void **>(&profiles));
  TF_INPUTPROCESSORPROFILE profile{};
  if (SUCCEEDED(result)) {
    result = profiles->GetProfile(
        TF_PROFILETYPE_INPUTPROCESSOR, famo::tsf::kLanguageId,
        famo::tsf::kTextServiceClsid, famo::tsf::kLanguageProfileGuid, nullptr,
        &profile);
    profiles->Release();
  }
  if (owns_com)
    CoUninitialize();
  return result == S_OK &&
         profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
         profile.langid == famo::tsf::kLanguageId &&
         IsEqualGUID(profile.clsid, famo::tsf::kTextServiceClsid) &&
         IsEqualGUID(profile.guidProfile, famo::tsf::kLanguageProfileGuid);
}

bool ProfileActive() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  ITfInputProcessorProfileMgr *profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr,
      reinterpret_cast<void **>(&profiles));
  TF_INPUTPROCESSORPROFILE active{};
  if (SUCCEEDED(result)) {
    result = profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
    profiles->Release();
  }
  if (owns_com)
    CoUninitialize();
  return result == S_OK &&
         active.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
         active.langid == famo::tsf::kLanguageId &&
         IsEqualGUID(active.clsid, famo::tsf::kTextServiceClsid) &&
         IsEqualGUID(active.guidProfile,
                     famo::tsf::kLanguageProfileGuid);
}

HRESULT ActivateProfile(LANGID *previous_language) {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    return initialized;

  ITfInputProcessorProfiles *languages = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&languages));
  LANGID previous = 0;
  if (SUCCEEDED(result))
    result = languages->GetCurrentLanguage(&previous);
  if (SUCCEEDED(result) && previous != famo::tsf::kLanguageId)
    result = languages->ChangeCurrentLanguage(famo::tsf::kLanguageId);

  ITfInputProcessorProfileMgr *profiles = nullptr;
  if (SUCCEEDED(result)) {
    result = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void **>(&profiles));
  }
  if (SUCCEEDED(result)) {
    result = profiles->ActivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR, famo::tsf::kLanguageId,
        famo::tsf::kTextServiceClsid, famo::tsf::kLanguageProfileGuid,
        nullptr, TF_IPPMF_ENABLEPROFILE | TF_IPPMF_FORSESSION);
  }
  if (FAILED(result) && languages && previous != 0 &&
      previous != famo::tsf::kLanguageId) {
    languages->ChangeCurrentLanguage(previous);
  }
  if (profiles)
    profiles->Release();
  if (languages)
    languages->Release();
  if (owns_com)
    CoUninitialize();
  if (previous_language)
    *previous_language = previous;
  return result;
}

HRESULT SwitchAwayFromProfile() {
  if (!ProfileActive())
    return S_OK;
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    return initialized;

  ITfInputProcessorProfileMgr *profiles = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfileMgr,
      reinterpret_cast<void **>(&profiles));
  if (SUCCEEDED(result)) {
    result = profiles->DeactivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR, famo::tsf::kLanguageId,
        famo::tsf::kTextServiceClsid, famo::tsf::kLanguageProfileGuid,
        nullptr, TF_IPPMF_FORSESSION);
    profiles->Release();
  }

  if (ProfileActive()) {
    ITfInputProcessorProfiles *languages = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&languages));
    if (SUCCEEDED(created)) {
      const HRESULT changed = languages->ChangeCurrentLanguage(0x0409);
      if (FAILED(result))
        result = changed;
      languages->Release();
    }
  }
  if (owns_com)
    CoUninitialize();
  return ProfileActive() ? E_FAIL : result;
}

bool KeyboardCategoryRegistered() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  ITfCategoryMgr *categories = nullptr;
  HRESULT result = CoCreateInstance(
      CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
      reinterpret_cast<void **>(&categories));
  IEnumGUID *items = nullptr;
  if (SUCCEEDED(result)) {
    result = categories->EnumCategoriesInItem(famo::tsf::kTextServiceClsid,
                                               &items);
    categories->Release();
  }
  bool found = false;
  if (SUCCEEDED(result) && items) {
    GUID item{};
    ULONG fetched = 0;
    while (items->Next(1, &item, &fetched) == S_OK && fetched == 1) {
      if (IsEqualGUID(item, GUID_TFCAT_TIP_KEYBOARD)) {
        found = true;
        break;
      }
    }
    items->Release();
  }
  if (owns_com)
    CoUninitialize();
  return found;
}

void WaitForRegistrationVisibility(bool present, bool expected_enabled = true) {
  constexpr DWORD kPollIntervalMs = 50;
  constexpr DWORD kMaxWaitMs = 2000;
  for (DWORD waited = 0; waited < kMaxWaitMs; waited += kPollIntervalMs) {
    const bool visible = RegistryPresent() && ProfileRegistered() &&
                         ProfileEnabled() == expected_enabled &&
                         KeyboardCategoryRegistered();
    const bool removed = !RegistryPresent() && !ProfileRegistered() &&
                         !KeyboardCategoryRegistered();
    if ((present && visible) || (!present && removed))
      return;
    Sleep(kPollIntervalMs);
  }
}

void WaitForMachineRegistrationRemoval() {
  constexpr DWORD kPollIntervalMs = 50;
  constexpr DWORD kMaxWaitMs = 2000;
  for (DWORD waited = 0; waited < kMaxWaitMs; waited += kPollIntervalMs) {
    if (!RegistryPresentAt(HKEY_LOCAL_MACHINE) && !ProfileRegistered() &&
        !KeyboardCategoryRegistered())
      return;
    Sleep(kPollIntervalMs);
  }
}

enum class LoadedState { NotLoaded, Loaded, Error };

LoadedState IsFileLoaded(const wchar_t *path, DWORD *error) {
  DWORD session = 0;
  wchar_t key[CCH_RM_SESSION_KEY + 1]{};
  DWORD result = RmStartSession(&session, 0, key);
  if (result != ERROR_SUCCESS) {
    *error = result;
    return LoadedState::Error;
  }
  const wchar_t *resources[] = {path};
  result = RmRegisterResources(session, 1, resources, 0, nullptr, 0, nullptr);
  UINT needed = 0;
  UINT count = 0;
  DWORD reasons = 0;
  if (result == ERROR_SUCCESS)
    result = RmGetList(session, &needed, &count, nullptr, &reasons);
  RmEndSession(session);
  if (result == ERROR_MORE_DATA && needed > 0)
    return LoadedState::Loaded;
  if (result == ERROR_SUCCESS)
    return needed > 0 || count > 0 ? LoadedState::Loaded
                                   : LoadedState::NotLoaded;
  *error = result;
  return LoadedState::Error;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc >= 2 && std::wstring_view(argv[1]) == L"loaded") {
    if (argc != 3)
      return 2;
    DWORD error = ERROR_SUCCESS;
    const LoadedState state = IsFileLoaded(argv[2], &error);
    if (state == LoadedState::Error) {
      std::fwprintf(stderr, L"loaded-module probe failed: %lu\n", error);
      return 3;
    }
    std::wprintf(L"loaded=%ls path=%ls\n",
                 state == LoadedState::Loaded ? L"yes" : L"no", argv[2]);
    return state == LoadedState::Loaded ? 0 : 1;
  }
  if (argc != 2) {
    std::fwprintf(
        stderr,
        L"usage: FamoProfileTool register|register-disabled|enable|disable|activate|check|check-disabled|check-absent|is-active|switch-away|start-runtime|cleanup-user|unregister|unregister-machine|loaded <dll>\n");
    return 2;
  }
  const std::wstring_view command(argv[1]);
  if (command == L"start-runtime") {
    const HRESULT result = StartRuntimeAsDesktopUser();
    if (FAILED(result)) {
      std::fwprintf(stderr, L"runtime start failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return static_cast<int>(HRESULT_CODE(result));
    }
    std::wprintf(L"runtime_started=yes path=%ls\\FamoRuntime.exe\n",
                  ModuleDirectory().c_str());
    return 0;
  } else if (command == L"cleanup-user") {
    const HRESULT result = CleanupDesktopUser();
    if (FAILED(result)) {
      std::fwprintf(stderr, L"desktop user cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"desktop_user_cleanup=yes\n");
    return 0;
  } else if (command == L"cleanup-user-state") {
    const HRESULT result = CleanupCurrentUserProfileState();
    if (FAILED(result)) {
      std::fwprintf(stderr, L"current user profile cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"current_user_profile_cleanup=yes\n");
    return 0;
  } else if (command == L"register" || command == L"register-disabled") {
    const HRESULT result = InvokeRegistration("DllRegisterServer");
    if (FAILED(result)) {
      std::fwprintf(stderr, L"profile registration failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    if (command == L"register-disabled") {
      const HRESULT disabled = SetProfileEnabled(FALSE);
      if (FAILED(disabled)) {
        InvokeRegistration("DllUnregisterServer");
        std::fwprintf(stderr, L"profile disable failed: 0x%08lx\n",
                      static_cast<unsigned long>(disabled));
        return 1;
      }
      WaitForRegistrationVisibility(true, false);
    } else {
      WaitForRegistrationVisibility(true);
    }
  } else if (command == L"enable" || command == L"disable") {
    const HRESULT result =
        SetProfileEnabled(command == L"enable" ? TRUE : FALSE);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"profile enable state failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    WaitForRegistrationVisibility(true, command == L"enable");
  } else if (command == L"activate") {
    LANGID previous_language = 0;
    const HRESULT result = ActivateProfile(&previous_language);
    if (FAILED(result)) {
      std::fwprintf(stderr,
                    L"profile activation failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"previous_language=0x%04x\n", previous_language);
  } else if (command == L"switch-away") {
    const HRESULT result = SwitchAwayFromProfile();
    if (FAILED(result)) {
      std::fwprintf(stderr, L"profile switch-away failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
  } else if (command == L"unregister" || command == L"unregister-machine") {
    const HRESULT result = InvokeRegistration(
        command == L"unregister-machine" ? "DllUnregisterMachine"
                                          : "DllUnregisterServer");
    if (FAILED(result)) {
      std::fwprintf(stderr, L"profile removal failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    if (command == L"unregister-machine")
      WaitForMachineRegistrationRemoval();
    else
      WaitForRegistrationVisibility(false);
  } else if (command != L"check" && command != L"check-disabled" &&
              command != L"check-absent" &&
              command != L"is-active") {
    return 2;
  }

  const bool registry = RegistryPresent();
  const bool profile = ProfileRegistered();
  const bool enabled = ProfileEnabled();
  const bool category = KeyboardCategoryRegistered();
  const bool active = ProfileActive();
  if (command == L"unregister-machine") {
    const bool machine_registry = RegistryPresentAt(HKEY_LOCAL_MACHINE);
    std::wprintf(L"machine_registry=%ls profile=%ls category=%ls\n",
                 machine_registry ? L"present" : L"absent",
                 profile ? L"present" : L"absent",
                 category ? L"present" : L"absent");
    return !machine_registry && !profile && !category ? 0 : 1;
  }
  if (command == L"unregister") {
    std::wprintf(
        L"registry=%ls profile=%ls enabled=%ls category=%ls active=%ls\n",
                 registry ? L"present" : L"absent",
                 profile ? L"present" : L"absent",
                 enabled ? L"yes" : L"no",
                 category ? L"present" : L"absent",
                 active ? L"yes" : L"no");
    return !registry && !profile && !category ? 0 : 1;
  }
  std::wprintf(
      L"registry=%ls profile=%ls enabled=%ls category=%ls active=%ls\n",
               registry ? L"present" : L"absent",
               profile ? L"present" : L"absent",
               enabled ? L"yes" : L"no",
               category ? L"present" : L"absent",
               active ? L"yes" : L"no");
  if (command == L"is-active")
    return active ? 0 : 1;
  if (command == L"check-absent")
    return !registry && !profile && !category && !active ? 0 : 1;
  if (command == L"switch-away")
    return registry && profile && category && !active ? 0 : 1;
  if (command == L"register-disabled" || command == L"check-disabled" ||
      command == L"disable")
    return registry && profile && !enabled && category && !active ? 0 : 1;
  return registry && profile && enabled && category &&
                 (command != L"activate" || active) &&
                 (command != L"switch-away" || !active)
             ? 0
             : 1;
}
