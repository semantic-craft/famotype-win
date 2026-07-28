#pragma once

namespace famo::runtime {

#if defined(FAMO_STABLE_IDENTITY)
inline constexpr wchar_t kDefaultRuntimeEndpointSuffix[] = L"runtime-v2";
inline constexpr wchar_t kDefaultControlEndpointSuffix[] = L"control-v2";
inline constexpr wchar_t kRuntimeSingletonPrefix[] = L"Famo.Runtime.v2";
#else
inline constexpr wchar_t kDefaultRuntimeEndpointSuffix[] = L"dev-runtime-v2";
inline constexpr wchar_t kDefaultControlEndpointSuffix[] = L"dev-control-v2";
inline constexpr wchar_t kRuntimeSingletonPrefix[] = L"Famo.Dev.Runtime.v2";
#endif

} // namespace famo::runtime
