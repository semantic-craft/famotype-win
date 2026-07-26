#include "runtime_style_config.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <charconv>
#include <set>
#include <string>

#include <windows.h>

#include "famo_runtime_protocol.h"

namespace famo::runtime {
namespace {

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

bool StylePath(std::string_view root, std::filesystem::path *path) {
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

bool ParseBoolean(std::string_view value, bool *parsed) {
  value = Trim(value);
  if (!parsed || (value != "true" && value != "false"))
    return false;
  *parsed = value == "true";
  return true;
}

bool ParsePreviewRows(std::string_view value, uint32_t *rows) {
  value = Trim(value);
  if (!rows)
    return false;
  uint32_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed < 1 || parsed > 2)
    return false;
  *rows = parsed;
  return true;
}

} // namespace

bool ParseHostBehaviorFlags(std::string_view text, uint32_t *flags) {
  static const std::set<std::string_view> kKnown = {"color_scheme",
                                                    "color_scheme_dark",
                                                    "font_face",
                                                    "font_point",
                                                    "label_format",
                                                    "label_font_point",
                                                    "comment_font_point",
                                                    "horizontal",
                                                    "orientation",
                                                    "inline_preedit",
                                                    "show_preedit",
                                                    "preview_pages",
                                                    "preview_rows",
                                                    "preedit_type",
                                                    "corner_radius",
                                                    "border_width",
                                                    "shadow_radius",
                                                    "margin_x",
                                                    "margin_y",
                                                    "famo_auto_pair",
                                                    "famo_cjk_english_spacing",
                                                    "famo_cjk_number_spacing"};
  if (!flags)
    return false;
  *flags = 0;
  bool saw_style = false;
  std::set<std::string> seen;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    std::string_view line(text.data() + start,
                          (end == std::string_view::npos ? text.size() : end) -
                              start);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const std::string_view trimmed = Trim(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      if (line == "style:" && !saw_style) {
        saw_style = true;
      } else {
        if (!saw_style || line.size() < 3 || line.substr(0, 2) != "  " ||
            std::isspace(static_cast<unsigned char>(line[2])))
          return false;
        const std::string_view child = line.substr(2);
        const size_t colon = child.find(':');
        const std::string_view key = colon == std::string_view::npos
                                         ? std::string_view{}
                                         : child.substr(0, colon);
        const std::string_view value = colon == std::string_view::npos
                                           ? std::string_view{}
                                           : Trim(child.substr(colon + 1));
        if (!kKnown.contains(key) || value.empty() || !seen.emplace(key).second)
          return false;

        bool enabled = false;
        uint32_t bit = 0;
        if (key == "inline_preedit")
          bit = kHostInlinePreedit;
        else if (key == "famo_auto_pair")
          bit = kHostAutoPair;
        else if (key == "famo_cjk_english_spacing")
          bit = kHostCjkEnglishSpacing;
        else if (key == "famo_cjk_number_spacing")
          bit = kHostCjkNumberSpacing;
        else if (key == "preview_pages")
          bit = kHostPreviewPages;
        if (bit != 0) {
          if (!ParseBoolean(value, &enabled))
            return false;
          if (enabled)
            *flags |= bit;
        } else if (key == "preedit_type") {
          if (value != "composition" && value != "preview")
            return false;
          if (value == "preview")
            *flags |= kHostCandidatePreview;
        } else if (key == "preview_rows") {
          uint32_t rows = 0;
          if (!ParsePreviewRows(value, &rows))
            return false;
          if (rows == 2)
            *flags |= kHostPreviewRowsTwo;
        } else if (key == "show_preedit") {
          if (!ParseBoolean(value, &enabled))
            return false;
        }
      }
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return saw_style;
}

bool ReadRuntimeStyleOverlay(std::string_view data_root,
                             RuntimeStyleOverlay *overlay) {
  if (!overlay)
    return false;
  *overlay = {};
  std::filesystem::path path;
  if (!StylePath(data_root, &path))
    return false;
  std::error_code ec;
  overlay->exists = std::filesystem::exists(path, ec);
  if (ec)
    return false;
  if (!overlay->exists)
    return true;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxFrameSize)
    return false;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  overlay->text.assign(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  if ((!input.eof() && !input.good()) || !IsValidUtf8(overlay->text))
    return false;
  return ParseHostBehaviorFlags(overlay->text, &overlay->host_behavior_flags);
}

} // namespace famo::runtime
