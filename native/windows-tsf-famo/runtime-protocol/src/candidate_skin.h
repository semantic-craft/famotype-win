#pragma once

#include <string_view>

#include "../../famo-candidate-ui/famo_candidate_ui.h"

namespace famo::runtime {

bool ParseCandidateSkin(std::string_view text, FamoSkin *skin);
bool LoadCandidateSkin(std::string_view data_root, FamoSkin *skin);
void ApplyHighContrastPalette(FamoSkin *skin, uint32_t background,
                              uint32_t foreground,
                              uint32_t selected_background,
                              uint32_t selected_foreground);

} // namespace famo::runtime
