#pragma once

#include <string>
#include <vector>

#include <ctffunc.h>

namespace famo::tsf {

HRESULT CreateSearchCandidateList(std::vector<std::wstring> candidates,
                                  ITfCandidateList **list) noexcept;

} // namespace famo::tsf
