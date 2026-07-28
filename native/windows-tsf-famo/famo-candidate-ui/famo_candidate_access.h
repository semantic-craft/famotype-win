#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "famo_candidate_ui.h"

namespace famo_candidate_ui {

constexpr uint32_t kLegacyCandidateSize =
    static_cast<uint32_t>(offsetof(FamoCandidate, label));

inline bool ReadableUtf8(const FamoUtf8String& value) {
  return value.length_bytes == 0 ||
         (value.size >= sizeof(FamoUtf8String) && value.data);
}

// Candidate arrays predate the append-only v1.2 label. Their element stride is
// therefore carried by FamoCandidate::size, not by the consumer's sizeof.
inline bool ReadCandidate(const FamoCandidate* base, uint32_t count,
                          uint32_t index, FamoCandidate* out) {
  if (!base || !out || index >= count) return false;

  uint32_t stride = 0;
  std::memcpy(&stride, base, sizeof(stride));
  if (stride < kLegacyCandidateSize ||
      index > (std::numeric_limits<size_t>::max)() / stride)
    return false;

  const auto* bytes = reinterpret_cast<const unsigned char*>(base) +
                      static_cast<size_t>(index) * stride;
  uint32_t item_size = 0;
  std::memcpy(&item_size, bytes, sizeof(item_size));
  if (item_size != stride) return false;

  std::memset(out, 0, sizeof(*out));
  std::memcpy(out, bytes, (std::min)(static_cast<size_t>(stride), sizeof(*out)));
  if (stride < offsetof(FamoCandidate, label) + sizeof(FamoUtf8String)) {
    out->label.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
    out->label.data = nullptr;
    out->label.length_bytes = 0;
  }
  return true;
}

inline bool ReadableCandidate(const FamoCandidate& candidate) {
  return ReadableUtf8(candidate.text) && ReadableUtf8(candidate.comment) &&
         ReadableUtf8(candidate.label);
}

}  // namespace famo_candidate_ui
