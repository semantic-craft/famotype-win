#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace famo::runtime {

struct RuntimeStyleOverlay {
  std::string text;
  bool exists = false;
  uint32_t host_behavior_flags = 0;
};

bool ReadRuntimeStyleOverlay(std::string_view data_root,
                             RuntimeStyleOverlay *overlay);

} // namespace famo::runtime
