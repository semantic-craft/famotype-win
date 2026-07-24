// FamoKeyEvent -> (keysym, modifier mask) for RimeApi::process_key.
//
// Engine pass-through (design decision, Session 20): the host supplies
// FamoKeyEvent.virtual_key already as a rime/X11 keysym and FamoKeyEvent.modifiers
// already as a rime/X11 modifier mask — each host expands from its own platform.
// The Weasel reroute passes keyEvent.keycode + expand_ibus_modifier(keyEvent.mask),
// i.e. the exact arguments legacy hands rime_api->process_key, so the abi path
// reaches rime with byte-identical input (IPC _Respond byte-parity, Tier C bar).
// The engine no longer does its own VK->keysym mapping (which forced every host to
// speak Windows VK; the Weasel handler only ever has the post-TSF ibus keysym).
// virtual_key==0 means "no key" — the caller then just refreshes context without
// feeding a key to rime.
#pragma once

#include <cstdint>

#include "../../famo_engine_api.h"

namespace famo_rime_keys {

inline bool FamoKeyToRime(const FamoKeyEvent& k, int* out_keycode, int* out_mask) {
  *out_keycode = static_cast<int>(k.virtual_key);
  *out_mask = static_cast<int>(k.modifiers);
  return k.virtual_key != 0;
}

}  // namespace famo_rime_keys
