#include "famo_benchmark_internal.h"

#include <windows.h>

#include <memory>

#include "famo_benchmark_surface.h"

namespace famo::benchmark::internal {
namespace {

class QpcClock {
 public:
  QpcClock() { QueryPerformanceFrequency(&frequency_); }
  int64_t Now() const {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
  }
  double Micros(int64_t begin, int64_t end) const {
    return static_cast<double>(end - begin) * 1000000.0 /
           static_cast<double>(frequency_.QuadPart);
  }

 private:
  LARGE_INTEGER frequency_{};
};

bool ForcedRendererFailure() {
  char value[2]{};
  return GetEnvironmentVariableA("FAMO_BENCHMARK_FORCE_RENDERER_FAILURE", value,
                                 static_cast<DWORD>(std::size(value))) > 0 &&
         value[0] == '1';
}

bool RectWithinContent(const FamoRect& rect, const FamoSize& content) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return true;
  return rect.left >= 0 && rect.top >= 0 && rect.right <= content.cx &&
         rect.bottom <= content.cy;
}

bool LayoutWithinContent(const FamoLayoutResult& layout) {
  if (layout.content_size.cx <= 0 || layout.content_size.cy <= 0 ||
      layout.candidate_count > FAMO_MAX_LAID_CANDIDATES) {
    return false;
  }
  if (!RectWithinContent(layout.preedit, layout.content_size) ||
      !RectWithinContent(layout.aux, layout.content_size) ||
      !RectWithinContent(layout.highlight, layout.content_size) ||
      !RectWithinContent(layout.status_icon, layout.content_size) ||
      !RectWithinContent(layout.prev_page, layout.content_size) ||
      !RectWithinContent(layout.next_page, layout.content_size)) {
    return false;
  }
  for (uint32_t i = 0; i < layout.candidate_count; ++i) {
    const FamoCandidateRects& candidate = layout.candidates[i];
    if (!RectWithinContent(candidate.bounds, layout.content_size) ||
        !RectWithinContent(candidate.label, layout.content_size) ||
        !RectWithinContent(candidate.text, layout.content_size) ||
        !RectWithinContent(candidate.comment, layout.content_size)) {
      return false;
    }
  }
  return true;
}

bool RunFrame(const fixture::SnapshotFixture& snapshot, const FamoSkin& skin,
              uint32_t dpi, FamoTextResources* resources, DibSurface* surface,
              LayeredHost* host, uint64_t* host_surface_creates,
              const QpcClock& clock, StageSamples* samples,
              std::string* error) {
  const int64_t frame_begin = clock.Now();
  int64_t begin = frame_begin;
  auto owned = std::make_unique<OwnedView>(snapshot);
  int64_t end = clock.Now();
  samples->snapshot_prepare.push_back(clock.Micros(begin, end));

  FamoLayoutInput input{};
  input.size = static_cast<uint32_t>(sizeof(input));
  input.caret_rect = {400, 300, 402, 320};
  input.work_area = {0, 0, 1920, 1080};
  input.dpi = dpi;
  input.measure = &FamoTextMeasure;
  input.measure_user = resources;

  FamoLayoutResult layout{};
  begin = clock.Now();
  const int32_t layout_result =
      FamoCandidateUiLayout(owned->get(), &skin, &input, &layout);
  end = clock.Now();
  samples->layout.push_back(clock.Micros(begin, end));
  if (layout_result != FAMO_UI_OK) {
    *error = "layout failed";
    return false;
  }
  if (!LayoutWithinContent(layout)) {
    *error = "layout rect exceeds content bounds";
    return false;
  }

  const int margin = layout.shadow_margin > 0 ? layout.shadow_margin : 0;
  const int width = layout.content_size.cx + margin * 2;
  const int height = layout.content_size.cy + margin * 2;
  if (!surface->Ensure(width, height, host_surface_creates) || !host->Ensure()) {
    *error = "synthetic surface or layered host creation failed";
    return false;
  }
  surface->Clear();

  begin = clock.Now();
  const int32_t paint_result = FamoCandidateUiPaint(
      owned->get(), &skin, &input, &layout, resources, surface->dc());
  end = clock.Now();
  samples->paint.push_back(clock.Micros(begin, end));
  if (paint_result != FAMO_UI_OK) {
    *error = "paint failed";
    return false;
  }

  const int x = layout.origin_x - margin;
  const int y = layout.origin_y - margin;
  begin = clock.Now();
  const bool submitted = host->Submit(*surface, x, y);
  end = clock.Now();
  samples->window_submit.push_back(clock.Micros(begin, end));
  if (!submitted) {
    *error = "UpdateLayeredWindow failed";
    return false;
  }

  begin = clock.Now();
  const bool moved = host->Move(x + 1, y);
  end = clock.Now();
  samples->window_move.push_back(clock.Micros(begin, end));
  if (!moved) {
    *error = "SetWindowPos failed";
    return false;
  }
  samples->total.push_back(clock.Micros(frame_begin, end));
  return true;
}

}  // namespace

int RunMatrixItem(const fixture::SnapshotFixture& snapshot,
                  const fixture::SkinPalette& palette, const Options& options,
                  const std::filesystem::path& output, MatrixResult* result,
                  std::string* error) {
  result->before = ReadResources();
  if (ForcedRendererFailure()) {
    *error = "forced renderer failure";
    result->status = "error";
    result->error = *error;
    result->after = ReadResources();
    return 3;
  }

  const FamoSkin skin = MakeSkin(palette, snapshot);
  FamoTextResources* raw_resources =
      FamoTextResourcesCreate(&skin, options.dpi);
  if (!raw_resources) {
    *error = "text resource creation failed";
    result->status = "error";
    result->error = *error;
    result->after = ReadResources();
    return 3;
  }

  FamoBenchmarkRenderCountersReset();
  int code = 0;
  {
    std::unique_ptr<FamoTextResources, decltype(&FamoTextResourcesDestroy)>
        text_resources(raw_resources, &FamoTextResourcesDestroy);
    DibSurface surface;
    LayeredHost host;
    QpcClock clock;

    if (!RunFrame(snapshot, skin, options.dpi, text_resources.get(), &surface,
                  &host, &result->host_surface_creates, clock, &result->cold,
                  error)) {
      code = 3;
    }
    for (uint32_t i = 0; code == 0 && i < options.iterations; ++i) {
      if (!RunFrame(snapshot, skin, options.dpi, text_resources.get(), &surface,
                    &host, &result->host_surface_creates, clock, &result->warm,
                    error)) {
        code = 3;
      }
    }
    if (code == 0) {
      result->visible_pixel_count = surface.VisiblePixelCount();
      if (result->visible_pixel_count == 0) {
        *error = "paint produced no visible pixels";
        code = 3;
      } else if (!SavePng(surface, output / result->capture_path)) {
        *error = "PNG capture failed";
        code = 4;
      }
    }
  }

  result->render_creates = FamoBenchmarkRenderCountersSnapshot();
  result->after = ReadResources();
  if (code != 0) {
    result->status = "error";
    result->error = *error;
  }
  return code;
}

}  // namespace famo::benchmark::internal
