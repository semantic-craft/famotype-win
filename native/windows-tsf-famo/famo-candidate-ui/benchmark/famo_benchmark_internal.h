#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../famo_candidate_ui.h"
#include "famo_benchmark.h"
#include "famo_benchmark_counters.h"
#include "famo_fixture.h"

namespace famo::benchmark::internal {

struct StageSamples {
  std::vector<double> snapshot_prepare;
  std::vector<double> layout;
  std::vector<double> paint;
  std::vector<double> window_submit;
  std::vector<double> window_move;
  std::vector<double> total;
};

struct ResourceSnapshot {
  uint64_t gdi_objects = 0;
  uint64_t working_set_bytes = 0;
  uint64_t peak_working_set_bytes = 0;
};

struct MatrixResult {
  std::string fixture_id;
  std::string skin_id;
  std::string mode;
  std::string layout;
  std::string interaction;
  uint32_t dpi = 96;
  uint32_t iterations = 0;
  StageSamples cold;
  StageSamples warm;
  ResourceSnapshot before;
  ResourceSnapshot after;
  uint64_t host_surface_creates = 0;
  FamoBenchmarkRenderCounters render_creates{};
  uint64_t visible_pixel_count = 0;
  std::string capture_path;
  std::string status = "ok";
  std::string error;
};

struct OwnedCandidate {
  std::string label;
  std::string text;
  std::string comment;
};

class OwnedView {
 public:
  explicit OwnedView(const fixture::SnapshotFixture& snapshot);
  const FamoCompositionView* get() const { return &view_; }

 private:
  std::string preedit_;
  std::string schema_id_;
  std::vector<OwnedCandidate> strings_;
  std::vector<FamoCandidate> candidates_;
  FamoCompositionView view_{};
};

std::string ModeName(fixture::ColorMode mode);
std::string FormName(fixture::SnapshotForm form);
std::string InteractionName(fixture::Interaction interaction);
FamoSkin MakeSkin(const fixture::SkinPalette& palette,
                  const fixture::SnapshotFixture& snapshot);

int RunMatrixItem(const fixture::SnapshotFixture& snapshot,
                  const fixture::SkinPalette& palette, const Options& options,
                  const std::filesystem::path& output, MatrixResult* result,
                  std::string* error);

bool OutputDirectoryReady(const Options& options, std::string* error);
bool WriteArtifacts(const std::filesystem::path& output,
                    const std::vector<MatrixResult>& matrix,
                    uint32_t manifest_version);

}  // namespace famo::benchmark::internal
