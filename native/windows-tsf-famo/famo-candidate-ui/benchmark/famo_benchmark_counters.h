#pragma once

#include <cstdint>

struct FamoBenchmarkRenderCounters {
  uint64_t text_surface = 0;
  uint64_t d2d_target = 0;
  uint64_t brush = 0;
  uint64_t text_layout = 0;
};

extern "C" void FamoBenchmarkRenderCountersReset() noexcept;
extern "C" FamoBenchmarkRenderCounters
FamoBenchmarkRenderCountersSnapshot() noexcept;
