#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "candidate_skin.h"
#include "candidate_window.h"

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #value, __FILE__,   \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace {

std::shared_ptr<RuntimeSnapshot> VisibleSnapshot(uint64_t sequence = 3) {
  auto snapshot = std::make_shared<RuntimeSnapshot>();
  snapshot->correlation = {1, 2, 3, 4, 5, sequence};
  snapshot->composition.handled = true;
  snapshot->composition.preedit = "ni";
  snapshot->composition.candidates = {
      Candidate{"\xe4\xbd\xa0", "", "1", 0, 0},
      Candidate{"\xe5\xb0\xbc", "", "2", 0, 0}};
  snapshot->composition.page_size = 8;
  snapshot->composition.is_last_page = 1;
  snapshot->ui_state = {{400, 300, 402, 320},
                        {-1920, 0, 1920, 1080},
                        192,
                        true,
                        true,
                        true};
  snapshot->composition_sequence = sequence - 1;
  snapshot->ui_sequence = sequence;
  return snapshot;
}

struct WindowProbe {
  HWND window = nullptr;
  RECT rect{};
  LONG_PTR ex_style = 0;
  HCURSOR cursor = nullptr;
  bool visible = false;
};

BOOL CALLBACK FindCandidate(HWND window, LPARAM parameter) {
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != GetCurrentProcessId())
    return TRUE;
  wchar_t name[64]{};
  GetClassNameW(window, name, static_cast<int>(std::size(name)));
  if (std::wstring_view(name) != L"FamoRuntimeCandidateWindow")
    return TRUE;
  auto *probe = reinterpret_cast<WindowProbe *>(parameter);
  probe->window = window;
  probe->visible = IsWindowVisible(window) != FALSE;
  probe->ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  probe->cursor =
      reinterpret_cast<HCURSOR>(GetClassLongPtrW(window, GCLP_HCURSOR));
  GetWindowRect(window, &probe->rect);
  return FALSE;
}

WindowProbe Probe() {
  WindowProbe result;
  EnumWindows(&FindCandidate, reinterpret_cast<LPARAM>(&result));
  return result;
}

bool WaitForVisibility(bool visible, WindowProbe *result = nullptr) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    WindowProbe probe = Probe();
    if (probe.window && probe.visible == visible) {
      if (result)
        *result = probe;
      return true;
    }
    Sleep(1);
  }
  return false;
}

bool PreviewRowsMapToAbsoluteCandidateIndexes() {
  FamoLayoutResult layout{};
  layout.preview_candidate_count = 3;
  layout.preview_candidates[0].bounds = {10, 20, 30, 40};
  layout.preview_candidates[1].bounds = {30, 20, 50, 40};
  layout.preview_candidates[2].bounds = {10, 40, 30, 60};
  PreviewSelection selection;
  CHECK(PreviewSelectionAt(layout, 35, 25, 0, 2, &selection));
  CHECK(selection.absolute_index == 3);
  CHECK(PreviewSelectionAt(layout, 15, 45, 0, 2, &selection));
  CHECK(selection.absolute_index == 4);
  CHECK(PreviewSelectionAt(layout, 15, 25, 4, 2, &selection));
  CHECK(selection.absolute_index == 10);
  CHECK(!PreviewSelectionAt(layout, 5, 5, 0, 2, &selection));
  CHECK(!PreviewSelectionAt(layout, 15, 25, 0, 0, &selection));
  return true;
}

bool ScrollTransitionIsBoundedAndOptional() {
  FamoLayoutResult previous{};
  previous.content_size = {240, 96};
  previous.shadow_margin = 12;
  previous.candidate_count = 2;
  previous.candidates[0].bounds = {8, 15, 58, 35};
  previous.candidates[1].bounds = {64, 15, 114, 35};
  previous.highlight = {8, 11, 58, 37};
  previous.preview_candidate_count = 4;
  previous.preview_candidates[0].bounds = {8, 40, 58, 60};
  previous.preview_candidates[1].bounds = {64, 40, 114, 60};
  previous.preview_candidates[2].bounds = {8, 65, 58, 85};
  previous.preview_candidates[3].bounds = {64, 65, 114, 85};
  FamoLayoutResult next = previous;

  ScrollTransitionPlan plan;
  CHECK(PlanScrollTransition(previous, next, 4, 5, true, &plan));
  CHECK(plan.direction == 1);
  CHECK(plan.row_step == 25);
  CHECK(plan.clip.left == 0 && plan.clip.top == 11 &&
        plan.clip.right == 240 && plan.clip.bottom == 85);
  CHECK(ScrollTransitionOffset(0, plan.row_step) == 0);
  const int32_t halfway = ScrollTransitionOffset(
      kCandidateScrollTransitionMs / 2, plan.row_step);
  CHECK(halfway > 0 && halfway < plan.row_step);
  CHECK(ScrollTransitionOffset(kCandidateScrollTransitionMs, plan.row_step) ==
        plan.row_step);

  CHECK(PlanScrollTransition(previous, next, 5, 4, true, &plan));
  CHECK(plan.direction == -1);
  CHECK(!PlanScrollTransition(previous, next, 4, 5, false, &plan));
  CHECK(!PlanScrollTransition(previous, next, 4, 6, true, &plan));
  next.content_size.cy++;
  CHECK(!PlanScrollTransition(previous, next, 4, 5, true, &plan));
  return true;
}

template <typename Predicate>
bool WaitForCounters(const CandidateWindow &window, Predicate predicate) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (predicate(window.counters()))
      return true;
    Sleep(1);
  }
  return false;
}

bool PrewarmCompletesBeforeReturn() {
  CandidateWindow window;
  const auto cold_started = std::chrono::steady_clock::now();
  CHECK(window.Start());
  CHECK(window.Prewarm());
  const auto renderer_ready = std::chrono::steady_clock::now();
  auto visible = VisibleSnapshot();
  const auto published = std::chrono::steady_clock::now();
  window.Publish(visible);
  CHECK(WaitForVisibility(true));
  const double cold_ready_ms =
      std::chrono::duration<double, std::milli>(renderer_ready - cold_started)
          .count();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - published)
          .count();
  std::printf("candidate_prewarm_boundary: cold_worker_resource_ready_ms=%.3f "
              "visible_publish_to_frame_ms=%.3f target_dpi=192\n",
              cold_ready_ms, elapsed_ms);
  window.Stop();
  CHECK(elapsed_ms <= 50.0);
  return true;
}

bool HiddenHighDpiStateDoesNotDelayFirstVisible() {
  CandidateWindow window;
  CHECK(window.Start());
  CHECK(window.Prewarm());
  auto hidden = VisibleSnapshot(10);
  hidden->revision = 1;
  hidden->ui_state.show_allowed = false;
  window.Publish(hidden);
  auto visible = VisibleSnapshot(11);
  visible->revision = 2;
  const auto published = std::chrono::steady_clock::now();
  window.Publish(visible);
  CHECK(WaitForVisibility(true));
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - published)
          .count();
  std::printf("candidate_hidden_high_dpi: visible_publish_to_frame_ms=%.3f "
              "target_dpi=192\n",
              elapsed_ms);
  window.Stop();
  CHECK(elapsed_ms <= 50.0);
  return true;
}

bool InlineHostPreeditStillShowsPanelHeader() {
  CandidateWindow window;
  CHECK(window.Start());
  CHECK(window.Prewarm());
  auto shown = VisibleSnapshot(30);
  shown->revision = 1;
  shown->composition.state_flags |= kHostInlinePreedit;
  shown->composition.preedit_cursor_pos = 2;
  window.Publish(shown);
  WindowProbe probe;
  CHECK(WaitForVisibility(true, &probe));
  const LONG shown_height = probe.rect.bottom - probe.rect.top;
  const uint64_t full_before = window.counters().full;

  FamoSkin hidden_skin = FamoSkinDefault();
  hidden_skin.show_preedit = 0;
  auto hidden_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::make_shared<const CandidateStylePresentation>(
                               CandidateStylePresentation{hidden_skin,
                                                          hidden_skin})});
  window.ActivateStyle(hidden_style);
  CHECK(WaitForCounters(window, [&](CandidateWindow::Counters counters) {
    return counters.full > full_before;
  }));
  probe = Probe();
  const LONG hidden_height = probe.rect.bottom - probe.rect.top;
  window.Stop();
  CHECK(shown_height > hidden_height);
  return true;
}

bool HealthyWindowAndHideRules() {
  CandidateWindow window;
  CHECK(window.Start());
  const HWND foreground = GetForegroundWindow();
  window.Publish(VisibleSnapshot());
  WindowProbe probe;
  CHECK(WaitForVisibility(true, &probe));
  CHECK((probe.ex_style & WS_EX_NOACTIVATE) != 0);
  CHECK((probe.ex_style & WS_EX_TOOLWINDOW) != 0);
  CHECK((probe.ex_style & WS_EX_LAYERED) != 0);
  CHECK((probe.ex_style & WS_EX_TOPMOST) != 0);
  CHECK(probe.cursor ==
        LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
  CHECK(GetForegroundWindow() == foreground);

  struct Placement {
    UiRect caret;
    UiRect work_area;
    uint32_t dpi;
  };
  constexpr Placement placements[] = {
      {{-12, 1020, -10, 1040}, {-1920, 0, 0, 1080}, 96},
      {{1910, 1050, 1912, 1070}, {0, 0, 1920, 1080}, 192},
      {{4400, 1180, 4402, 1200}, {1920, -200, 4480, 1240}, 144}};
  uint64_t placement_sequence = 4;
  for (const Placement &placement : placements) {
    auto moved = VisibleSnapshot(placement_sequence++);
    moved->ui_state.caret = placement.caret;
    moved->ui_state.work_area = placement.work_area;
    moved->ui_state.dpi = placement.dpi;
    window.Publish(moved);
    for (int attempt = 0; attempt < 100; ++attempt) {
      probe = Probe();
      if (probe.window && probe.visible &&
          probe.rect.left >= placement.work_area.left - 64 &&
          probe.rect.top >= placement.work_area.top - 64 &&
          probe.rect.right <= placement.work_area.right + 64 &&
          probe.rect.bottom <= placement.work_area.bottom + 64) {
        break;
      }
      Sleep(10);
    }
    CHECK(probe.window && probe.visible);
    if (probe.rect.left < placement.work_area.left - 64 ||
        probe.rect.top < placement.work_area.top - 64 ||
        probe.rect.right > placement.work_area.right + 64 ||
        probe.rect.bottom > placement.work_area.bottom + 64) {
      std::fprintf(stderr,
                   "placement outside work area: window=(%ld,%ld,%ld,%ld) "
                   "work=(%ld,%ld,%ld,%ld) dpi=%u\n",
                   probe.rect.left, probe.rect.top, probe.rect.right,
                   probe.rect.bottom, placement.work_area.left,
                   placement.work_area.top, placement.work_area.right,
                   placement.work_area.bottom, placement.dpi);
    }
    CHECK(probe.rect.left >= placement.work_area.left - 64);
    CHECK(probe.rect.top >= placement.work_area.top - 64);
    CHECK(probe.rect.right <= placement.work_area.right + 64);
    CHECK(probe.rect.bottom <= placement.work_area.bottom + 64);
    CHECK(GetForegroundWindow() == foreground);
  }

  auto newest = VisibleSnapshot(placement_sequence++);
  newest->revision = 100;
  newest->ui_state.caret = {-1500, 300, -1498, 320};
  window.Publish(newest);
  CHECK(WaitForVisibility(true, &probe));
  for (int attempt = 0; attempt < 100 && probe.rect.left > -1000; ++attempt) {
    Sleep(1);
    probe = Probe();
  }
  CHECK(probe.rect.left < -1000);
  auto stale = VisibleSnapshot(placement_sequence++);
  stale->revision = 99;
  stale->ui_state.caret = {1500, 300, 1502, 320};
  window.Publish(stale);
  Sleep(25);
  probe = Probe();
  CHECK(probe.rect.left < -1000);

  FamoSkin wide_skin = FamoSkinDefault();
  wide_skin.min_width = 600;
  auto independent_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{0, std::make_shared<const CandidateStylePresentation>(
                               CandidateStylePresentation{wide_skin,
                                                          wide_skin})});
  window.ActivateStyle(independent_style);
  auto snapshot_style = std::make_shared<const RuntimeStyleState>(
      RuntimeStyleState{
          0, std::make_shared<const CandidateStylePresentation>(
                 CandidateStylePresentation{FamoSkinDefault(),
                                            FamoSkinDefault()})});
  auto styled = VisibleSnapshot(placement_sequence++);
  styled->revision = 200;
  styled->style = snapshot_style;
  window.Publish(styled);
  for (int attempt = 0; attempt < 100; ++attempt) {
    probe = Probe();
    if (probe.window && probe.visible && probe.rect.right - probe.rect.left < 800)
      break;
    Sleep(1);
  }
  CHECK(probe.rect.right - probe.rect.left < 800);

  auto denied = VisibleSnapshot(placement_sequence++);
  denied->ui_state.show_allowed = false;
  window.Publish(denied);
  CHECK(WaitForVisibility(false));

  auto unavailable = VisibleSnapshot(placement_sequence++);
  unavailable->ui_state.layout_available = false;
  window.Publish(unavailable);
  CHECK(WaitForVisibility(false));

  auto unfocused = VisibleSnapshot(placement_sequence++);
  unfocused->ui_state.focused = false;
  window.Publish(unfocused);
  CHECK(WaitForVisibility(false));

  auto unapplied = VisibleSnapshot(placement_sequence++);
  unapplied->ui_sequence = unapplied->composition_sequence;
  window.Publish(unapplied);
  CHECK(WaitForVisibility(false));
  window.Stop();
  return true;
}

bool FirstVisibleBudgetAfterPrewarm() {
  std::vector<double> prewarm_ms;
  std::vector<double> samples_ms;
  for (uint64_t sample = 0; sample < 100; ++sample) {
    CandidateWindow window;
    CHECK(window.Start());
    const auto prewarm_started = std::chrono::steady_clock::now();
    CHECK(window.Prewarm());
    auto hidden = VisibleSnapshot(sample * 2 + 10);
    hidden->revision = sample * 2 + 1;
    hidden->ui_state.show_allowed = false;
    window.Publish(hidden);
    prewarm_ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - prewarm_started)
                             .count());
    const auto published = std::chrono::steady_clock::now();
    auto visible = VisibleSnapshot(sample * 2 + 11);
    visible->revision = hidden->revision + 1;
    window.Publish(visible);
    CHECK(WaitForVisibility(true));
    samples_ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - published)
                             .count());
    window.Stop();
  }
  std::sort(prewarm_ms.begin(), prewarm_ms.end());
  std::sort(samples_ms.begin(), samples_ms.end());
  const double p99 = samples_ms[(samples_ms.size() * 99 - 1) / 100];
  const double prewarm_p99 =
      prewarm_ms[(prewarm_ms.size() * 99 - 1) / 100];
  std::printf("candidate_first_visible: samples=%zu target_dpi=192 "
              "resource_prewarm_p99_ms=%.3f "
              "visible_p99_ms=%.3f visible_max_ms=%.3f\n",
              samples_ms.size(), prewarm_p99, p99, samples_ms.back());
  CHECK(p99 <= 50.0);
  return true;
}

bool FastPathsAndDeviceRecoveryAreObservable() {
  CandidateWindow window;
  CHECK(window.Start());
  auto initial = VisibleSnapshot(30);
  initial->revision = 1;
  window.Publish(initial);
  WindowProbe initial_probe;
  CHECK(WaitForVisibility(true, &initial_probe));
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.full >= 1;
  }));

  auto duplicate = VisibleSnapshot(31);
  duplicate->revision = 2;
  window.Publish(duplicate);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.duplicate >= 1;
  }));

  auto moved = VisibleSnapshot(32);
  moved->revision = 3;
  moved->ui_state.caret = {700, 450, 702, 470};
  window.Publish(moved);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.anchor_only >= 1;
  }));
  WindowProbe moved_probe = Probe();
  CHECK(moved_probe.visible);
  CHECK(moved_probe.rect.left != initial_probe.rect.left ||
        moved_probe.rect.top != initial_probe.rect.top);

  auto selected = VisibleSnapshot(33);
  selected->revision = 4;
  selected->ui_state = moved->ui_state;
  selected->composition.highlighted_index = 1;
  window.Publish(selected);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.selection_only >= 1;
  }));
  CHECK(PostMessageW(moved_probe.window, WM_THEMECHANGED, 0, 0) != FALSE);
  CHECK(WaitForCounters(window, [](const CandidateWindow::Counters &value) {
    return value.full >= 2;
  }));
  const CandidateWindow::Counters counters = window.counters();
  std::printf("candidate_fast_paths: full=%llu duplicate=%llu anchor=%llu "
              "selection=%llu\n",
              static_cast<unsigned long long>(counters.full),
              static_cast<unsigned long long>(counters.duplicate),
              static_cast<unsigned long long>(counters.anchor_only),
              static_cast<unsigned long long>(counters.selection_only));
  window.Stop();

  CandidateWindow recovering(CandidateWindow::Fault::DeviceLossOnce);
  CHECK(recovering.Start());
  auto recover_snapshot = VisibleSnapshot(40);
  recover_snapshot->revision = 1;
  recovering.Publish(recover_snapshot);
  CHECK(WaitForVisibility(true));
  CHECK(WaitForCounters(
      recovering, [](const CandidateWindow::Counters &value) {
        return value.device_recovery == 1 && value.full >= 1;
      }));
  recovering.Stop();
  return true;
}

bool PaintFailureHidesWithoutBlockingEngine() {
  CandidateWindow window(CandidateWindow::Fault::PaintAfterVisible);
  CHECK(window.Start());
  window.Publish(VisibleSnapshot());
  CHECK(WaitForVisibility(true));
  auto changed = VisibleSnapshot(4);
  changed->composition.highlighted_index = 1;
  window.Publish(changed);
  CHECK(WaitForVisibility(false));

  RuntimeService service;
  service.SetSnapshotSink(&window);
  std::string error;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {21, 22, 23, 0, 0, 0};
  CHECK(service.Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {21, 22, 23, 24, 25, 1};
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::Ok);
  Frame key;
  key.command = Command::ProcessKey;
  key.correlation = {21, 22, 23, 24, 25, 2};
  CHECK(EncodeKeyEvent({'N', 0, 0, 1, 1}, &key.payload));
  const auto key_started = std::chrono::steady_clock::now();
  CHECK(service.Dispatch(key).status == Status::Ok);
  CHECK(std::chrono::steady_clock::now() - key_started <
        std::chrono::milliseconds(50));
  service.SetSnapshotSink(nullptr);
  service.Stop();
  window.Stop();
  return true;
}

bool FaultsNeverBlockPublisher() {
  constexpr CandidateWindow::Fault faults[] = {
      CandidateWindow::Fault::Create, CandidateWindow::Fault::Layout,
      CandidateWindow::Fault::Paint, CandidateWindow::Fault::Submit,
      CandidateWindow::Fault::Hang};
  for (CandidateWindow::Fault fault : faults) {
    CandidateWindow window(fault);
    CHECK(window.Start());
    const auto publish_started = std::chrono::steady_clock::now();
    window.Publish(VisibleSnapshot());
    CHECK(std::chrono::steady_clock::now() - publish_started <
          std::chrono::milliseconds(10));
    Sleep(25);
    const auto stop_started = std::chrono::steady_clock::now();
    window.Stop();
    CHECK(std::chrono::steady_clock::now() - stop_started <
          std::chrono::milliseconds(400));
  }
  return true;
}

bool HangingUiDoesNotDelayEngine() {
  CandidateWindow window(CandidateWindow::Fault::Hang);
  CHECK(window.Start());
  RuntimeService service;
  service.SetSnapshotSink(&window);
  std::string error;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {11, 12, 13, 0, 0, 0};
  CHECK(service.Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {11, 12, 13, 14, 15, 1};
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::Ok);
  Frame key;
  key.command = Command::ProcessKey;
  key.correlation = {11, 12, 13, 14, 15, 2};
  CHECK(EncodeKeyEvent({'N', 0, 0, 1, 1}, &key.payload));
  const auto started = std::chrono::steady_clock::now();
  CHECK(service.Dispatch(key).status == Status::Ok);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(50));
  service.Stop();
  service.SetSnapshotSink(nullptr);
  window.Stop();
  return true;
}

} // namespace

int main() {
  if (!PreviewRowsMapToAbsoluteCandidateIndexes() ||
      !ScrollTransitionIsBoundedAndOptional() ||
      !PrewarmCompletesBeforeReturn() ||
      !HiddenHighDpiStateDoesNotDelayFirstVisible() ||
      !InlineHostPreeditStillShowsPanelHeader() ||
      !HealthyWindowAndHideRules() ||
      !FirstVisibleBudgetAfterPrewarm() ||
      !FastPathsAndDeviceRecoveryAreObservable() ||
      !PaintFailureHidesWithoutBlockingEngine() ||
      !FaultsNeverBlockPublisher() || !HangingUiDoesNotDelayEngine())
    return 1;
  std::printf("candidate_window_selfcheck: OK\n");
  return 0;
}
