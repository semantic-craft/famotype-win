#include "famo_pipe_security.h"

#include <aclapi.h>
#include <sddl.h>

#include <cwctype>
#include <vector>

#include "win_handle.h"

namespace famo::runtime {
namespace {

using win::UniqueHandle;

void WinError(std::string_view operation, std::string *error) {
  if (error) {
    *error =
        std::string(operation) + " failed: " + std::to_string(GetLastError());
  }
}

bool IsSafeSuffix(std::wstring_view value) {
  if (value.empty() || value.size() > 64)
    return false;
  for (wchar_t ch : value) {
    if (!std::iswalnum(ch) && ch != L'-' && ch != L'_')
      return false;
  }
  return true;
}

bool FullPath(std::wstring_view value, std::wstring *full) {
  const DWORD required =
      GetFullPathNameW(std::wstring(value).c_str(), 0, nullptr, nullptr);
  if (required == 0)
    return false;
  std::vector<wchar_t> buffer(required);
  const DWORD written = GetFullPathNameW(std::wstring(value).c_str(), required,
                                         buffer.data(), nullptr);
  if (written == 0 || written >= required)
    return false;
  full->assign(buffer.data(), written);
  if (full->starts_with(L"\\\\?\\"))
    full->erase(0, 4);
  return true;
}

bool SamePath(std::wstring_view left, std::wstring_view right) {
  std::wstring left_full, right_full;
  if (!FullPath(left, &left_full) || !FullPath(right, &right_full))
    return false;
  return _wcsicmp(left_full.c_str(), right_full.c_str()) == 0;
}

// peer_unreadable is set only when the peer process cannot be inspected at
// all. A refusal with an answer in hand -- wrong session, wrong image -- stays
// a hard failure so it can never be mistaken for a sandbox restriction.
bool VerifyProcess(DWORD pid, const PipeEndpoint &endpoint,
                   std::wstring_view expected_path, std::string *error,
                   bool *peer_unreadable = nullptr) {
  DWORD session_id = 0;
  if (!ProcessIdToSessionId(pid, &session_id)) {
    // An AppContainer host cannot query another process's session, and this is
    // the first thing asked, so it denies the whole peer check before the
    // image comparison is ever reached. The endpoint name the client opened
    // already encodes its own user SID and session, so the caller's
    // owner-based proof carries the guarantee this branch would have given.
    if (GetLastError() == ERROR_ACCESS_DENIED && peer_unreadable)
      *peer_unreadable = true;
    WinError("ProcessIdToSessionId", error);
    return false;
  }
  if (session_id != endpoint.session_id) {
    if (error)
      *error = "pipe peer belongs to a different session";
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }
  if (expected_path.empty())
    return true;

  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) {
    if (GetLastError() == ERROR_ACCESS_DENIED && peer_unreadable)
      *peer_unreadable = true;
    WinError("OpenProcess(peer)", error);
    return false;
  }
  std::vector<wchar_t> path(32768);
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
    WinError("QueryFullProcessImageName", error);
    return false;
  }
  if (!SamePath(std::wstring_view(path.data(), length), expected_path)) {
    if (error)
      *error = "pipe peer executable path mismatch";
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }
  return true;
}

uint64_t FileTimeValue(const FILETIME &value) {
  ULARGE_INTEGER encoded{};
  encoded.LowPart = value.dwLowDateTime;
  encoded.HighPart = value.dwHighDateTime;
  return encoded.QuadPart;
}

// Proves the pipe object itself belongs to the expected user. The owner is
// readable from the handle already held, so it needs no access to the peer
// process. An AppContainer cannot create a pipe under this name, so a lesser
// privileged squatter is still refused, and a pipe owned by any other user is
// rejected outright.
bool VerifyPipeOwner(HANDLE pipe, const PipeEndpoint &endpoint,
                     std::string *error) {
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status =
      GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                      &owner, nullptr, nullptr, nullptr, &descriptor);
  if (status != ERROR_SUCCESS) {
    SetLastError(status);
    WinError("GetSecurityInfo(pipe owner)", error);
    return false;
  }
  LPWSTR owner_text = nullptr;
  const bool converted =
      owner != nullptr && ConvertSidToStringSidW(owner, &owner_text) != FALSE;
  const bool matches =
      converted && endpoint.user_sid == std::wstring_view(owner_text);
  if (owner_text)
    LocalFree(owner_text);
  if (descriptor)
    LocalFree(descriptor);
  if (!matches) {
    if (error)
      *error = "pipe owner is not the expected user";
    SetLastError(ERROR_ACCESS_DENIED);
  }
  return matches;
}

bool ReadProcessIdentity(DWORD pid, PipeClientIdentity *identity,
                         std::string *error,
                         bool *peer_unreadable = nullptr) {
  if (!identity)
    return true;
  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) {
    if (GetLastError() == ERROR_ACCESS_DENIED && peer_unreadable)
      *peer_unreadable = true;
    WinError("OpenProcess(client identity)", error);
    return false;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
    WinError("GetProcessTimes(client)", error);
    return false;
  }
  const uint64_t creation_time = FileTimeValue(created);
  if (creation_time == 0) {
    if (error)
      *error = "client process creation time is unavailable";
    return false;
  }
  *identity = {pid, creation_time};
  return true;
}

} // namespace

bool BuildCurrentPipeEndpoint(std::wstring_view suffix, PipeEndpoint *endpoint,
                              std::string *error) {
  if (!endpoint || !IsSafeSuffix(suffix)) {
    if (error)
      *error = "invalid pipe endpoint suffix";
    return false;
  }

  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    WinError("OpenProcessToken", error);
    return false;
  }
  UniqueHandle token(raw_token);
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    WinError("GetTokenInformation(size)", error);
    return false;
  }
  std::vector<uint8_t> storage(size);
  if (!GetTokenInformation(token.get(), TokenUser, storage.data(), size,
                           &size)) {
    WinError("GetTokenInformation", error);
    return false;
  }
  const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.data());
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
    WinError("ConvertSidToStringSid", error);
    return false;
  }
  endpoint->user_sid = sid_text;
  LocalFree(sid_text);
  DWORD session_id = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
    WinError("ProcessIdToSessionId(current)", error);
    return false;
  }
  endpoint->session_id = session_id;
  endpoint->name = L"\\\\.\\pipe\\Famo.Runtime.v2." + endpoint->user_sid +
                   L"." + std::to_wstring(endpoint->session_id) + L"." +
                   std::wstring(suffix);
  return true;
}

PipeEndpoint BuildUiPipeEndpoint(const PipeEndpoint &endpoint) {
  PipeEndpoint ui = endpoint;
  ui.name += L".ui";
  return ui;
}

bool BuildPipeSecurity(const PipeEndpoint &endpoint,
                       SECURITY_ATTRIBUTES *attributes,
                       PSECURITY_DESCRIPTOR *descriptor, std::string *error) {
  if (!attributes || !descriptor || endpoint.user_sid.empty()) {
    if (error)
      *error = "invalid security descriptor arguments";
    return false;
  }
  // AppContainer clients still carry the desktop user's SID, but Windows
  // performs a second access check against their restricted SID set. Keep the
  // existing per-user boundary and grant only pipe read/write to both modern
  // application-package classes. The low mandatory label permits their
  // low-integrity tokens to write to the pipe; the DACL still limits access to
  // the current user, SYSTEM, and those package classes. Without both pieces,
  // hosts such as SearchHost load the TIP but cannot open the out-of-process
  // Runtime channel.
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;" + endpoint.user_sid +
      L")(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)S:(ML;;NW;;;LW)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, descriptor, nullptr)) {
    WinError("ConvertStringSecurityDescriptor", error);
    return false;
  }
  attributes->nLength = sizeof(*attributes);
  attributes->lpSecurityDescriptor = *descriptor;
  attributes->bInheritHandle = FALSE;
  return true;
}

bool VerifyPipeServer(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::wstring_view expected_path, std::string *error,
                      PipeClientIdentity *identity) {
  ULONG pid = 0;
  if (!GetNamedPipeServerProcessId(pipe, &pid)) {
    WinError("GetNamedPipeServerProcessId", error);
    return false;
  }
  bool peer_unreadable = false;
  if (VerifyProcess(pid, endpoint, expected_path, error, &peer_unreadable) &&
      ReadProcessIdentity(pid, identity, error, &peer_unreadable)) {
    return true;
  }
  // A host that runs in an AppContainer -- the Windows search surface and the
  // shell input hosts do -- cannot open a medium integrity process, so the
  // server's image path and creation time are unreadable from there and the
  // checks above fail with ERROR_ACCESS_DENIED. Refusing the connection would
  // leave those hosts with no engine at all: every key would pass through
  // untranslated. Prove the peer a different way instead of dropping the
  // requirement.
  if (!peer_unreadable)
    return false;
  if (!VerifyPipeOwner(pipe, endpoint, error))
    return false;
  // The peer is the expected user's, but this host could not read the process
  // identity that authenticates a cross-process candidate click. Leave it
  // empty: an empty identity is falsy, so every capability that depends on it
  // refuses rather than trusting an unauthenticated peer.
  if (identity)
    *identity = {};
  return true;
}

bool VerifyPipeClient(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::string *error, PipeClientIdentity *identity,
                      std::wstring_view expected_path) {
  ULONG pid = 0;
  if (!GetNamedPipeClientProcessId(pipe, &pid)) {
    WinError("GetNamedPipeClientProcessId", error);
    return false;
  }
  return VerifyProcess(pid, endpoint, expected_path, error) &&
         ReadProcessIdentity(pid, identity, error);
}

HANDLE AcquirePipeClientIdentityLease(
    const PipeClientIdentity &identity) noexcept {
  if (!identity)
    return nullptr;
  UniqueHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, identity.process_id));
  if (!process)
    return nullptr;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user) ||
      FileTimeValue(created) != identity.process_creation_time) {
    return nullptr;
  }
  if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
    return nullptr;
  return process.release();
}

bool PipeClientIsAlive(const PipeClientIdentity &identity) {
  UniqueHandle process(AcquirePipeClientIdentityLease(identity));
  return process != nullptr;
}

} // namespace famo::runtime
