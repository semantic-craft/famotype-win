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

bool BuildCurrentPipeEndpoint(std::wstring_view suffix, PipeEndpoint *endpoint,
                              std::string *error);
PipeEndpoint BuildUiPipeEndpoint(const PipeEndpoint &endpoint);
bool BuildPipeSecurity(const PipeEndpoint &endpoint,
                       SECURITY_ATTRIBUTES *attributes,
                       PSECURITY_DESCRIPTOR *descriptor, std::string *error);
bool VerifyPipeServer(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::wstring_view expected_path, std::string *error);
bool VerifyPipeClient(HANDLE pipe, const PipeEndpoint &endpoint,
                      std::string *error);

} // namespace famo::runtime
