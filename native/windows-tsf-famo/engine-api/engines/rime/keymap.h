// FamoKeyEvent -> (keysym, modifier mask) for RimeApi::process_key.
//
// The host supplies FamoKeyEvent.virtual_key as a rime/X11 keysym. ABI v1
// historically passed the host-expanded rime/X11 modifier mask through
// verbatim, despite also publishing compact convenience constants. ABI v2
// makes the expanded mask contract explicit. Both Weasel reroutes pass
// keyEvent.keycode + expand_ibus_modifier(keyEvent.mask), i.e. the exact
// arguments legacy hands rime_api->process_key.
// The engine no longer does its own VK->keysym mapping (which forced every host to
// speak Windows VK; the Weasel handler only ever has the post-TSF ibus keysym).
// virtual_key==0 means "no key" — the caller then just refreshes context without
// feeding a key to rime.
#pragma once

#include <cstdint>

#include "../../famo_engine_api.h"

namespace famo_rime_keys {

inline bool FamoKeyToRimeV1(const FamoKeyEvent& k, int* out_keycode,
                           int* out_mask) {
  *out_keycode = static_cast<int>(k.virtual_key);
  *out_mask = static_cast<int>(k.modifiers);
  return k.virtual_key != 0;
}

inline bool FamoKeyToRimeV2(const FamoKeyEvent& k, int* out_keycode,
                           int* out_mask) {
  *out_keycode = static_cast<int>(k.virtual_key);
  *out_mask = static_cast<int>(k.modifiers);
  return k.virtual_key != 0;
}

}  // namespace famo_rime_keys
