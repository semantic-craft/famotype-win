#pragma once

namespace famo::runtime {

#if defined(FAMO_STABLE_IDENTITY)
inline constexpr wchar_t kDefaultRuntimeEndpointSuffix[] = L"runtime-v1";
inline constexpr wchar_t kDefaultControlEndpointSuffix[] = L"control-v1";
inline constexpr wchar_t kRuntimeSingletonPrefix[] = L"Famo.Runtime.v1";
#else
inline constexpr wchar_t kDefaultRuntimeEndpointSuffix[] = L"dev-runtime-v1";
inline constexpr wchar_t kDefaultControlEndpointSuffix[] = L"dev-control-v1";
inline constexpr wchar_t kRuntimeSingletonPrefix[] = L"Famo.Dev.Runtime.v1";
#endif

} // namespace famo::runtime
