#include "famo_benchmark.h"

#include <filesystem>
#include <string>
#include <vector>

#include "famo_benchmark_internal.h"
#include "famo_benchmark_surface.h"

namespace famo::benchmark {

int Run(const Options& options, std::string* error) {
  using namespace internal;
  if (!error) return 2;
  const auto loaded = fixture::LoadCandidateFixtureManifest(options.fixtures_path);
  if (!loaded.ok) {
    *error = "fixture contract failed at line " +
             std::to_string(loaded.error.line) + ": " + loaded.error.message;
    return 2;
  }

  std::vector<const fixture::SnapshotFixture*> snapshots;
  for (const auto& snapshot : loaded.manifest.snapshots) {
    if (options.all_fixtures || snapshot.id == options.fixture_id)
      snapshots.push_back(&snapshot);
  }
  std::vector<const fixture::SkinPalette*> palettes;
  for (const auto& palette : loaded.manifest.skins) {
    if (!options.all_skins && palette.id != options.skin_id) continue;
    if (!options.all_modes && palette.mode != options.color_mode) continue;
    palettes.push_back(&palette);
  }
  if (snapshots.empty() || palettes.empty()) {
    *error = "requested fixture or skin mode does not exist";
    return 2;
  }
  if (!OutputDirectoryReady(options, error)) return 4;

  GdiplusSession gdiplus;
  if (!gdiplus.ok()) {
    *error = "GDI+ startup failed";
    return 3;
  }

  const std::filesystem::path output(options.output_directory);
  std::vector<MatrixResult> matrix;
  matrix.reserve(snapshots.size() * palettes.size());
  for (const auto* snapshot : snapshots) {
    for (const auto* palette : palettes) {
      MatrixResult result;
      result.fixture_id = snapshot->id;
      result.skin_id = palette->id;
      result.mode = ModeName(palette->mode);
      result.layout = FormName(snapshot->form);
      result.interaction = InteractionName(snapshot->interaction);
      result.dpi = options.dpi;
      result.iterations = options.iterations;
      result.capture_path = snapshot->id + "__" + palette->id + "__" +
                            result.mode + "__" +
                            std::to_string(options.dpi) + "dpi.png";

      const int code = RunMatrixItem(*snapshot, *palette, options, output,
                                     &result, error);
      matrix.push_back(std::move(result));
      if (code != 0) {
        if (!WriteArtifacts(output, matrix, loaded.manifest.version)) {
          *error = "cannot write failed benchmark JSON artifacts";
          return 4;
        }
        return code;
      }
    }
  }

  if (!WriteArtifacts(output, matrix, loaded.manifest.version)) {
    *error = "cannot write benchmark JSON artifacts";
    return 4;
  }
  return 0;
}

}  // namespace famo::benchmark
