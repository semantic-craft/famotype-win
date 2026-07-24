#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace famo::fixture {

enum class ColorMode { kLight, kDark };
enum class SnapshotForm { kCompact, kExpanded, kVertical };
enum class Interaction { kSelected, kHover, kNeutral };

struct SkinPalette {
  std::string id;
  std::string display_name;
  ColorMode mode = ColorMode::kLight;
  uint32_t accent = 0;
  uint32_t accent_deep = 0;
  uint32_t on_accent = 0;
  uint32_t card = 0;
  uint32_t card2 = 0;
  uint32_t ink = 0;
  uint32_t ink2 = 0;
  uint32_t ink3 = 0;
};

struct CandidateFixture {
  int32_t id = 0;
  std::string label;
  std::string text;
  std::string comment;
  bool badge = false;
  std::string source;
  std::string provenance;
};

struct SnapshotFixture {
  std::string id;
  SnapshotForm form = SnapshotForm::kCompact;
  Interaction interaction = Interaction::kNeutral;
  std::string preedit;
  uint32_t selection_start_bytes = 0;
  uint32_t selection_end_bytes = 0;
  uint32_t caret_bytes = 0;
  uint32_t page_index = 0;
  uint32_t page_size = 0;
  bool has_prev_page = false;
  bool has_next_page = false;
  int32_t highlighted_index = 0;
  int32_t hovered_index = -1;
  std::string schema_id;
  std::vector<CandidateFixture> candidates;
};

struct FixtureManifest {
  uint32_t version = 0;
  std::vector<SkinPalette> skins;
  std::vector<SnapshotFixture> snapshots;

  const SkinPalette* FindSkin(const std::string& id, ColorMode mode) const;
  const SnapshotFixture* FindSnapshot(const std::string& id) const;
};

struct FixtureError {
  size_t line = 0;
  std::string message;
};

struct LoadResult {
  bool ok = false;
  FixtureManifest manifest;
  FixtureError error;
};

// Loads the versioned UTF-8 fixture contract used by Windows benchmarks and
// the macOS capture host. It never reads Rime/user state and never logs text.
LoadResult LoadCandidateFixtureManifest(const std::string& path);

}  // namespace famo::fixture
