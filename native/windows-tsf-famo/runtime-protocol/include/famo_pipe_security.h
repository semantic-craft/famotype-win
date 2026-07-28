#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <windows.h>

namespace famo::runtime {

struct PipeEndpoint {
  std::wstring name;
  std::wstring user_sid;
  uint32_t session_id = 0;
};

struct PipeClientIdentity {
  uint32_t process_id = 0;
  uint64_t process_creation_time = 0;

  bool operator==(const PipeClientIdentity &) const = default;
  explicit operator bool() const {
    return process_id != 0 && process_creation_time != 0;
  }
};

bool BuildCurrentPipeEndpoint(std::wstring_view suffix, PipeEndpoint *endpoint,
                              std::string *error);
PipeEndpoint BuildUiPipeEndpoint(const PipeEndpoint &endpoint);
bool BuildPipeSecurity(const PipeEndpoint &endpoint,
                       SECURITY_ATTRIBUTES *attributes,
                       PSECURITY_DESCRIPTOR *descriptor, std::string *error);
bool VerifyPipeServer(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::wstring_view expected_path, std::string *error,
                      PipeClientIdentity *identity = nullptr);
bool VerifyPipeClient(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::string *error,
                      PipeClientIdentity *identity = nullptr,
                      std::wstring_view expected_path = {});
// Acquires a live handle only when PID and process creation time still match.
// Keeping the returned handle open prevents Windows from recycling that PID.
// The caller owns the handle and must CloseHandle it.
HANDLE AcquirePipeClientIdentityLease(
    const PipeClientIdentity &identity) noexcept;
bool PipeClientIsAlive(const PipeClientIdentity &identity);

} // namespace famo::runtime
