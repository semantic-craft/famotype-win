#include <cstdio>
#include <filesystem>
#include <fstream>

#include <windows.h>

#include "candidate_skin.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,       \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("famo-candidate-skin-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(root);
  std::ofstream(root / "famo-style.yaml")
      << "style:\n"
         "  color_scheme: wuda_dark\n"
         "  font_face: \"Segoe UI\"\n"
         "  font_point: 19\n"
         "  label_font_point: 13\n"
         "  comment_font_point: 11\n"
         "  horizontal: true\n"
         "  inline_preedit: true\n"
         "  preedit_type: preview\n"
         "  famo_auto_pair: true\n"
         "  famo_cjk_english_spacing: true\n"
         "  famo_cjk_number_spacing: true\n"
         "  corner_radius: 9\n"
         "  border_width: 2\n"
         "  shadow_radius: 7\n"
         "  margin_x: 15\n"
         "  margin_y: 16\n";
  FamoSkin skin{};
  CHECK(LoadCandidateSkin(root.string(), &skin));
  CHECK(skin.layout_type == FAMO_LAYOUT_HORIZONTAL);
  CHECK(skin.text_font.point_size == 19.0f);
  CHECK(skin.round_corner == 9 && skin.border == 2);
  CHECK(skin.margin_x == 15 && skin.margin_y == 16);
  CHECK(skin.hilited_back_color == 0xff3ca081u);
  CHECK(skin.text_color == 0xff98a19cu);
  CHECK(skin.candidate_text_color == 0xffe5eae7u);
  CHECK(skin.label_color == 0xff98a19cu);
  CHECK(skin.comment_color == 0xff66706bu);
  CHECK(skin.shadow_color == 0x73000000u);

  FamoSkin high_contrast = FamoSkinDefault();
  ApplyHighContrastPalette(&high_contrast, 0xff010203u, 0xfff1f2f3u,
                           0xff112233u, 0xffe1e2e3u);
  CHECK(high_contrast.back_color == 0xff010203u);
  CHECK(high_contrast.text_color == 0xfff1f2f3u);
  CHECK(high_contrast.candidate_text_color == 0xfff1f2f3u);
  CHECK(high_contrast.label_color == 0xfff1f2f3u);
  CHECK(high_contrast.comment_color == 0xfff1f2f3u);
  CHECK(high_contrast.border_color == 0xfff1f2f3u);
  CHECK(high_contrast.hilited_back_color == 0xff112233u);
  CHECK(high_contrast.hilited_text_color == 0xffe1e2e3u);
  CHECK(high_contrast.hilited_comment_color == 0xffe1e2e3u);
  CHECK(high_contrast.shadow_color == 0u);
  CHECK(high_contrast.shadow_radius == 0);

  FamoSkin hidden = FamoSkinDefault();
  hidden.label_color = 0;
  hidden.comment_color = 0;
  hidden.hilited_comment_color = 0;
  hidden.prevpage_color = 0;
  hidden.nextpage_color = 0;
  ApplyHighContrastPalette(&hidden, 0xff010203u, 0xfff1f2f3u,
                           0xff112233u, 0xffe1e2e3u);
  CHECK(hidden.label_color == 0);
  CHECK(hidden.comment_color == 0);
  CHECK(hidden.hilited_comment_color == 0);
  CHECK(hidden.prevpage_color == 0);
  CHECK(hidden.nextpage_color == 0);
  std::ofstream(root / "famo-style.yaml")
      << "style:\n  color_scheme: wuda\noutside: true\n";
  CHECK(!LoadCandidateSkin(root.string(), &skin));
  std::ofstream(root / "famo-style.yaml")
      << "style:\n  color_scheme: wuda\n  unknown: true\n";
  CHECK(!LoadCandidateSkin(root.string(), &skin));
  std::ofstream(root / "famo-style.yaml")
      << "style:\n  margin_x: 10\n  margin_x: 11\n";
  CHECK(!LoadCandidateSkin(root.string(), &skin));
  std::filesystem::remove_all(root);
  std::printf("candidate_skin_selfcheck: OK\n");
  return 0;
}
