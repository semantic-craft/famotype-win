#pragma once

#include <cstdint>

namespace famo::tsf {

#ifndef FAMO_BRIDGE_ABI_VERSION
#define FAMO_BRIDGE_ABI_VERSION 4
#endif

// Increment only when the installed TSF Bridge payload changes. Product and
// Runtime versions advance independently of this stable in-process ABI.
inline constexpr uint32_t kBridgeAbiVersion = FAMO_BRIDGE_ABI_VERSION;
static_assert(kBridgeAbiVersion > 0);

} // namespace famo::tsf
