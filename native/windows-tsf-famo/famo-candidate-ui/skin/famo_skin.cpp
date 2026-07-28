// FamoSkin compiled-in default (B2). The real source is the active Rime color
// scheme + famo-style.yaml, parsed by a loader wired in Phase C; here we ship a
// neutral opaque default so the component builds + the geometry self-check runs
// without any yaml. Values are the style knob-set re-declared as our own —
// names may echo intent (round_corner, hilite_padding_*); the code is ours.

#include <cstring>

#include "../famo_c_abi_boundary.h"
#include "../famo_candidate_ui.h"

namespace {
void SetFace(FamoFontSpec* f, const char* face, float pt) {
  std::memset(f, 0, sizeof(*f));
  // Truncate defensively; FAMO_FONT_FACE_MAX is generous for a face-list.
  size_t n = std::strlen(face);
  if (n > FAMO_FONT_FACE_MAX - 1) n = FAMO_FONT_FACE_MAX - 1;
  std::memcpy(f->face, face, n);  // buffer zeroed above → NUL-terminated
  f->point_size = pt;
}

FamoSkin SkinDefaultImpl() {
  FamoSkin s;
  std::memset(&s, 0, sizeof(s));
  s.size = static_cast<uint32_t>(sizeof(FamoSkin));

  s.layout_type = FAMO_LAYOUT_VERTICAL;

  // Famo shenda light palette. 0xAARRGGBB.
  s.text_color            = 0xFF6B6A64u;
  s.back_color            = 0xFFFBF9F5u;
  s.border_color          = 0x298E2447u;
  s.hilited_text_color    = 0xFFFBF9F5u;
  s.hilited_back_color    = 0xFFA82C53u;
  s.candidate_text_color  = 0xFF2A2622u;
  s.label_color           = 0xFF6B6A64u;
  s.comment_color         = 0xFF9A9387u;
  s.hilited_comment_color = 0xFFFBF9F5u;
  s.prevpage_color        = 0xFF9A9387u;
  s.nextpage_color        = 0xFF9A9387u;
  s.shadow_color          = 0x38000000u;
  s.card2_color           = 0xFFF3EFE7u;

  SetFace(&s.label_font,   "Segoe UI", 13.7f);
  SetFace(&s.text_font,    "Segoe UI", 19.0f);
  SetFace(&s.comment_font, "Segoe UI", 15.2f);

  s.margin_x          = 8;
  s.margin_y          = 8;
  // Band → list gap. Holds the hairline separator with ~6px of air either side;
  // 12 also lands on the 4dp grid both Material and Apple layouts are built on.
  s.spacing           = 12;
  s.candidate_spacing = 6;
  s.hilite_padding_x  = 8;
  s.hilite_padding_y  = 7;
  s.label_spacing     = 5;
  s.comment_spacing   = 8;
  s.round_corner      = 13;
  s.border            = 1;
  s.shadow_radius     = 16;
  s.shadow_offset_x   = 0;
  s.shadow_offset_y   = 8;
  s.min_width         = 76;  // vertical: base candidate size * 4 (macOS parity)
  s.min_height        = 0;
  s.max_width         = 0;  // unlimited
  s.max_height        = 0;
  s.status_icon_size  = 0;  // no status icon by default (composing panel)
  // 2px floor: 1px is the old hardcoded value and is thin enough to disappear on
  // high-DPI. The host overwrites this with SPI_GETCARETWIDTH when it has one.
  s.caret_width       = 2;
  s.show_preedit      = 1;
  s.preview_pages     = 0;
  s.preview_rows      = 2;

  return s;
}

FamoSkin SkinDefaultCpp() noexcept {
  try {
    return SkinDefaultImpl();
  } catch (...) {
    return {};
  }
}
}  // namespace

extern "C" FamoSkin FamoSkinDefault(void) noexcept {
  FAMO_C_ABI_SEH_RETURN(SkinDefaultCpp(), FamoSkin{});
}
