#include "famo_fixture.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace famo::fixture {
namespace {

constexpr char kHeader[] = "# famo-candidate-fixtures-v1";

LoadResult Fail(size_t line, const std::string& message) {
  LoadResult result;
  result.error.line = line;
  result.error.message = message;
  return result;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t begin = 0;
  while (true) {
    const size_t end = line.find('\t', begin);
    fields.emplace_back(line.substr(begin, end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return fields;
}

template <typename T>
bool ParseInteger(std::string_view text, T* out) {
  if (!out || text.empty()) return false;
  T value{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) return false;
  *out = value;
  return true;
}

bool ParseBool(const std::string& text, bool* out) {
  if (text == "0") {
    *out = false;
    return true;
  }
  if (text == "1") {
    *out = true;
    return true;
  }
  return false;
}

bool ParseColor(std::string_view text, uint32_t* out) {
  if (!out || text.size() != 7 || text.front() != '#') return false;
  uint32_t rgb = 0;
  const char* begin = text.data() + 1;
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(begin, end, rgb, 16);
  if (parsed.ec != std::errc() || parsed.ptr != end) return false;
  *out = 0xFF000000u | rgb;
  return true;
}

bool ParseMode(const std::string& text, ColorMode* out) {
  if (text == "light") {
    *out = ColorMode::kLight;
    return true;
  }
  if (text == "dark") {
    *out = ColorMode::kDark;
    return true;
  }
  return false;
}

bool ParseForm(const std::string& text, SnapshotForm* out) {
  if (text == "compact") {
    *out = SnapshotForm::kCompact;
    return true;
  }
  if (text == "expanded") {
    *out = SnapshotForm::kExpanded;
    return true;
  }
  if (text == "vertical") {
    *out = SnapshotForm::kVertical;
    return true;
  }
  return false;
}

bool ParseInteraction(const std::string& text, Interaction* out) {
  if (text == "selected") {
    *out = Interaction::kSelected;
    return true;
  }
  if (text == "hover") {
    *out = Interaction::kHover;
    return true;
  }
  if (text == "neutral") {
    *out = Interaction::kNeutral;
    return true;
  }
  return false;
}

bool IsValidProvenance(const std::string& text) {
  return text.empty() || text == "personal" || text == "project" ||
         text == "rule" || text == "verified" || text == "ai";
}

}  // namespace

const SkinPalette* FixtureManifest::FindSkin(const std::string& id,
                                             ColorMode mode) const {
  for (const auto& skin : skins) {
    if (skin.id == id && skin.mode == mode) return &skin;
  }
  return nullptr;
}

const SnapshotFixture* FixtureManifest::FindSnapshot(
    const std::string& id) const {
  for (const auto& snapshot : snapshots) {
    if (snapshot.id == id) return &snapshot;
  }
  return nullptr;
}

LoadResult LoadCandidateFixtureManifest(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return Fail(0, "cannot open fixture manifest");

  FixtureManifest manifest;
  std::unordered_map<std::string, size_t> snapshot_indexes;
  std::vector<size_t> snapshot_lines;
  struct SkinPairState {
    uint32_t modes = 0;
    size_t line = 0;
  };
  std::unordered_map<std::string, SkinPairState> skin_pairs;
  std::string line;
  size_t line_number = 0;
  bool saw_header = false;
  bool saw_version = false;

  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line_number == 1 && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    if (line.empty()) continue;
    if (line.front() == '#') {
      if (line_number == 1 && line == kHeader) saw_header = true;
      continue;
    }

    const auto fields = SplitTabs(line);
    if (fields.empty()) continue;

    if (fields[0] == "meta") {
      if (fields.size() != 3 || fields[1] != "version" || saw_version ||
          !ParseInteger(fields[2], &manifest.version) || manifest.version != 1u) {
        return Fail(line_number, "invalid or duplicate fixture version");
      }
      saw_version = true;
      continue;
    }

    if (fields[0] == "skin") {
      if (fields.size() != 12) return Fail(line_number, "skin needs 12 columns");
      SkinPalette skin;
      skin.id = fields[1];
      skin.display_name = fields[2];
      if (skin.id.empty() || skin.display_name.empty() ||
          !ParseMode(fields[3], &skin.mode) ||
          !ParseColor(fields[4], &skin.accent) ||
          !ParseColor(fields[5], &skin.accent_deep) ||
          !ParseColor(fields[6], &skin.on_accent) ||
          !ParseColor(fields[7], &skin.card) ||
          !ParseColor(fields[8], &skin.card2) ||
          !ParseColor(fields[9], &skin.ink) ||
          !ParseColor(fields[10], &skin.ink2) ||
          !ParseColor(fields[11], &skin.ink3)) {
        return Fail(line_number, "invalid skin record");
      }
      if (manifest.FindSkin(skin.id, skin.mode))
        return Fail(line_number, "duplicate skin mode");
      auto& pair = skin_pairs[skin.id];
      pair.modes |= skin.mode == ColorMode::kLight ? 1u : 2u;
      pair.line = line_number;
      manifest.skins.push_back(std::move(skin));
      continue;
    }

    if (fields[0] == "snapshot") {
      if (fields.size() != 15)
        return Fail(line_number, "snapshot needs 15 columns");
      SnapshotFixture snapshot;
      snapshot.id = fields[1];
      if (snapshot.id.empty() || snapshot_indexes.count(snapshot.id) != 0 ||
          !ParseForm(fields[2], &snapshot.form) ||
          !ParseInteraction(fields[3], &snapshot.interaction) ||
          !ParseInteger(fields[5], &snapshot.selection_start_bytes) ||
          !ParseInteger(fields[6], &snapshot.selection_end_bytes) ||
          !ParseInteger(fields[7], &snapshot.caret_bytes) ||
          !ParseInteger(fields[8], &snapshot.page_index) ||
          !ParseInteger(fields[9], &snapshot.page_size) ||
          !ParseBool(fields[10], &snapshot.has_prev_page) ||
          !ParseBool(fields[11], &snapshot.has_next_page) ||
          !ParseInteger(fields[12], &snapshot.highlighted_index) ||
          !ParseInteger(fields[13], &snapshot.hovered_index)) {
        return Fail(line_number, "invalid snapshot record");
      }
      snapshot.preedit = fields[4];
      snapshot.schema_id = fields[14];
      snapshot_indexes.emplace(snapshot.id, manifest.snapshots.size());
      manifest.snapshots.push_back(std::move(snapshot));
      snapshot_lines.push_back(line_number);
      continue;
    }

    if (fields[0] == "candidate") {
      if (fields.size() != 9)
        return Fail(line_number, "candidate needs 9 columns");
      const auto found = snapshot_indexes.find(fields[1]);
      if (found == snapshot_indexes.end())
        return Fail(line_number, "candidate references unknown snapshot");
      CandidateFixture candidate;
      if (!ParseInteger(fields[2], &candidate.id) ||
          !ParseBool(fields[6], &candidate.badge)) {
        return Fail(line_number, "invalid candidate record");
      }
      candidate.label = fields[3];
      candidate.text = fields[4];
      candidate.comment = fields[5];
      candidate.source = fields[7];
      candidate.provenance = fields[8];
      if (!IsValidProvenance(candidate.provenance))
        return Fail(line_number, "invalid candidate provenance");
      auto& candidates = manifest.snapshots[found->second].candidates;
      for (const auto& existing : candidates) {
        if (existing.id == candidate.id)
          return Fail(line_number, "duplicate candidate id in snapshot");
      }
      candidates.push_back(std::move(candidate));
      continue;
    }

    return Fail(line_number, "unknown fixture record");
  }

  if (!saw_header) return Fail(1, "missing fixture v1 header");
  if (!saw_version) return Fail(0, "missing fixture version");
  for (const auto& entry : skin_pairs) {
    if (entry.second.modes != 3u) {
      return Fail(entry.second.line,
                  "each skin must define both light and dark modes");
    }
  }
  for (size_t i = 0; i < manifest.snapshots.size(); ++i) {
    const auto& snapshot = manifest.snapshots[i];
    const int32_t count = static_cast<int32_t>(snapshot.candidates.size());
    if (count == 0 || snapshot.highlighted_index < 0 ||
        snapshot.highlighted_index >= count) {
      return Fail(snapshot_lines[i],
                  "snapshot highlight must reference an existing candidate");
    }
    if (snapshot.hovered_index < -1 || snapshot.hovered_index >= count) {
      return Fail(snapshot_lines[i],
                  "snapshot hover must reference an existing candidate or -1");
    }
  }

  LoadResult result;
  result.ok = true;
  result.manifest = std::move(manifest);
  return result;
}

}  // namespace famo::fixture
