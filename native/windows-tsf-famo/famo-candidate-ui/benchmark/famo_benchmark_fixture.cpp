#include "famo_benchmark_internal.h"

namespace famo::benchmark::internal {
namespace {

FamoUtf8String StringView(const std::string& value) {
  FamoUtf8String result{};
  result.size = static_cast<uint32_t>(sizeof(result));
  result.data = value.empty() ? nullptr : value.data();
  result.length_bytes = static_cast<uint32_t>(value.size());
  return result;
}

}  // namespace

OwnedView::OwnedView(const fixture::SnapshotFixture& snapshot)
    : preedit_(snapshot.preedit), schema_id_(snapshot.schema_id) {
  strings_.reserve(snapshot.candidates.size());
  for (const auto& candidate : snapshot.candidates) {
    strings_.push_back({candidate.label, candidate.text, candidate.comment});
  }
  candidates_.resize(snapshot.candidates.size());
  for (size_t i = 0; i < snapshot.candidates.size(); ++i) {
    auto& out = candidates_[i];
    out = {};
    out.size = static_cast<uint32_t>(sizeof(out));
    out.label = StringView(strings_[i].label);
    out.text = StringView(strings_[i].text);
    out.comment = StringView(strings_[i].comment);
    out.flags = i == 0 ? FAMO_CANDIDATE_FLAG_DEFAULT : 0u;
  }

  view_ = {};
  view_.size = static_cast<uint32_t>(sizeof(view_));
  view_.preedit = StringView(preedit_);
  view_.candidates = candidates_.data();
  view_.candidate_count = static_cast<uint32_t>(candidates_.size());
  view_.highlighted_index = static_cast<uint32_t>(
      snapshot.interaction == fixture::Interaction::kHover &&
              snapshot.hovered_index >= 0
          ? snapshot.hovered_index
          : snapshot.highlighted_index);
  view_.page_index = snapshot.page_index;
  view_.page_size = snapshot.page_size;
  view_.state_flags = FAMO_COMPOSITION_HAS_CANDIDATES;
  if (!preedit_.empty()) view_.state_flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  view_.preedit_sel_start = snapshot.selection_start_bytes;
  view_.preedit_sel_end = snapshot.selection_end_bytes;
  view_.preedit_cursor_pos = snapshot.caret_bytes;
  view_.schema_id = StringView(schema_id_);
  view_.schema_name = StringView(schema_id_);
  view_.status_flags = FAMO_STATUS_COMPOSING;
  view_.is_last_page = snapshot.has_next_page ? 0u : 1u;
}

std::string ModeName(fixture::ColorMode mode) {
  return mode == fixture::ColorMode::kDark ? "dark" : "light";
}

std::string FormName(fixture::SnapshotForm form) {
  switch (form) {
    case fixture::SnapshotForm::kCompact: return "compact";
    case fixture::SnapshotForm::kExpanded: return "expanded";
    case fixture::SnapshotForm::kVertical: return "vertical";
  }
  return "unknown";
}

std::string InteractionName(fixture::Interaction interaction) {
  switch (interaction) {
    case fixture::Interaction::kSelected: return "selected";
    case fixture::Interaction::kHover: return "hover";
    case fixture::Interaction::kNeutral: return "neutral";
  }
  return "unknown";
}

FamoSkin MakeSkin(const fixture::SkinPalette& palette,
                  const fixture::SnapshotFixture& snapshot) {
  FamoSkin skin = FamoSkinDefault();
  skin.layout_type = snapshot.form == fixture::SnapshotForm::kCompact
                         ? FAMO_LAYOUT_HORIZONTAL
                         : FAMO_LAYOUT_VERTICAL;
  skin.min_width = snapshot.form == fixture::SnapshotForm::kCompact ? 210 : 150;
  skin.text_color = palette.ink2;
  skin.back_color = palette.card;
  skin.border_color = (palette.accent_deep & 0x00ffffffu) | 0x29000000u;
  skin.hilited_text_color = palette.on_accent;
  skin.hilited_back_color = palette.accent;
  skin.candidate_text_color = palette.ink;
  skin.label_color = palette.ink2;
  skin.comment_color = palette.ink3;
  skin.hilited_comment_color = palette.on_accent;
  skin.prevpage_color = palette.ink3;
  skin.nextpage_color = palette.ink3;
  skin.shadow_color = palette.mode == fixture::ColorMode::kDark
                          ? 0x73000000u
                          : 0x29000000u;
  return skin;
}

}  // namespace famo::benchmark::internal
