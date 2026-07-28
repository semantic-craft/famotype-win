#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../famo_engine_api.h"

namespace famo_view_abi {

inline bool HasField(uint32_t view_size, size_t offset, size_t field_size) {
  return offset <= view_size && field_size <= view_size - offset;
}

inline bool Negotiate(const FamoCompositionView* caller,
                      uint32_t* negotiated_size) {
  if (!caller || !negotiated_size ||
      caller->size <
          static_cast<uint32_t>(FAMO_COMPOSITION_VIEW_V1_REQUIRED_SIZE)) {
    return false;
  }
  *negotiated_size =
      caller->size < static_cast<uint32_t>(sizeof(FamoCompositionView))
          ? caller->size
          : static_cast<uint32_t>(sizeof(FamoCompositionView));
  return true;
}

inline bool HasV12Candidates(uint32_t view_size) {
  return view_size >=
         static_cast<uint32_t>(FAMO_COMPOSITION_VIEW_V12_FIELD_SIZE);
}

inline size_t CandidateStride(uint32_t view_size) {
  return HasV12Candidates(view_size)
             ? sizeof(FamoCandidate)
             : static_cast<size_t>(FAMO_CANDIDATE_V1_STRIDE);
}

inline FamoCandidate* CandidateAt(void* candidates, size_t index,
                                  size_t stride) {
  return reinterpret_cast<FamoCandidate*>(
      static_cast<unsigned char*>(candidates) + index * stride);
}

inline void BeginResult(FamoCompositionView* result, uint32_t view_size) {
  std::memset(result, 0, sizeof(*result));
  result->size = view_size;
}

inline void Publish(FamoCompositionView* caller,
                    const FamoCompositionView& result,
                    uint32_t view_size) {
  std::memcpy(caller, &result, view_size);
}

inline void ClearPreservingSize(FamoCompositionView* view,
                                uint32_t view_size) {
  std::memset(view, 0, view_size);
  view->size = view_size;
}

}  // namespace famo_view_abi
