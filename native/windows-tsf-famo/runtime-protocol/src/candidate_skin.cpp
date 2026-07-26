#include "candidate_skin.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <windows.h>

#include "famo_runtime_protocol.h"

namespace famo::runtime {
namespace {

struct Palette {
  uint32_t accent;
  uint32_t deep;
  uint32_t card;
  uint32_t card2;
  uint32_t on_accent;
  uint32_t ink;
  uint32_t ink2;
  uint32_t ink3;
};

const std::map<std::string, Palette> &Palettes() {
  static const std::map<std::string, Palette> values = {
      {"shenda",
       {0xffa82c53, 0xff8e2447, 0xfffbf9f5, 0xfff3efe7, 0xfffbf9f5,
        0xff2a2622, 0xff6b6a64, 0xff9a9387}},
      {"stanford",
       {0xff8c1515, 0xff820000, 0xfffbfbfc, 0xfff1f2f4, 0xfffbfbfc,
        0xff2e2d29, 0xff53565a, 0xff8a8d90}},
      {"wuda",
       {0xff2a8367, 0xff1f6b52, 0xfff8fbf9, 0xffeff2ee, 0xfff9faf8,
        0xff282d2a, 0xff565f5a, 0xff8a938e}},
      {"xiada",
       {0xff1d4a8c, 0xff123061, 0xfff8fafc, 0xffeff2f7, 0xfff8fafc,
        0xff242a36, 0xff5c6a81, 0xff8898af}},
      {"illinois",
       {0xff13294b, 0xffcc4a00, 0xfffcf4e9, 0xfffbe7d4, 0xffff7a2e,
        0xff13294b, 0xff6e5a3a, 0xff9a8a6e}},
      {"illinoisflame",
       {0xffc24a00, 0xff9a3a00, 0xfffdf6ec, 0xfff6e9d4, 0xfffff4e8,
        0xff3a2a1b, 0xff7b6248, 0xffab9174}},
      {"nyu",
       {0xff57068c, 0xff3f0567, 0xfffbf9fe, 0xfff2ecf9, 0xfffbf9fe,
        0xff2a2333, 0xff655b79, 0xff958bac}},
      {"shenda_dark",
       {0xffe06a8e, 0xffc24e72, 0xff262321, 0xff211e1c, 0xff1a1816,
        0xffece4d8, 0xffa89e90, 0xff766d62}},
      {"stanford_dark",
       {0xffb83a4b, 0xff8c1515, 0xff26282c, 0xff212327, 0xfff2f2f0,
        0xffe8eaed, 0xff9da1a6, 0xff6c7075}},
      {"wuda_dark",
       {0xff3ca081, 0xff2a8367, 0xff212423, 0xff1c1e1d, 0xff121413,
        0xffe5eae7, 0xff98a19c, 0xff66706b}},
      {"xiada_dark",
       {0xff4879c5, 0xff1d4a8c, 0xff212429, 0xff1b1e22, 0xff0f141c,
        0xffe6eaf0, 0xff98a4b8, 0xff66758a}},
      {"illinois_dark",
       {0xffff7a2e, 0xff13294b, 0xff1e2334, 0xff262e44, 0xff13294b,
        0xffece6dc, 0xff9aa0b0, 0xff6a7185}},
      {"illinoisflame_dark",
       {0xffff6e24, 0xffc24a00, 0xff241a12, 0xff2c2117, 0xff2b1707,
        0xfff1e5d7, 0xffb49c85, 0xff7f6c59}},
      {"nyu_dark",
       {0xffa274da, 0xff57068c, 0xff242029, 0xff1d1a25, 0xff15101f,
        0xffeae5f1, 0xffa99ebb, 0xff786c8c}},
  };
  return values;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

std::string Unquote(std::string_view value) {
  value = Trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  return std::string(value);
}

bool ParseInt(std::string_view value, int low, int high, int *result) {
  const std::string text(Trim(value));
  if (text.empty() || !result)
    return false;
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (errno || end != text.c_str() + text.size() || parsed < low ||
      parsed > high)
    return false;
  *result = static_cast<int>(parsed);
  return true;
}

bool ParseFloat(std::string_view value, float low, float high, float *result) {
  const std::string text(Trim(value));
  if (text.empty() || !result)
    return false;
  char *end = nullptr;
  errno = 0;
  const float parsed = std::strtof(text.c_str(), &end);
  if (errno || end != text.c_str() + text.size() || parsed < low ||
      parsed > high)
    return false;
  *result = parsed;
  return true;
}

bool DataPath(std::string_view root, std::filesystem::path *path) {
  if (!path || !IsValidUtf8(root))
    return false;
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root.data(),
                          static_cast<int>(root.size()), nullptr, 0);
  if (count <= 0)
    return false;
  std::wstring wide(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root.data(),
                          static_cast<int>(root.size()), wide.data(),
                          count) != count)
    return false;
  *path = std::filesystem::path(wide) / L"famo-style.yaml";
  return true;
}

bool ApplyPalette(std::string_view name, FamoSkin *skin) {
  const auto found = Palettes().find(std::string(name));
  if (found == Palettes().end())
    return false;
  const Palette &p = found->second;
  const bool dark = name.size() >= 5 && name.substr(name.size() - 5) == "_dark";
  skin->text_color = p.ink2;
  DWORD transparency = 1;
  DWORD transparency_size = sizeof(transparency);
  const bool transparent =
      RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"EnableTransparency", RRF_RT_REG_DWORD, nullptr,
                   &transparency, &transparency_size) != ERROR_SUCCESS ||
      transparency != 0;
  skin->back_color = (p.card & 0x00ffffffu) |
                     (transparent ? 0xb8000000u : 0xff000000u);
  skin->card2_color = p.card2;
  skin->border_color = (p.deep & 0x00ffffffu) | 0x29000000u;
  skin->hilited_text_color = p.on_accent;
  skin->hilited_back_color = p.accent;
  skin->candidate_text_color = p.ink;
  skin->label_color = p.ink2;
  skin->comment_color = p.ink3;
  skin->hilited_comment_color = p.on_accent;
  skin->prevpage_color = p.ink3;
  skin->nextpage_color = p.ink3;
  skin->shadow_color = dark ? 0x80000000u : 0x38000000u;
  return true;
}

bool UseDarkPalette() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  const LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  return status == ERROR_SUCCESS && light == 0;
}

bool ApplyScalar(std::string_view key, std::string_view value, bool dark,
                 FamoSkin *skin) {
  if (key == "color_scheme" || key == "color_scheme_dark") {
    const std::string_view name = Trim(value);
    if (!Palettes().contains(std::string(name)))
      return false;
    if ((key == "color_scheme" || dark) && !ApplyPalette(name, skin))
      return false;
  } else if (key == "font_face") {
    const std::string face = Unquote(value);
    if (!IsValidUtf8(face) || face.empty() || face.size() >= FAMO_FONT_FACE_MAX)
      return false;
    for (FamoFontSpec *font :
         {&skin->text_font, &skin->label_font, &skin->comment_font}) {
      std::memset(font->face, 0, sizeof(font->face));
      std::memcpy(font->face, face.data(), face.size());
    }
  } else if (key == "font_point") {
    if (!ParseFloat(value, 8, 72, &skin->text_font.point_size))
      return false;
  } else if (key == "label_format") {
    const std::string format = Unquote(value);
    if (format != "%s." && !format.empty())
      return false;
    if (format.empty())
      skin->label_color = 0;
  } else if (key == "label_font_point") {
    if (!ParseFloat(value, 0, 72, &skin->label_font.point_size))
      return false;
    if (skin->label_font.point_size == 0)
      skin->label_color = 0;
  } else if (key == "comment_font_point") {
    if (!ParseFloat(value, 0, 72, &skin->comment_font.point_size))
      return false;
    if (skin->comment_font.point_size == 0) {
      skin->comment_color = 0;
      skin->hilited_comment_color = 0;
    }
  } else if (key == "horizontal") {
    const auto v = Trim(value);
    if (v != "true" && v != "false")
      return false;
    skin->layout_type =
        v == "true" ? FAMO_LAYOUT_HORIZONTAL : FAMO_LAYOUT_VERTICAL;
    skin->min_width = v == "true" ? 210 : 76;
  } else if (key == "show_preedit" || key == "preview_pages") {
    const auto v = Trim(value);
    if (v != "true" && v != "false")
      return false;
    const uint32_t enabled = v == "true" ? 1u : 0u;
    if (key == "show_preedit")
      skin->show_preedit = enabled;
    else
      skin->preview_pages = enabled;
  } else if (key == "preview_rows") {
    int rows = 0;
    if (!ParseInt(value, 1, 2, &rows))
      return false;
    skin->preview_rows = static_cast<uint32_t>(rows);
  } else if (key == "inline_preedit" || key == "famo_auto_pair" ||
             key == "famo_cjk_english_spacing" ||
             key == "famo_cjk_number_spacing") {
    const auto v = Trim(value);
    if (v != "true" && v != "false")
      return false;
    // Validated here as part of the same bounded style map; RuntimeService
    // consumes these host behavior keys atomically with this visual reload.
  } else if (key == "preedit_type") {
    const auto v = Trim(value);
    if (v != "composition" && v != "preview")
      return false;
  } else {
    int *target = nullptr;
    int high = 128;
    if (key == "corner_radius")
      target = &skin->round_corner;
    else if (key == "border_width") {
      target = &skin->border;
      high = 16;
    } else if (key == "shadow_radius")
      target = &skin->shadow_radius;
    else if (key == "margin_x")
      target = &skin->margin_x;
    else if (key == "margin_y")
      target = &skin->margin_y;
    if (!target || !ParseInt(value, 0, high, target))
      return false;
  }
  return true;
}

// The user's configured Windows text-cursor width (Settings → Accessibility →
// Text cursor, or HKCU\Control Panel\Desktop\CaretWidth; documented range 1..20).
//
// Read here on the HOST side, never inside famo-candidate-ui: that component is
// clean-room and pixel-tested, so a SystemParametersInfo call in its paint path
// would make bitmap_smoke's assertions depend on the test machine's
// accessibility settings.
//
// Floored at 2 rather than passed through raw. Windows' own default is 1, which
// is near-invisible on a transient IME popup at high DPI. A floor can only ever
// make the caret MORE visible, so it cannot regress the setting it is honouring;
// anything the user chooses above 2 passes through untouched.
int32_t SystemCaretWidth() {
  UINT width = 0;
  if (!SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &width, 0))
    return 2;
  if (width > 20u)
    width = 20u;
  return (std::max)(2, static_cast<int32_t>(width));
}

} // namespace

bool ParseCandidateSkin(std::string_view text, FamoSkin *skin) {
  if (!skin)
    return false;
  *skin = FamoSkinDefault();
  skin->caret_width = SystemCaretWidth();
  std::istringstream input{std::string(text)};
  std::string line;
  bool saw_style = false;
  std::set<std::string> seen;
  const bool dark = UseDarkPalette();
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string_view trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#')
      continue;
    if (line == "style:" && !saw_style) {
      saw_style = true;
      continue;
    }
    if (!saw_style || line.size() < 3 || line.substr(0, 2) != "  " ||
        std::isspace(static_cast<unsigned char>(line[2])))
      return false;
    const std::string_view child = std::string_view(line).substr(2);
    const size_t colon = child.find(':');
    const std::string_view key = colon == std::string_view::npos
                                     ? std::string_view{}
                                     : child.substr(0, colon);
    if (key.empty() || !seen.emplace(key).second ||
        !ApplyScalar(key, child.substr(colon + 1), dark, skin))
      return false;
  }
  if (skin->layout_type != FAMO_LAYOUT_HORIZONTAL) {
    skin->min_width = (std::max)(64, static_cast<int>(skin->text_font.point_size * 4.0f + 0.5f));
  }
  return input.eof() && saw_style;
}

bool LoadCandidateSkin(std::string_view data_root, FamoSkin *skin) {
  if (!skin)
    return false;
  *skin = FamoSkinDefault();
  skin->caret_width = SystemCaretWidth();
  std::filesystem::path path;
  if (!DataPath(data_root, &path))
    return false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return !ec;
  if (std::filesystem::file_size(path, ec) > kMaxFrameSize || ec)
    return false;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  if (!input.eof() && !input.good())
    return false;
  return ParseCandidateSkin(text, skin);
}

void ApplyHighContrastPalette(FamoSkin *skin, uint32_t background,
                              uint32_t foreground,
                              uint32_t selected_background,
                              uint32_t selected_foreground) {
  if (!skin)
    return;
  const bool show_label = (skin->label_color & 0xff000000u) != 0;
  const bool show_comment = (skin->comment_color & 0xff000000u) != 0;
  const bool show_selected_comment =
      (skin->hilited_comment_color & 0xff000000u) != 0;
  const bool show_prev_page = (skin->prevpage_color & 0xff000000u) != 0;
  const bool show_next_page = (skin->nextpage_color & 0xff000000u) != 0;
  skin->text_color = foreground;
  skin->back_color = background;
  skin->card2_color = background;
  skin->border_color = foreground;
  skin->hilited_text_color = selected_foreground;
  skin->hilited_back_color = selected_background;
  skin->candidate_text_color = foreground;
  skin->label_color = show_label ? foreground : 0;
  skin->comment_color = show_comment ? foreground : 0;
  skin->hilited_comment_color =
      show_selected_comment ? selected_foreground : 0;
  skin->prevpage_color = show_prev_page ? foreground : 0;
  skin->nextpage_color = show_next_page ? foreground : 0;
  skin->shadow_color = 0;
  skin->shadow_radius = 0;
  skin->shadow_offset_x = 0;
  skin->shadow_offset_y = 0;
  skin->border = (std::max)(1, skin->border);
}

} // namespace famo::runtime
