#pragma once

#include <string_view>

#include "../../famo-candidate-ui/famo_candidate_ui.h"

namespace famo::runtime {

struct CandidateStylePresentation {
  FamoSkin light;
  FamoSkin dark;
};

bool ParseCandidateSkin(std::string_view text, FamoSkin *skin);
bool ParseCandidateSkinForTheme(std::string_view text, bool dark,
                                FamoSkin *skin);
bool LoadCandidateSkin(std::string_view data_root, FamoSkin *skin);
bool SystemUsesDarkPalette();
void ApplyHighContrastPalette(FamoSkin *skin, uint32_t background,
                              uint32_t foreground,
                              uint32_t selected_background,
                              uint32_t selected_foreground);

} // namespace famo::runtime
