#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <msctf.h>
#include <restartmanager.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>

#include "famo_guids.h"

namespace {

bool ProfileActive();
bool ProfileRegistered();
HRESULT SetProfileEnabled(BOOL enabled);
HRESULT SwitchAwayFromProfile();
bool TokenIsMediumIntegrityDesktop(HANDLE token);
bool CurrentProcessTokenMatchesSid(std::wstring_view expected_sid);
enum class LoadedState { NotLoaded, Loaded, Error };
LoadedState IsFileLoaded(const wchar_t *path, DWORD *error);
HRESULT WaitForExecutableExit(const std::wstring &path);

constexpr wchar_t kIdentityPipePrefix[] =
    L"\\\\.\\pipe\\FamoInstallerIdentity-";
constexpr DWORD kIdentityPipeTimeoutMs = 15000;

DWORD WaitForOverlappedUntil(HANDLE handle, OVERLAPPED *operation,
                             ULONGLONG deadline, DWORD *transferred) {
  const ULONGLONG now = GetTickCount64();
  const DWORD remaining =
      now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
  const DWORD waited = WaitForSingleObject(operation->hEvent, remaining);
  if (waited == WAIT_OBJECT_0)
    return GetOverlappedResult(handle, operation, transferred, FALSE)
               ? ERROR_SUCCESS
               : GetLastError();

  const DWORD wait_error =
      waited == WAIT_TIMEOUT
          ? ERROR_TIMEOUT
          : (waited == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
  const BOOL cancelled = CancelIoEx(handle, operation);
  const DWORD cancel_error = cancelled ? ERROR_SUCCESS : GetLastError();
  const BOOL drained =
      GetOverlappedResult(handle, operation, transferred, TRUE);
  const DWORD drain_error = drained ? ERROR_SUCCESS : GetLastError();
  // The operation can complete between the wait result and CancelIoEx. In that
  // boundary race, a successful blocking drain is the authoritative result.
  if (drained)
    return ERROR_SUCCESS;
  if (!cancelled && cancel_error != ERROR_NOT_FOUND)
    return cancel_error;
  if (drain_error != ERROR_OPERATION_ABORTED)
    return drain_error;
  return wait_error;
}

bool IsHexNonce(std::wstring_view nonce) {
  if (nonce.size() != 32)
    return false;
  for (const wchar_t value : nonce) {
    if (!((value >= L'0' && value <= L'9') ||
          (value >= L'a' && value <= L'f') ||
          (value >= L'A' && value <= L'F')))
      return false;
  }
  return true;
}

bool IsLowerHexTransactionId(std::wstring_view value) {
  if (value.size() != 32)
    return false;
  for (const wchar_t character : value) {
    if (!((character >= L'0' && character <= L'9') ||
          (character >= L'a' && character <= L'f')))
      return false;
  }
  return true;
}

bool IsUpperHexHash(std::wstring_view value) {
  if (value.size() != 64)
    return false;
  for (const wchar_t character : value) {
    if (!((character >= L'0' && character <= L'9') ||
          (character >= L'A' && character <= L'F')))
      return false;
  }
  return true;
}

std::wstring IdentityPipeName(std::wstring_view nonce) {
  return std::wstring(kIdentityPipePrefix) + std::wstring(nonce);
}

bool TokenBelongsToAdministrators(HANDLE token) {
  BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
  DWORD sid_size = sizeof(sid_buffer);
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid_buffer,
                          &sid_size))
    return false;
  BOOL member = FALSE;
  return CheckTokenMembership(token, sid_buffer, &member) && member;
}

bool TokenCanResumeElevated(HANDLE token) {
  if (TokenBelongsToAdministrators(token))
    return true;

  TOKEN_LINKED_TOKEN linked{};
  DWORD returned = 0;
  if (!GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked),
                           &returned))
    return false;
  const bool capable = TokenBelongsToAdministrators(linked.LinkedToken);
  CloseHandle(linked.LinkedToken);
  return capable;
}

bool TokenIdentity(HANDLE token, std::wstring *sid_text,
                   std::wstring *account, DWORD *session_id,
                   bool *resume_capable) {
  DWORD token_user_size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &token_user_size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_user_size == 0)
    return false;
  std::vector<BYTE> token_user_buffer(token_user_size);
  if (!GetTokenInformation(token, TokenUser, token_user_buffer.data(),
                           token_user_size, &token_user_size))
    return false;
  const auto *token_user =
      reinterpret_cast<const TOKEN_USER *>(token_user_buffer.data());
  if (!IsValidSid(token_user->User.Sid))
    return false;

  LPWSTR allocated_sid = nullptr;
  if (!ConvertSidToStringSidW(token_user->User.Sid, &allocated_sid))
    return false;
  *sid_text = allocated_sid;
  LocalFree(allocated_sid);

  DWORD name_size = 0;
  DWORD domain_size = 0;
  SID_NAME_USE sid_type{};
  LookupAccountSidW(nullptr, token_user->User.Sid, nullptr, &name_size,
                    nullptr, &domain_size, &sid_type);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || name_size == 0)
    return false;
  std::wstring name(name_size, L'\0');
  std::wstring domain(domain_size, L'\0');
  if (!LookupAccountSidW(nullptr, token_user->User.Sid, name.data(),
                         &name_size, domain.data(), &domain_size, &sid_type))
    return false;
  name.resize(name_size);
  domain.resize(domain_size);
  while (!name.empty() && name.back() == L'\0')
    name.pop_back();
  while (!domain.empty() && domain.back() == L'\0')
    domain.pop_back();
  if (name.empty())
    return false;
  *account = domain.empty() ? name : domain + L"\\" + name;
  if (account->find_first_of(L"\r\n=") != std::wstring::npos)
    return false;

  DWORD returned = 0;
  if (!GetTokenInformation(token, TokenSessionId, session_id,
                           sizeof(*session_id), &returned) ||
      *session_id == 0)
    return false;
  *resume_capable = TokenCanResumeElevated(token);
  return true;
}

bool WriteIdentityRecordCreateNew(const std::wstring &path,
                                  std::wstring_view pipe_id,
                                  std::wstring_view challenge,
                                  std::wstring_view sid,
                                  std::wstring_view account,
                                  DWORD session_id,
                                  bool resume_capable) {
  const std::wstring record =
      L"format=1\r\npipe_id=" + std::wstring(pipe_id) +
      L"\r\nchallenge=" + std::wstring(challenge) + L"\r\nsid=" +
      std::wstring(sid) + L"\r\nsession=" + std::to_wstring(session_id) +
      L"\r\naccount=" + std::wstring(account) + L"\r\nresume_capable=" +
      (resume_capable ? L"1\r\n" : L"0\r\n");
  const int utf8_size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, record.data(),
      static_cast<int>(record.size()), nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0)
    return false;
  std::string utf8(static_cast<size_t>(utf8_size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, record.data(),
                          static_cast<int>(record.size()), utf8.data(),
                          utf8_size, nullptr, nullptr) != utf8_size)
    return false;

  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const bool success =
      utf8.size() <= std::numeric_limits<DWORD>::max() &&
      WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                nullptr) &&
      written == utf8.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!success)
    DeleteFileW(path.c_str());
  return success;
}

DWORD CaptureOriginalUser(std::wstring_view pipe_id,
                          std::wstring_view challenge,
                          const std::wstring &record_path) {
  if (!IsHexNonce(pipe_id) || !IsHexNonce(challenge) ||
      pipe_id == challenge)
    return ERROR_INVALID_PARAMETER;

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  // The pipe is local and one-shot. Anonymous/network clients are denied;
  // the actual identity is taken from the impersonated token, never payload.
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(D;;GA;;;AN)(D;;GA;;;NU)(A;;GRGW;;;AU)(A;;GA;;;SY)"
          L"(A;;GA;;;BA)",
          SDDL_REVISION_1, &descriptor, nullptr))
    return GetLastError();
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.lpSecurityDescriptor = descriptor;
  const std::wstring pipe_name = IdentityPipeName(pipe_id);
  HANDLE pipe = CreateNamedPipeW(
      pipe_name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE |
          FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 0, 512, kIdentityPipeTimeoutMs, &security);
  LocalFree(descriptor);
  if (pipe == INVALID_HANDLE_VALUE)
    return GetLastError();

  const ULONGLONG deadline = GetTickCount64() + kIdentityPipeTimeoutMs;
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent) {
    const DWORD error = GetLastError();
    CloseHandle(pipe);
    return error;
  }
  BOOL connected = ConnectNamedPipe(pipe, &overlapped);
  DWORD error = connected ? ERROR_SUCCESS : GetLastError();
  if (!connected && error == ERROR_PIPE_CONNECTED) {
    error = ERROR_SUCCESS;
  } else if (!connected && error == ERROR_IO_PENDING) {
    DWORD transferred = 0;
    error = WaitForOverlappedUntil(pipe, &overlapped, deadline,
                                   &transferred);
  }
  CloseHandle(overlapped.hEvent);
  if (error != ERROR_SUCCESS) {
    CloseHandle(pipe);
    return error;
  }

  const std::wstring expected = L"FAMO_IDENTITY_PROOF_V1:" +
                                std::wstring(challenge);
  wchar_t message[96]{};
  DWORD bytes_read = 0;
  OVERLAPPED read_overlapped{};
  read_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!read_overlapped.hEvent) {
    error = GetLastError();
  } else {
    const BOOL read =
        ReadFile(pipe, message, sizeof(message) - sizeof(wchar_t), &bytes_read,
                 &read_overlapped);
    if (!read) {
      const DWORD read_error = GetLastError();
      error = read_error == ERROR_IO_PENDING
                  ? WaitForOverlappedUntil(pipe, &read_overlapped, deadline,
                                           &bytes_read)
                  : read_error;
    }
    CloseHandle(read_overlapped.hEvent);
    if (error == ERROR_SUCCESS &&
        (bytes_read % sizeof(wchar_t)) != 0) {
      error = ERROR_INVALID_DATA;
    } else if (error == ERROR_SUCCESS) {
      const std::wstring_view received(message,
                                       bytes_read / sizeof(wchar_t));
      if (received != expected)
        error = ERROR_INVALID_DATA;
    }
  }

  std::wstring sid;
  std::wstring account;
  DWORD client_session = 0;
  bool resume_capable = false;
  if (error == ERROR_SUCCESS && !ImpersonateNamedPipeClient(pipe))
    error = GetLastError();
  HANDLE client_token = nullptr;
  if (error == ERROR_SUCCESS &&
      !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE,
                       &client_token))
    error = GetLastError();
  if (error == ERROR_SUCCESS && !TokenIsMediumIntegrityDesktop(client_token))
    error = ERROR_ELEVATION_REQUIRED;
  if (error == ERROR_SUCCESS &&
      !TokenIdentity(client_token, &sid, &account, &client_session,
                     &resume_capable))
    error = GetLastError() == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                            : GetLastError();
  DWORD server_session = 0;
  if (error == ERROR_SUCCESS &&
      (!ProcessIdToSessionId(GetCurrentProcessId(), &server_session) ||
       server_session == 0 || server_session != client_session))
    error = ERROR_INVALID_OWNER;
  if (client_token)
    CloseHandle(client_token);
  RevertToSelf();

  if (error == ERROR_SUCCESS &&
      !WriteIdentityRecordCreateNew(record_path, pipe_id, challenge, sid, account,
                                    client_session, resume_capable))
    error = GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT
                                            : GetLastError();
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  return error;
}

DWORD ProveCurrentToken(std::wstring_view pipe_id,
                        std::wstring_view challenge) {
  if (!IsHexNonce(pipe_id) || !IsHexNonce(challenge) ||
      pipe_id == challenge)
    return ERROR_INVALID_PARAMETER;
  HANDLE current_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token))
    return GetLastError();
  const bool desktop_token = TokenIsMediumIntegrityDesktop(current_token);
  CloseHandle(current_token);
  if (!desktop_token)
    return ERROR_ELEVATION_REQUIRED;
  const std::wstring pipe_name = IdentityPipeName(pipe_id);
  const ULONGLONG deadline = GetTickCount64() + kIdentityPipeTimeoutMs;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  DWORD error = ERROR_FILE_NOT_FOUND;
  do {
    if (WaitNamedPipeW(pipe_name.c_str(), 250)) {
      pipe = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (pipe != INVALID_HANDLE_VALUE)
        break;
      error = GetLastError();
    } else {
      error = GetLastError();
    }
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
      return error;
    Sleep(25);
  } while (GetTickCount64() < deadline);
  if (pipe == INVALID_HANDLE_VALUE)
    return error == ERROR_FILE_NOT_FOUND ? ERROR_TIMEOUT : error;
  const std::wstring proof =
      L"FAMO_IDENTITY_PROOF_V1:" + std::wstring(challenge);
  DWORD written = 0;
  const DWORD bytes = static_cast<DWORD>(proof.size() * sizeof(wchar_t));
  const bool success =
      WriteFile(pipe, proof.data(), bytes, &written, nullptr) &&
      written == bytes && FlushFileBuffers(pipe);
  error = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(pipe);
  return error;
}

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

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  return path;
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

bool TokenMatchesSid(HANDLE token, std::wstring_view expected_sid) {
  PSID expected = nullptr;
  if (expected_sid.empty() ||
      !ConvertStringSidToSidW(std::wstring(expected_sid).c_str(), &expected))
    return false;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  const bool matches =
      size != 0 &&
      GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
      EqualSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid,
               expected);
  LocalFree(expected);
  return matches;
}

bool TokenIsMediumIntegrityDesktop(HANDLE token) {
  TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeFull;
  DWORD returned = 0;
  if (!token ||
      !GetTokenInformation(token, TokenElevationType, &elevation,
                           sizeof(elevation), &returned) ||
      (elevation != TokenElevationTypeDefault &&
       elevation != TokenElevationTypeLimited))
    return false;

  DWORD integrity_size = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &integrity_size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || integrity_size == 0)
    return false;
  std::vector<BYTE> integrity_buffer(integrity_size);
  if (!GetTokenInformation(token, TokenIntegrityLevel,
                           integrity_buffer.data(), integrity_size,
                           &integrity_size))
    return false;
  const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(
      integrity_buffer.data());
  if (!IsValidSid(label->Label.Sid))
    return false;
  const DWORD subauthority_count =
      *GetSidSubAuthorityCount(label->Label.Sid);
  if (subauthority_count == 0)
    return false;
  const DWORD integrity =
      *GetSidSubAuthority(label->Label.Sid, subauthority_count - 1);
  // Desktop TSF/runtime work is user data work. Low tokens cannot perform it,
  // while High/System tokens would reintroduce the scheduled-task elevation
  // bug this broker exists to prevent.
  return integrity >= SECURITY_MANDATORY_MEDIUM_RID &&
         integrity < SECURITY_MANDATORY_HIGH_RID;
}

bool CurrentProcessTokenMatchesSid(std::wstring_view expected_sid) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  const bool matches = TokenMatchesSid(token, expected_sid);
  CloseHandle(token);
  return matches;
}

HRESULT CurrentProcessCanonicalSid(std::wstring *sid) {
  sid->clear();
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return HRESULT_FROM_WIN32(GetLastError());
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  DWORD error = GetLastError();
  if (error != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    CloseHandle(token);
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                    : error);
  }
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    error = GetLastError();
    CloseHandle(token);
    return HRESULT_FROM_WIN32(error);
  }
  CloseHandle(token);
  const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
  if (!IsValidSid(user->User.Sid))
    return HRESULT_FROM_WIN32(ERROR_INVALID_SID);
  LPWSTR allocated = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &allocated))
    return HRESULT_FROM_WIN32(GetLastError());
  *sid = allocated;
  LocalFree(allocated);
  if (sid->empty() ||
      sid->find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             L"abcdefghijklmnopqrstuvwxyz"
                             L"0123456789-_") != std::wstring::npos) {
    sid->clear();
    return HRESULT_FROM_WIN32(ERROR_INVALID_SID);
  }
  return S_OK;
}

HRESULT RunAsDesktopUser(const std::wstring &executable,
                         const std::wstring &arguments, bool wait,
                         std::wstring_view expected_sid = {},
                         DWORD *child_exit_code = nullptr) {
  constexpr DWORD kShellTokenTimeoutMs = 15000;
  constexpr DWORD kShellRetryMs = 100;
  constexpr DWORD kChildTimeoutMs = 120000;
  if (child_exit_code)
    *child_exit_code = STILL_ACTIVE;
  const HRESULT privilege = EnablePrivilege(L"SeImpersonatePrivilege");
  if (FAILED(privilege))
    return privilege;

  HANDLE shell_process = nullptr;
  HANDLE shell_token = nullptr;
  const ULONGLONG deadline = GetTickCount64() + kShellTokenTimeoutMs;
  DWORD shell_error = ERROR_NOT_FOUND;
  do {
    const HWND shell = GetShellWindow();
    DWORD shell_process_id = 0;
    if (shell)
      GetWindowThreadProcessId(shell, &shell_process_id);
    if (shell_process_id != 0) {
      shell_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  shell_process_id);
      if (shell_process &&
          OpenProcessToken(shell_process, TOKEN_QUERY | TOKEN_DUPLICATE,
                           &shell_token)) {
        shell_error = ERROR_SUCCESS;
        break;
      }
      shell_error = GetLastError();
      if (shell_process) {
        CloseHandle(shell_process);
        shell_process = nullptr;
      }
    }
    if (GetTickCount64() >= deadline)
      break;
    Sleep(kShellRetryMs);
  } while (true);
  if (!shell_token)
    return HRESULT_FROM_WIN32(shell_error == ERROR_SUCCESS ? ERROR_NOT_FOUND
                                                            : shell_error);
  if (!TokenIsMediumIntegrityDesktop(shell_token)) {
    CloseHandle(shell_token);
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
  }
  if (!expected_sid.empty() && !TokenMatchesSid(shell_token, expected_sid)) {
    CloseHandle(shell_token);
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
  }
  HANDLE primary_token = nullptr;
  if (!DuplicateTokenEx(shell_token, MAXIMUM_ALLOWED, nullptr,
                        SecurityImpersonation, TokenPrimary, &primary_token)) {
    const DWORD error = GetLastError();
    CloseHandle(shell_token);
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(error);
  }
  if (!TokenIsMediumIntegrityDesktop(primary_token) ||
      (!expected_sid.empty() &&
       !TokenMatchesSid(primary_token, expected_sid))) {
    CloseHandle(primary_token);
    CloseHandle(shell_token);
    CloseHandle(shell_process);
    return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
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
      command_line.data(), CREATE_SUSPENDED, nullptr, ModuleDirectory().c_str(),
      &startup, &process);
  DWORD error = created ? ERROR_SUCCESS : GetLastError();
  if (created) {
    HANDLE created_token = nullptr;
    if (!OpenProcessToken(process.hProcess, TOKEN_QUERY, &created_token)) {
      error = GetLastError();
    } else {
      const bool valid_child =
          TokenIsMediumIntegrityDesktop(created_token) &&
          (expected_sid.empty() ||
           TokenMatchesSid(created_token, expected_sid));
      CloseHandle(created_token);
      if (!valid_child)
        error = ERROR_ELEVATION_REQUIRED;
    }
    if (error != ERROR_SUCCESS) {
      TerminateProcess(process.hProcess, error);
      WaitForSingleObject(process.hProcess, 5000);
    } else if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      error = GetLastError();
      TerminateProcess(process.hProcess, error);
      WaitForSingleObject(process.hProcess, 5000);
    } else if (wait) {
      const DWORD waited =
          WaitForSingleObject(process.hProcess, kChildTimeoutMs);
      DWORD exit_code = 1;
      if (waited == WAIT_TIMEOUT) {
        if (!TerminateProcess(process.hProcess, ERROR_TIMEOUT))
          error = GetLastError();
        else {
          WaitForSingleObject(process.hProcess, 5000);
          error = ERROR_TIMEOUT;
        }
      } else if (waited != WAIT_OBJECT_0)
        error = GetLastError();
      else if (!GetExitCodeProcess(process.hProcess, &exit_code))
        error = GetLastError();
      else {
        if (child_exit_code)
          *child_exit_code = exit_code;
      }
      if (error == ERROR_SUCCESS && exit_code != 0)
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

bool ExecutableLeafMatches(const std::wstring &path,
                           std::wstring_view expected_leaf) {
  const size_t separator = path.find_last_of(L"\\/");
  const std::wstring_view leaf =
      separator == std::wstring::npos
          ? std::wstring_view(path)
          : std::wstring_view(path).substr(separator + 1);
  return _wcsicmp(std::wstring(leaf).c_str(),
                  std::wstring(expected_leaf).c_str()) == 0;
}

bool ResolveDesktopOperation(std::wstring_view kind,
                             std::wstring_view operation,
                             std::wstring_view sid,
                             const std::wstring &executable,
                             std::wstring *arguments) {
  if (kind == L"profile" &&
      ExecutableLeafMatches(executable, L"FamoProfileTool.exe")) {
    if (operation == L"clear-user-com-shadow") {
      *arguments = L"clear-user-com-shadow " + std::wstring(sid);
      return true;
    }
    if (operation == L"check" || operation == L"check-disabled" ||
        operation == L"is-active" || operation == L"is-enabled" ||
        operation == L"switch-away" || operation == L"enable" ||
        operation == L"disable" || operation == L"activate" ||
        operation == L"cleanup-user-state") {
      *arguments = operation;
      return true;
    }
  } else if (kind == L"settings" &&
             ExecutableLeafMatches(executable, L"FamoSettings.exe")) {
    if (operation == L"seed") {
      *arguments = L"--seed-only";
      return true;
    }
    if (operation == L"seed-no-activate") {
      *arguments = L"--seed-only --no-activate";
      return true;
    }
    if (operation == L"add-input-tip") {
      *arguments = L"--add-input-tip";
      return true;
    }
    if (operation == L"remove-input-tip") {
      *arguments = L"--remove-input-tip";
      return true;
    }
    if (operation == L"is-input-tip") {
      *arguments = L"--is-input-tip";
      return true;
    }
    constexpr std::wstring_view kPreparePrefix =
        L"prepare-seed-transaction-";
    constexpr std::wstring_view kApplyPrefix = L"apply-seed-transaction-";
    constexpr std::wstring_view kApplyNoActivatePrefix =
        L"apply-seed-transaction-no-activate-";
    constexpr std::wstring_view kRollbackPrefix =
        L"rollback-seed-transaction-";
    constexpr std::wstring_view kCommitPrefix =
        L"commit-seed-transaction-";
    constexpr std::wstring_view kDiscardPrefix =
        L"discard-seed-transaction-";
    if (operation.starts_with(kPreparePrefix)) {
      const std::wstring_view id = operation.substr(kPreparePrefix.size());
      if (IsLowerHexTransactionId(id)) {
        *arguments = L"--prepare-seed-transaction " + std::wstring(id);
        return true;
      }
    } else if (operation.starts_with(kDiscardPrefix)) {
      const std::wstring_view id = operation.substr(kDiscardPrefix.size());
      if (IsLowerHexTransactionId(id)) {
        *arguments = L"--discard-seed-transaction " + std::wstring(id);
        return true;
      }
    } else {
      bool no_activate = false;
      std::wstring_view prefix;
      std::wstring_view switch_name;
      if (operation.starts_with(kApplyNoActivatePrefix)) {
        prefix = kApplyNoActivatePrefix;
        switch_name = L"--apply-seed-transaction ";
        no_activate = true;
      } else if (operation.starts_with(kApplyPrefix)) {
        prefix = kApplyPrefix;
        switch_name = L"--apply-seed-transaction ";
      } else if (operation.starts_with(kRollbackPrefix)) {
        prefix = kRollbackPrefix;
        switch_name = L"--rollback-seed-transaction ";
      } else if (operation.starts_with(kCommitPrefix)) {
        prefix = kCommitPrefix;
        switch_name = L"--commit-seed-transaction ";
      }
      if (!prefix.empty()) {
        const std::wstring_view identity = operation.substr(prefix.size());
        if (identity.size() == 97 && identity[32] == L'-') {
          const std::wstring_view id = identity.substr(0, 32);
          const std::wstring_view hash = identity.substr(33);
          if (IsLowerHexTransactionId(id) && IsUpperHexHash(hash)) {
            *arguments = std::wstring(switch_name) + std::wstring(id) + L" " +
                         std::wstring(hash);
            if (no_activate)
              *arguments += L" --no-activate";
            return true;
          }
        }
      }
    }
  } else if (kind == L"runtime" &&
             ExecutableLeafMatches(executable, L"FamoRuntime.exe")) {
    if (operation == L"start") {
      arguments->clear();
      return true;
    }
    if (operation == L"shutdown") {
      *arguments = L"--control shutdown";
      return true;
    }
    if (operation == L"quit") {
      *arguments = L"/quit";
      return true;
    }
    if (operation == L"deploy") {
      *arguments = L"--control deploy";
      return true;
    }
    if (operation == L"reload-options") {
      *arguments = L"--control reload-options";
      return true;
    }
  }
  return false;
}

int RunBoundDesktopOperation(std::wstring_view sid, std::wstring_view wait_mode,
                             std::wstring_view kind,
                             std::wstring_view operation,
                             const std::wstring &executable) {
  const bool wait = wait_mode == L"wait";
  if ((!wait && wait_mode != L"nowait") ||
      (operation != L"start" && !wait))
    return 2;
  std::wstring arguments;
  if (!ResolveDesktopOperation(kind, operation, sid, executable, &arguments))
    return 2;
  DWORD child_exit_code = STILL_ACTIVE;
  const HRESULT result =
      RunAsDesktopUser(executable, arguments, wait, sid, &child_exit_code);
  if (wait && child_exit_code != STILL_ACTIVE)
    return static_cast<int>(child_exit_code);
  if (FAILED(result)) {
    std::fwprintf(stderr, L"bound desktop operation failed: 0x%08lx\n",
                  static_cast<unsigned long>(result));
    return 1;
  }
  return 0;
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

HRESULT CleanupDesktopUser(std::wstring_view expected_sid = {}) {
  const std::wstring directory = ModuleDirectory();
  if (directory.empty())
    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);

  const std::wstring runtime = directory + L"\\FamoRuntime.exe";
  const HRESULT shutdown =
      RunAsDesktopUser(runtime, L"--control shutdown", true, expected_sid);
  const HRESULT runtime_absent = WaitForExecutableExit(runtime);
  if (FAILED(runtime_absent))
    return FAILED(shutdown) ? shutdown : runtime_absent;
  HRESULT result = RunAsDesktopUser(
      directory + L"\\FamoProfileTool.exe", L"cleanup-user-state", true,
      expected_sid);
  if (FAILED(result))
    return result;
  return RunAsDesktopUser(directory + L"\\settings\\FamoSettings.exe",
                          L"--remove-input-tip", true, expected_sid);
}

std::wstring FinalPathForHandle(HANDLE handle);
HRESULT ValidatePinnedDirectory(HANDLE directory,
                                const std::wstring &expected_path,
                                std::wstring *final_path);
HRESULT OpenOrCreateRelativeLockFile(HANDLE parent, std::wstring_view leaf,
                                     const std::wstring &expected_path,
                                     HANDLE *opened);
using UserDataLockDirectoryValidationHook =
    void (*)(const std::wstring &);
UserDataLockDirectoryValidationHook
    g_user_data_lock_directory_validation_hook = nullptr;

class UserDataTransactionLease {
 public:
  UserDataTransactionLease() = default;
  UserDataTransactionLease(const UserDataTransactionLease &) = delete;
  UserDataTransactionLease &operator=(const UserDataTransactionLease &) =
      delete;
  ~UserDataTransactionLease() { Release(); }

  HRESULT Acquire(const std::wstring &local_data,
                  std::wstring_view canonical_sid) {
    if (mutex_ || file_ != INVALID_HANDLE_VALUE)
      return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (local_data.empty() || canonical_sid.empty() ||
        canonical_sid.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        L"abcdefghijklmnopqrstuvwxyz"
                                        L"0123456789-_") !=
            std::wstring_view::npos)
      return E_INVALIDARG;

    constexpr ULONGLONG kTimeoutMs = 30000;
    constexpr DWORD kRetryMs = 25;
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    const std::wstring mutex_name =
        L"Global\\Famo.Settings.UserData.Transaction." +
        std::wstring(canonical_sid);
    mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex_) {
      const ULONGLONG now = GetTickCount64();
      const DWORD remaining = now >= deadline
                                  ? 0
                                  : static_cast<DWORD>(deadline - now);
      const DWORD waited = WaitForSingleObject(mutex_, remaining);
      if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        mutex_owned_ = true;
      } else {
        const DWORD error = waited == WAIT_TIMEOUT
                                ? ERROR_TIMEOUT
                                : (waited == WAIT_FAILED ? GetLastError()
                                                        : ERROR_GEN_FAILURE);
        Release();
        return HRESULT_FROM_WIN32(error);
      }
    } else {
      const DWORD error = GetLastError();
      if (error != ERROR_ACCESS_DENIED && error != ERROR_INVALID_HANDLE)
        return HRESULT_FROM_WIN32(error);
      // Match the managed writer: if the Global object namespace is
      // unavailable, the stable per-SID file lock remains mandatory.
    }

    std::wstring lock_directory = local_data;
    while (!lock_directory.empty() &&
           (lock_directory.back() == L'\\' ||
            lock_directory.back() == L'/'))
      lock_directory.pop_back();
    lock_directory += L"\\Famo.UserDataLocks";
    if (!CreateDirectoryW(lock_directory.c_str(), nullptr)) {
      const DWORD error = GetLastError();
      if (error != ERROR_ALREADY_EXISTS) {
        Release();
        return HRESULT_FROM_WIN32(error);
      }
    }
    lock_directory_ = CreateFileW(
        lock_directory.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock_directory_ == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      Release();
      return HRESULT_FROM_WIN32(error);
    }
    std::wstring lock_directory_final;
    HRESULT directory_result = ValidatePinnedDirectory(
        lock_directory_, lock_directory, &lock_directory_final);
    if (FAILED(directory_result)) {
      Release();
      return directory_result;
    }
    if (g_user_data_lock_directory_validation_hook)
      g_user_data_lock_directory_validation_hook(lock_directory);

    const std::wstring lock_path =
        lock_directory + L"\\" + std::wstring(canonical_sid) +
        L".transaction.lock";
    while (true) {
      const HRESULT opened = OpenOrCreateRelativeLockFile(
          lock_directory_, std::wstring(canonical_sid) +
                               L".transaction.lock",
          lock_path, &file_);
      if (SUCCEEDED(opened))
        return S_OK;
      const DWORD error = HRESULT_CODE(opened);
      if (error != ERROR_SHARING_VIOLATION &&
          error != ERROR_LOCK_VIOLATION) {
        Release();
        return opened;
      }
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
        Release();
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      const DWORD remaining = static_cast<DWORD>(deadline - now);
      Sleep(remaining < kRetryMs ? remaining : kRetryMs);
    }
  }

  void Release() {
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
      file_ = INVALID_HANDLE_VALUE;
    }
    if (lock_directory_ != INVALID_HANDLE_VALUE) {
      CloseHandle(lock_directory_);
      lock_directory_ = INVALID_HANDLE_VALUE;
    }
    if (mutex_) {
      if (mutex_owned_)
        ReleaseMutex(mutex_);
      CloseHandle(mutex_);
      mutex_ = nullptr;
      mutex_owned_ = false;
    }
  }

 private:
  HANDLE mutex_ = nullptr;
  HANDLE lock_directory_ = INVALID_HANDLE_VALUE;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  bool mutex_owned_ = false;
};

HRESULT AcquireUserDataTransactionLock(
    const std::wstring &local_data, std::wstring_view canonical_sid,
    UserDataTransactionLease *lease) {
  if (!lease)
    return E_POINTER;
  return lease->Acquire(local_data, canonical_sid);
}

std::wstring FinalPathForHandle(HANDLE handle) {
  std::wstring path(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle, path.data(), static_cast<DWORD>(path.size()),
      FILE_NAME_NORMALIZED);
  if (length == 0 || length >= path.size())
    return {};
  path.resize(length);
  if (path.starts_with(L"\\\\?\\UNC\\"))
    path = L"\\\\" + path.substr(8);
  else if (path.starts_with(L"\\\\?\\"))
    path.erase(0, 4);
  while (path.size() > 3 &&
         (path.back() == L'\\' || path.back() == L'/'))
    path.pop_back();
  return path;
}

bool FinalPathIsContained(const std::wstring &root,
                          const std::wstring &path) {
  return _wcsicmp(root.c_str(), path.c_str()) == 0 ||
         (path.size() > root.size() &&
          path[root.size()] == L'\\' &&
          _wcsnicmp(root.c_str(), path.c_str(), root.size()) == 0);
}

HRESULT MarkObjectForDeletion(HANDLE handle) {
  struct DispositionInformationEx {
    DWORD flags;
  };
  constexpr DWORD kFileDispositionFlagDelete = 0x00000001;
  constexpr DWORD kFileDispositionFlagIgnoreReadonly = 0x00000010;
  DispositionInformationEx extended{
      kFileDispositionFlagDelete |
      kFileDispositionFlagIgnoreReadonly};
  if (SetFileInformationByHandle(
          handle, FileDispositionInfoEx, &extended, sizeof(extended)))
    return S_OK;

  const DWORD extended_error = GetLastError();
  if (extended_error != ERROR_INVALID_PARAMETER &&
      extended_error != ERROR_INVALID_FUNCTION &&
      extended_error != ERROR_NOT_SUPPORTED &&
      extended_error != ERROR_ACCESS_DENIED)
    return HRESULT_FROM_WIN32(extended_error);

  // Older systems do not support IGNORE_READONLY. Clear the attribute through
  // this already validated and pinned handle, never by reopening its path.
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)))
    return HRESULT_FROM_WIN32(GetLastError());
  if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
    basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    if (!SetFileInformationByHandle(
            handle, FileBasicInfo, &basic, sizeof(basic)))
      return HRESULT_FROM_WIN32(GetLastError());
  }
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  return SetFileInformationByHandle(handle, FileDispositionInfo,
                                    &disposition, sizeof(disposition))
             ? S_OK
             : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT OpenRelativeNoDeleteShare(HANDLE parent, std::wstring_view leaf,
                                  ACCESS_MASK access, HANDLE *opened) {
  *opened = INVALID_HANDLE_VALUE;
  if (leaf.empty() || leaf.size() > USHRT_MAX / sizeof(wchar_t) ||
      leaf.find_first_of(L"\\/") != std::wstring_view::npos ||
      leaf == L"." || leaf == L"..")
    return E_INVALIDARG;
  UNICODE_STRING name{};
  name.Buffer = const_cast<PWSTR>(leaf.data());
  name.Length =
      static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE,
                             parent, nullptr);
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = NtOpenFile(
      opened, access | SYNCHRONIZE, &attributes, &status_block,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT);
  if (status < 0) {
    *opened = INVALID_HANDLE_VALUE;
    return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
  }
  return S_OK;
}

HRESULT OpenOrCreateRelativeLockFile(
    HANDLE parent, std::wstring_view leaf,
    const std::wstring &expected_path, HANDLE *opened) {
  *opened = INVALID_HANDLE_VALUE;
  if (leaf.empty() || leaf.size() > USHRT_MAX / sizeof(wchar_t) ||
      leaf.find_first_of(L"\\/") != std::wstring_view::npos ||
      leaf == L"." || leaf == L"..")
    return E_INVALIDARG;
  UNICODE_STRING name{};
  name.Buffer = const_cast<PWSTR>(leaf.data());
  name.Length =
      static_cast<USHORT>(leaf.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE,
                             parent, nullptr);
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = NtCreateFile(
      opened,
      FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      &attributes, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, 0,
      FILE_OPEN_IF,
      FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT |
          FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);
  if (status < 0) {
    *opened = INVALID_HANDLE_VALUE;
    return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
  }
  BY_HANDLE_FILE_INFORMATION information{};
  const std::wstring final_path = FinalPathForHandle(*opened);
  if (!GetFileInformationByHandle(*opened, &information)) {
    const DWORD error = GetLastError();
    CloseHandle(*opened);
    *opened = INVALID_HANDLE_VALUE;
    return HRESULT_FROM_WIN32(error);
  }
  const ULONGLONG object_id =
      (static_cast<ULONGLONG>(information.nFileIndexHigh) << 32) |
      information.nFileIndexLow;
  if ((information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      object_id == 0 || final_path.empty() ||
      _wcsicmp(final_path.c_str(), expected_path.c_str()) != 0) {
    CloseHandle(*opened);
    *opened = INVALID_HANDLE_VALUE;
    return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  }
  return S_OK;
}

HRESULT ValidatePinnedDirectory(HANDLE directory,
                                const std::wstring &expected_path,
                                std::wstring *final_path) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(directory, &information))
    return HRESULT_FROM_WIN32(GetLastError());
  if ((information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
      FILE_ATTRIBUTE_DIRECTORY)
    return HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID);
  *final_path = FinalPathForHandle(directory);
  if (final_path->empty() ||
      _wcsicmp(final_path->c_str(), expected_path.c_str()) != 0)
    return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  return S_OK;
}

HRESULT DeletePinnedTreeObject(const std::wstring &path, HANDLE object,
                               const std::wstring &allowed_root) {
  BY_HANDLE_FILE_INFORMATION information{};
  HRESULT result = GetFileInformationByHandle(object, &information)
                       ? S_OK
                       : HRESULT_FROM_WIN32(GetLastError());
  if (SUCCEEDED(result) &&
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    result = HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID);
  const std::wstring final_path =
      SUCCEEDED(result) ? FinalPathForHandle(object) : L"";
  if (SUCCEEDED(result) &&
      (final_path.empty() ||
       !FinalPathIsContained(allowed_root, final_path)))
    result = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  const bool directory =
      (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (SUCCEEDED(result) && directory) {
    std::vector<std::wstring> children;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND)
        result = HRESULT_FROM_WIN32(error);
    } else {
      do {
        const std::wstring_view name(found.cFileName);
        if (name != L"." && name != L".." &&
            name.find_first_of(L"\\/") == std::wstring_view::npos)
          children.emplace_back(path + L"\\" + std::wstring(name));
      } while (FindNextFileW(search, &found));
      const DWORD error = GetLastError();
      FindClose(search);
      if (error != ERROR_NO_MORE_FILES)
        result = HRESULT_FROM_WIN32(error);
    }
    for (const std::wstring &child : children) {
      if (FAILED(result))
        break;
      HANDLE child_object = INVALID_HANDLE_VALUE;
      result = OpenRelativeNoDeleteShare(
          object, child.substr(path.size() + 1),
          DELETE | FILE_READ_ATTRIBUTES | FILE_READ_DATA |
              FILE_WRITE_ATTRIBUTES,
          &child_object);
      if (SUCCEEDED(result)) {
        result = DeletePinnedTreeObject(
            child, child_object, allowed_root);
        CloseHandle(child_object);
      }
    }
  }
  if (SUCCEEDED(result))
    result = MarkObjectForDeletion(object);
  return result;
}

using UserDataDeleteValidationHook = void (*)(const std::wstring &);
UserDataDeleteValidationHook g_user_data_delete_validation_hook = nullptr;

HRESULT DeletePinnedDirectoryChild(const std::wstring &parent_path,
                                   std::wstring_view child_leaf) {
  std::wstring full_parent(32768, L'\0');
  const DWORD parent_length = GetFullPathNameW(
      parent_path.c_str(), static_cast<DWORD>(full_parent.size()),
      full_parent.data(), nullptr);
  if (parent_length == 0 || parent_length >= full_parent.size())
    return HRESULT_FROM_WIN32(GetLastError());
  full_parent.resize(parent_length);
  while (full_parent.size() > 3 && full_parent.back() == L'\\')
    full_parent.pop_back();

  wchar_t volume_buffer[32768]{};
  if (!GetVolumePathNameW(full_parent.c_str(), volume_buffer,
                          ARRAYSIZE(volume_buffer)))
    return HRESULT_FROM_WIN32(GetLastError());
  std::wstring volume(volume_buffer);
  HANDLE volume_handle = CreateFileW(
      volume.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (volume_handle == INVALID_HANDLE_VALUE)
    return HRESULT_FROM_WIN32(GetLastError());
  std::vector<HANDLE> chain{volume_handle};
  HRESULT result = S_OK;
  std::wstring expected_volume = volume;
  while (expected_volume.size() > 3 && expected_volume.back() == L'\\')
    expected_volume.pop_back();
  std::wstring volume_final;
  result = ValidatePinnedDirectory(
      volume_handle, expected_volume, &volume_final);

  size_t offset = volume.size();
  while (SUCCEEDED(result) && offset < full_parent.size()) {
    while (offset < full_parent.size() &&
           (full_parent[offset] == L'\\' ||
            full_parent[offset] == L'/'))
      ++offset;
    if (offset >= full_parent.size())
      break;
    const size_t separator = full_parent.find_first_of(L"\\/", offset);
    const size_t end = separator == std::wstring::npos
                           ? full_parent.size()
                           : separator;
    const std::wstring_view component(
        full_parent.data() + offset, end - offset);
    HANDLE next = INVALID_HANDLE_VALUE;
    result = OpenRelativeNoDeleteShare(
        chain.back(), component,
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE, &next);
    if (FAILED(result))
      break;
    chain.push_back(next);
    std::wstring expected =
        full_parent.substr(0, end);
    std::wstring actual;
    result = ValidatePinnedDirectory(next, expected, &actual);
    offset = end;
  }

  HANDLE target = INVALID_HANDLE_VALUE;
  const std::wstring target_path =
      full_parent + L"\\" + std::wstring(child_leaf);
  if (SUCCEEDED(result)) {
    result = OpenRelativeNoDeleteShare(
        chain.back(), child_leaf,
        DELETE | FILE_READ_ATTRIBUTES | FILE_READ_DATA |
            FILE_WRITE_ATTRIBUTES,
        &target);
    if (HRESULT_CODE(result) == ERROR_FILE_NOT_FOUND ||
        HRESULT_CODE(result) == ERROR_PATH_NOT_FOUND)
      result = S_FALSE;
  }
  if (SUCCEEDED(result) && result != S_FALSE) {
    BY_HANDLE_FILE_INFORMATION target_information{};
    const std::wstring target_final = FinalPathForHandle(target);
    if (!GetFileInformationByHandle(target, &target_information)) {
      result = HRESULT_FROM_WIN32(GetLastError());
    } else if ((target_information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY |
                 FILE_ATTRIBUTE_REPARSE_POINT)) !=
                   FILE_ATTRIBUTE_DIRECTORY ||
               target_final.empty() ||
               _wcsicmp(target_final.c_str(), target_path.c_str()) != 0) {
      result = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }
    if (SUCCEEDED(result) && g_user_data_delete_validation_hook)
      g_user_data_delete_validation_hook(target_path);
    if (SUCCEEDED(result))
      result = DeletePinnedTreeObject(
          target_path, target, target_path);
  }
  if (target != INVALID_HANDLE_VALUE)
    CloseHandle(target);
  for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator)
    CloseHandle(*iterator);
  return result == S_FALSE ? S_OK : result;
}

HRESULT DeleteCurrentUserData(std::wstring_view expected_sid) {
  if (!CurrentProcessTokenMatchesSid(expected_sid))
    return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);

  PWSTR allocated_local_data = nullptr;
  const HRESULT known_folder = SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
      &allocated_local_data);
  if (FAILED(known_folder))
    return known_folder;
  const std::wstring local_data(allocated_local_data);
  CoTaskMemFree(allocated_local_data);
  std::wstring canonical_sid;
  HRESULT result = CurrentProcessCanonicalSid(&canonical_sid);
  if (FAILED(result))
    return result;
  UserDataTransactionLease transaction_lock;
  result = AcquireUserDataTransactionLock(
      local_data, canonical_sid, &transaction_lock);
  if (FAILED(result))
    return result;
  return DeletePinnedDirectoryChild(local_data, L"Famo");
}

HRESULT DeleteDesktopUserData(std::wstring_view expected_sid) {
  const std::wstring executable = ModulePath();
  if (executable.empty())
    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  return RunAsDesktopUser(
      executable,
      L"delete-user-data-current " + std::wstring(expected_sid),
      true, expected_sid);
}

HRESULT ClearCurrentUserComShadow(std::wstring_view expected_sid) {
  if (!CurrentProcessTokenMatchesSid(expected_sid))
    return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
  const std::wstring com =
      L"Software\\Classes\\CLSID\\" + TextServiceGuidText();
  const LSTATUS removed = RegDeleteTreeW(HKEY_CURRENT_USER, com.c_str());
  if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
    return HRESULT_FROM_WIN32(removed);
  return !UserKeyPresent(com) ? S_OK : E_FAIL;
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

bool MachineRegistrationPresent() {
  return RegistryPresentAt(HKEY_LOCAL_MACHINE) && ProfileRegistered() &&
         KeyboardCategoryRegistered();
}

void WaitForMachineRegistrationVisibility() {
  constexpr DWORD kPollIntervalMs = 50;
  constexpr DWORD kMaxWaitMs = 2000;
  for (DWORD waited = 0; waited < kMaxWaitMs; waited += kPollIntervalMs) {
    if (MachineRegistrationPresent())
      return;
    Sleep(kPollIntervalMs);
  }
}

HRESULT UnregisterMachineWithoutLoadingServiceDll() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    return initialized;

  const GUID kProfileCategories[] = {
      GUID_TFCAT_TIP_KEYBOARD,
      GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
      GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
      GUID_TFCAT_TIPCAP_SECUREMODE,
      GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
      GUID_TFCAT_TIPCAP_COMLESS,
      GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
  };
  ITfCategoryMgr *categories = nullptr;
  if (SUCCEEDED(CoCreateInstance(
          CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
          IID_ITfCategoryMgr, reinterpret_cast<void **>(&categories)))) {
    for (const GUID &category : kProfileCategories) {
      categories->UnregisterCategory(famo::tsf::kTextServiceClsid, category,
                                     famo::tsf::kTextServiceClsid);
    }
    categories->Release();
  }

  ITfInputProcessorProfiles *profiles = nullptr;
  if (SUCCEEDED(CoCreateInstance(
          CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
          IID_ITfInputProcessorProfiles,
          reinterpret_cast<void **>(&profiles)))) {
    profiles->RemoveLanguageProfile(famo::tsf::kTextServiceClsid,
                                    famo::tsf::kLanguageId,
                                    famo::tsf::kLanguageProfileGuid);
    profiles->Unregister(famo::tsf::kTextServiceClsid);
    profiles->Release();
  }

  const std::wstring guid = TextServiceGuidText();
  const std::wstring com = L"Software\\Classes\\CLSID\\" + guid;
  const std::wstring tip = L"Software\\Microsoft\\CTF\\TIP\\" + guid;
  const LSTATUS com_removed =
      RegDeleteTreeW(HKEY_LOCAL_MACHINE, com.c_str());
  const LSTATUS tip_removed =
      RegDeleteTreeW(HKEY_LOCAL_MACHINE, tip.c_str());
  if (owns_com)
    CoUninitialize();
  if ((com_removed != ERROR_SUCCESS &&
       com_removed != ERROR_FILE_NOT_FOUND) ||
      (tip_removed != ERROR_SUCCESS &&
       tip_removed != ERROR_FILE_NOT_FOUND))
    return HRESULT_FROM_WIN32(com_removed != ERROR_SUCCESS &&
                                     com_removed != ERROR_FILE_NOT_FOUND
                                 ? com_removed
                                 : tip_removed);
  WaitForMachineRegistrationRemoval();
  return !RegistryPresentAt(HKEY_LOCAL_MACHINE) && !ProfileRegistered() &&
                 !KeyboardCategoryRegistered()
             ? S_OK
             : E_FAIL;
}

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

HRESULT WaitForExecutableExit(const std::wstring &path) {
  constexpr DWORD kShutdownTimeoutMs = 10000;
  constexpr DWORD kPollIntervalMs = 100;
  const ULONGLONG deadline = GetTickCount64() + kShutdownTimeoutMs;
  while (true) {
    DWORD error = ERROR_SUCCESS;
    const LoadedState state = IsFileLoaded(path.c_str(), &error);
    if (state == LoadedState::NotLoaded)
      return S_OK;
    if (state == LoadedState::Error)
      return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                       : error);
    if (GetTickCount64() >= deadline)
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    Sleep(kPollIntervalMs);
  }
}

} // namespace

#ifndef FAMO_PROFILE_TOOL_NO_MAIN
int wmain(int argc, wchar_t **argv) {
  if (argc == 5 &&
      std::wstring_view(argv[1]) == L"capture-original-user") {
    const DWORD error = CaptureOriginalUser(argv[2], argv[3], argv[4]);
    if (error != ERROR_SUCCESS) {
      std::fwprintf(stderr, L"original-user capture failed: %lu\n", error);
      return 1;
    }
    std::wprintf(L"original_user_captured=yes\n");
    return 0;
  }
  if (argc == 6 &&
      std::wstring_view(argv[1]) == L"capture-original-user-for") {
    if (!CurrentProcessTokenMatchesSid(argv[2])) {
      std::fwprintf(stderr, L"recovery process SID mismatch\n");
      return 1;
    }
    const DWORD error = CaptureOriginalUser(argv[3], argv[4], argv[5]);
    if (error != ERROR_SUCCESS) {
      std::fwprintf(stderr, L"bound original-user capture failed: %lu\n",
                    error);
      return 1;
    }
    std::wprintf(L"original_user_captured=yes sid=%ls\n", argv[2]);
    return 0;
  }
  if (argc == 4 &&
      std::wstring_view(argv[1]) == L"prove-current-token") {
    const DWORD error = ProveCurrentToken(argv[2], argv[3]);
    if (error != ERROR_SUCCESS) {
      std::fwprintf(stderr, L"current-token proof failed: %lu\n", error);
      return 1;
    }
    return 0;
  }
  if ((argc == 4 &&
       std::wstring_view(argv[1]) == L"prove-shell-token") ||
      (argc == 5 &&
       std::wstring_view(argv[1]) == L"prove-shell-token-for")) {
    const bool bound = argc == 5;
    const std::wstring_view sid = bound ? argv[2] : L"";
    const wchar_t *pipe_id = argv[bound ? 3 : 2];
    const wchar_t *challenge = argv[bound ? 4 : 3];
    const std::wstring executable = ModulePath();
    if (executable.empty())
      return 1;
    DWORD child_exit_code = STILL_ACTIVE;
    const HRESULT result = RunAsDesktopUser(
        executable,
        L"prove-current-token " + std::wstring(pipe_id) + L" " + challenge,
        true, sid, &child_exit_code);
    if (child_exit_code != STILL_ACTIVE)
      return static_cast<int>(child_exit_code);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"shell-token proof launch failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    return 0;
  }
  if (argc == 7 &&
      std::wstring_view(argv[1]) == L"desktop-run-for") {
    return RunBoundDesktopOperation(argv[2], argv[3], argv[4], argv[5],
                                    argv[6]);
  }
  if (argc == 3 &&
      std::wstring_view(argv[1]) == L"start-runtime-for") {
    const std::wstring directory = ModuleDirectory();
    if (directory.empty())
      return 1;
    return RunBoundDesktopOperation(argv[2], L"nowait", L"runtime", L"start",
                                    directory + L"\\FamoRuntime.exe");
  }
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
  if (argc == 3 &&
      std::wstring_view(argv[1]) == L"clear-user-com-shadow") {
    const HRESULT result = ClearCurrentUserComShadow(argv[2]);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"current-user COM shadow cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"current_user_com_shadow=absent\n");
    return 0;
  }
  if (argc == 3 && std::wstring_view(argv[1]) == L"cleanup-user-for") {
    const HRESULT result = CleanupDesktopUser(argv[2]);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"bound desktop user cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"desktop_user_cleanup=yes\n");
    return 0;
  }
  if (argc == 3 &&
      std::wstring_view(argv[1]) == L"delete-user-data-for") {
    const HRESULT result = DeleteDesktopUserData(argv[2]);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"bound user-data cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"desktop_user_data=absent\n");
    return 0;
  }
  if (argc == 3 &&
      std::wstring_view(argv[1]) == L"delete-user-data-current") {
    const HRESULT result = DeleteCurrentUserData(argv[2]);
    if (FAILED(result)) {
      std::fwprintf(stderr, L"current user-data cleanup failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"current_user_data=absent\n");
    return 0;
  }
  if (argc != 2) {
    std::fwprintf(
        stderr,
        L"usage: FamoProfileTool register|register-machine|register-disabled|enable|disable|activate|check|check-machine|check-disabled|check-absent|is-active|is-enabled|switch-away|start-runtime|start-runtime-for <sid>|desktop-run-for <sid> wait|nowait <kind> <operation> <executable>|cleanup-user|cleanup-user-for <sid>|delete-user-data-for <sid>|delete-user-data-current <sid>|clear-user-com-shadow <sid>|unregister|unregister-machine|unregister-machine-direct|loaded <dll>|capture-original-user <pipe-id> <challenge> <record>|capture-original-user-for <sid> <pipe-id> <challenge> <record>|prove-current-token <pipe-id> <challenge>|prove-shell-token <pipe-id> <challenge>|prove-shell-token-for <sid> <pipe-id> <challenge>\n");
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
  } else if (command == L"unregister-machine-direct") {
    const HRESULT result = UnregisterMachineWithoutLoadingServiceDll();
    if (FAILED(result)) {
      std::fwprintf(stderr,
                    L"direct machine profile removal failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    std::wprintf(L"machine_registry=absent profile=absent category=absent\n");
    return 0;
  } else if (command == L"register" || command == L"register-machine" ||
             command == L"register-disabled") {
    const HRESULT result = InvokeRegistration(
        command == L"register-machine" ? "DllRegisterMachine"
                                        : "DllRegisterServer");
    if (FAILED(result)) {
      std::fwprintf(stderr, L"profile registration failed: 0x%08lx\n",
                    static_cast<unsigned long>(result));
      return 1;
    }
    if (command == L"register-machine") {
      WaitForMachineRegistrationVisibility();
    } else if (command == L"register-disabled") {
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
  } else if (command != L"check" && command != L"check-machine" &&
              command != L"check-disabled" &&
              command != L"check-absent" &&
              command != L"is-active" &&
              command != L"is-enabled") {
    return 2;
  }

  const bool registry = RegistryPresent();
  const bool profile = ProfileRegistered();
  const bool enabled = ProfileEnabled();
  const bool category = KeyboardCategoryRegistered();
  const bool active = ProfileActive();
  if (command == L"register-machine" || command == L"check-machine") {
    const bool machine = RegistryPresentAt(HKEY_LOCAL_MACHINE);
    std::wprintf(L"machine_registry=%ls profile=%ls category=%ls\n",
                 machine ? L"present" : L"absent",
                 profile ? L"present" : L"absent",
                 category ? L"present" : L"absent");
    return machine && profile && category ? 0 : 1;
  }
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
  if (command == L"is-enabled")
    return enabled ? 0 : 1;
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
#endif
