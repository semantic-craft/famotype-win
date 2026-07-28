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
         "  show_preedit: false\n"
         "  preview_pages: true\n"
         "  preview_rows: 1\n"
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
  CHECK(skin.show_preedit == 0 && skin.preview_pages == 1 &&
        skin.preview_rows == 1);
  CHECK(skin.hilited_back_color == 0xff3ca081u);
  CHECK(skin.text_color == 0xff98a19cu);
  CHECK(skin.candidate_text_color == 0xffe5eae7u);
  CHECK(skin.label_color == 0xff98a19cu);
  CHECK(skin.comment_color == 0xff66706bu);
  CHECK(skin.shadow_color == 0x80000000u);

  const std::string system_style =
      "style:\n  color_scheme: shenda\n  color_scheme_dark: shenda_dark\n";
  FamoSkin system_light{};
  FamoSkin system_dark{};
  CHECK(ParseCandidateSkinForTheme(system_style, false, &system_light));
  CHECK(ParseCandidateSkinForTheme(system_style, true, &system_dark));
  CHECK(system_light.hilited_back_color == 0xffa82c53u);
  CHECK(system_dark.hilited_back_color == 0xffe06a8eu);
  CHECK(ParseCandidateSkinForTheme(system_style, false, &system_light));
  CHECK(system_light.hilited_back_color == 0xffa82c53u);

  struct PaletteCase {
    const char *name;
    uint32_t accent, deep, card, card2, on_accent, ink, ink2, ink3;
  };
  const PaletteCase mac_parity[] = {
      {"illinois", 0xff13294b, 0xffcc4a00, 0xfffcf4e9, 0xfffbe7d4,
       0xffff7a2e, 0xff13294b, 0xff6e5a3a, 0xff9a8a6e},
      {"illinois_dark", 0xffff7a2e, 0xff13294b, 0xff1e2334, 0xff262e44,
       0xff13294b, 0xffece6dc, 0xff9aa0b0, 0xff6a7185},
      {"illinoisflame", 0xffc24a00, 0xff9a3a00, 0xfffdf6ec,
       0xfff6e9d4, 0xfffff4e8, 0xff3a2a1b, 0xff7b6248, 0xffab9174},
      {"illinoisflame_dark", 0xffff6e24, 0xffc24a00, 0xff241a12,
       0xff2c2117, 0xff2b1707, 0xfff1e5d7, 0xffb49c85, 0xff7f6c59},
      {"nyu", 0xff57068c, 0xff3f0567, 0xfffbf9fe, 0xfff2ecf9,
       0xfffbf9fe, 0xff2a2333, 0xff655b79, 0xff958bac},
      {"nyu_dark", 0xffa274da, 0xff57068c, 0xff242029, 0xff1d1a25,
       0xff15101f, 0xffeae5f1, 0xffa99ebb, 0xff786c8c},
  };
  for (const PaletteCase &p : mac_parity) {
    std::ofstream(root / "famo-style.yaml")
        << "style:\n  color_scheme: " << p.name << "\n";
    CHECK(LoadCandidateSkin(root.string(), &skin));
    CHECK(skin.hilited_back_color == p.accent);
    CHECK(skin.border_color == ((p.deep & 0x00ffffffu) | 0x29000000u));
    CHECK((skin.back_color & 0x00ffffffu) ==
          (p.card & 0x00ffffffu));
    // Opaque regardless of the system's EnableTransparency setting: the panel is
    // a layered window with no blur behind it, so translucency was see-through,
    // not acrylic.
    CHECK((skin.back_color >> 24) == 0xffu);
    CHECK(skin.card2_color == p.card2);
    CHECK(skin.hilited_text_color == p.on_accent);
    CHECK(skin.candidate_text_color == p.ink);
    CHECK(skin.text_color == p.ink2);
    CHECK(skin.comment_color == p.ink3);
  }

  FamoSkin high_contrast = FamoSkinDefault();
  ApplyHighContrastPalette(&high_contrast, 0xff010203u, 0xfff1f2f3u,
                           0xff112233u, 0xffe1e2e3u);
  CHECK(high_contrast.back_color == 0xff010203u);
  CHECK(high_contrast.card2_color == 0xff010203u);
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
  std::ofstream(root / "famo-style.yaml")
      << "style:\n  font_point: nan\n";
  CHECK(!LoadCandidateSkin(root.string(), &skin));
  std::filesystem::remove_all(root);
  std::printf("candidate_skin_selfcheck: OK\n");
  return 0;
}
