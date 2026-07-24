// Candidate Window V2 shared-fixture contract.
//
// This test observes the public benchmark loader only. It does not know parser
// internals and does not link fixture parsing into the production renderer.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../benchmark/famo_fixture.h"

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      return 1;                                                           \
    }                                                                     \
  } while (0)

namespace {

std::filesystem::path WriteTempFixture(const char* name,
                                       const std::string& contents) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
  return path;
}

std::string ValidSkinPair() {
  return
      "skin\tshenda\t荔园红\tlight\t#A82C53\t#8E2447\t#FBF9F5\t"
      "#FBF9F5\t#F3EFE7\t#2A2622\t#6B6A64\t#9A9387\n"
      "skin\tshenda\t荔园红\tdark\t#E06A8E\t#C24E72\t#1A1816\t"
      "#262321\t#211E1C\t#ECE4D8\t#A89E90\t#766D62\n";
}

}  // namespace

int main() {
  const auto loaded = famo::fixture::LoadCandidateFixtureManifest(
      FAMO_FIXTURE_MANIFEST_PATH);
  CHECK(loaded.ok);
  CHECK(loaded.manifest.version == 1u);
  CHECK(loaded.manifest.skins.size() == 8u);
  CHECK(loaded.manifest.snapshots.size() == 6u);

  const auto* shenda_light =
      loaded.manifest.FindSkin("shenda", famo::fixture::ColorMode::kLight);
  CHECK(shenda_light != nullptr);
  CHECK(shenda_light->display_name == "荔园红");
  CHECK(shenda_light->accent == 0xFFA82C53u);
  CHECK(shenda_light->card == 0xFFFBF9F5u);

  const auto* legal = loaded.manifest.FindSnapshot("expanded-legal-source");
  CHECK(legal != nullptr);
  CHECK(legal->form == famo::fixture::SnapshotForm::kExpanded);
  CHECK(legal->candidates.size() >= 2u);
  CHECK(legal->candidates[1].badge);
  CHECK(legal->candidates[1].source == "《中华人民共和国民法典》第五百零九条");
  CHECK(legal->candidates[1].provenance == "verified");

  const auto* emoji = loaded.manifest.FindSnapshot("emoji-astral");
  CHECK(emoji != nullptr);
  CHECK(!emoji->candidates.empty());
  CHECK(emoji->candidates[0].text == "👋🏽");

  const auto incomplete_path = WriteTempFixture(
      "famo-fixtures-incomplete-skin.tsv",
      "# famo-candidate-fixtures-v1\n"
      "meta\tversion\t1\n"
      "skin\tshenda\t荔园红\tlight\t#A82C53\t#8E2447\t#FBF9F5\t"
      "#FBF9F5\t#F3EFE7\t#2A2622\t#6B6A64\t#9A9387\n");
  const auto incomplete = famo::fixture::LoadCandidateFixtureManifest(
      incomplete_path.string());
  std::filesystem::remove(incomplete_path);
  CHECK(!incomplete.ok);
  CHECK(incomplete.error.line == 3u);
  CHECK(incomplete.error.message.find("light and dark") != std::string::npos);

  const auto duplicate_candidate_path = WriteTempFixture(
      "famo-fixtures-duplicate-candidate.tsv",
      std::string("# famo-candidate-fixtures-v1\nmeta\tversion\t1\n") +
          ValidSkinPair() +
          "snapshot\tone\tcompact\tselected\tx\t0\t0\t1\t0\t5\t0\t0\t0\t-1\ttest\n"
          "candidate\tone\t0\t1\t甲\t\t0\t\t\n"
          "candidate\tone\t0\t2\t乙\t\t0\t\t\n");
  const auto duplicate_candidate =
      famo::fixture::LoadCandidateFixtureManifest(
          duplicate_candidate_path.string());
  std::filesystem::remove(duplicate_candidate_path);
  CHECK(!duplicate_candidate.ok);
  CHECK(duplicate_candidate.error.line == 7u);
  CHECK(duplicate_candidate.error.message.find("duplicate candidate") !=
        std::string::npos);

  const auto bad_provenance_path = WriteTempFixture(
      "famo-fixtures-bad-provenance.tsv",
      std::string("# famo-candidate-fixtures-v1\nmeta\tversion\t1\n") +
          ValidSkinPair() +
          "snapshot\tone\tcompact\tselected\tx\t0\t0\t1\t0\t5\t0\t0\t0\t-1\ttest\n"
          "candidate\tone\t0\t1\t甲\t\t0\t\tuntrusted\n");
  const auto bad_provenance = famo::fixture::LoadCandidateFixtureManifest(
      bad_provenance_path.string());
  std::filesystem::remove(bad_provenance_path);
  CHECK(!bad_provenance.ok);
  CHECK(bad_provenance.error.line == 6u);
  CHECK(bad_provenance.error.message.find("provenance") != std::string::npos);

  const auto dangling_candidate_path = WriteTempFixture(
      "famo-fixtures-dangling-candidate.tsv",
      std::string("# famo-candidate-fixtures-v1\nmeta\tversion\t1\n") +
          ValidSkinPair() +
          "candidate\tmissing\t0\t1\t甲\t\t0\t\t\n");
  const auto dangling_candidate = famo::fixture::LoadCandidateFixtureManifest(
      dangling_candidate_path.string());
  std::filesystem::remove(dangling_candidate_path);
  CHECK(!dangling_candidate.ok);
  CHECK(dangling_candidate.error.line == 5u);
  CHECK(dangling_candidate.error.message.find("unknown snapshot") !=
        std::string::npos);

  const auto bad_columns_path = WriteTempFixture(
      "famo-fixtures-bad-columns.tsv",
      std::string("# famo-candidate-fixtures-v1\nmeta\tversion\t1\n") +
          ValidSkinPair() +
          "snapshot\tone\tcompact\n");
  const auto bad_columns = famo::fixture::LoadCandidateFixtureManifest(
      bad_columns_path.string());
  std::filesystem::remove(bad_columns_path);
  CHECK(!bad_columns.ok);
  CHECK(bad_columns.error.line == 5u);
  CHECK(bad_columns.error.message.find("15 columns") != std::string::npos);

  const auto bad_highlight_path = WriteTempFixture(
      "famo-fixtures-bad-highlight.tsv",
      std::string("# famo-candidate-fixtures-v1\nmeta\tversion\t1\n") +
          ValidSkinPair() +
          "snapshot\tone\tcompact\tselected\tx\t0\t0\t1\t0\t5\t0\t0\t1\t-1\ttest\n"
          "candidate\tone\t0\t1\t甲\t\t0\t\t\n");
  const auto bad_highlight = famo::fixture::LoadCandidateFixtureManifest(
      bad_highlight_path.string());
  std::filesystem::remove(bad_highlight_path);
  CHECK(!bad_highlight.ok);
  CHECK(bad_highlight.error.line == 5u);
  CHECK(bad_highlight.error.message.find("highlight") != std::string::npos);

  std::printf("fixture_contract: OK\n");
  return 0;
}
