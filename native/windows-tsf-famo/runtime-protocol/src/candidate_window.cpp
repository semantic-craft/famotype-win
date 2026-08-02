#include "candidate_window.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include <objbase.h>
#include <windows.h>

#include <UIAutomation.h>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "candidate_automation.h"
#include "candidate_skin.h"
#include "dib_surface.h"
#include "win_handle.h"

namespace famo::runtime {
namespace {

constexpr uint32_t kModeIndicatorDurationMs = 700;
constexpr int kModeIndicatorSize = 40;
constexpr int kModeIndicatorInset = 3;
constexpr wchar_t kCandidateWindowClassName[] = L"FamoRuntimeCandidateWindow";
constexpr wchar_t kModeIndicatorClassName[] = L"FamoRuntimeModeIndicator";

int ScaleDpi(int value, uint32_t dpi) {
  if (dpi == 0)
    dpi = 96;
  return static_cast<int>((static_cast<int64_t>(value) * dpi + 48) / 96);
}

FamoUtf8String ViewString(std::string_view value) {
  return {static_cast<uint32_t>(sizeof(FamoUtf8String)), value.data(),
          static_cast<uint32_t>(value.size())};
}

uint32_t SystemColor(int index) {
  const COLORREF color = GetSysColor(index);
  return 0xff000000u | (static_cast<uint32_t>(GetRValue(color)) << 16) |
         (static_cast<uint32_t>(GetGValue(color)) << 8) |
         static_cast<uint32_t>(GetBValue(color));
}

bool ApplySystemHighContrast(FamoSkin *skin) {
  HIGHCONTRASTW state{sizeof(state)};
  if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(state), &state, 0) ||
      (state.dwFlags & HCF_HIGHCONTRASTON) == 0)
    return false;
  ApplyHighContrastPalette(skin, SystemColor(COLOR_WINDOW),
                           SystemColor(COLOR_WINDOWTEXT),
                           SystemColor(COLOR_HIGHLIGHT),
                           SystemColor(COLOR_HIGHLIGHTTEXT));
  return true;
}

bool SameCompositionExceptSelection(const Composition &left,
                                    const Composition &right) {
  return left.handled == right.handled && left.preedit == right.preedit &&
         left.commit == right.commit &&
         left.commit_preview == right.commit_preview &&
         left.schema_id == right.schema_id &&
         left.schema_name == right.schema_name &&
         left.candidates == right.candidates &&
         left.preview_candidates == right.preview_candidates &&
         left.page_index == right.page_index &&
         left.page_size == right.page_size &&
         left.state_flags == right.state_flags &&
         left.preedit_sel_start == right.preedit_sel_start &&
         left.preedit_sel_end == right.preedit_sel_end &&
         left.preedit_cursor_pos == right.preedit_cursor_pos &&
         left.status_flags == right.status_flags &&
         left.is_last_page == right.is_last_page;
}

bool SameUiExceptPlacement(const UiState &left, const UiState &right) {
  return left.dpi == right.dpi &&
         left.layout_available == right.layout_available &&
         left.focused == right.focused &&
         left.show_allowed == right.show_allowed &&
         left.selection_capability == right.selection_capability;
}

bool SurfaceFitsWorkArea(const FamoLayoutResult &layout,
                         const UiRect &work_area) {
  const int64_t work_width =
      static_cast<int64_t>(work_area.right) - work_area.left;
  const int64_t work_height =
      static_cast<int64_t>(work_area.bottom) - work_area.top;
  const int64_t margin = (std::max)(layout.shadow_margin, 0);
  const int64_t surface_width =
      static_cast<int64_t>(layout.content_size.cx) + 2 * margin;
  const int64_t surface_height =
      static_cast<int64_t>(layout.content_size.cy) + 2 * margin;
  return work_width > 0 && work_height > 0 && surface_width > 0 &&
         surface_height > 0 && surface_width <= work_width &&
         surface_height <= work_height;
}

bool SameWorkAreaExtent(const UiRect &left, const UiRect &right) {
  return static_cast<int64_t>(left.right) - left.left ==
             static_cast<int64_t>(right.right) - right.left &&
         static_cast<int64_t>(left.bottom) - left.top ==
             static_cast<int64_t>(right.bottom) - right.top;
}

class ViewAdapter {
public:
  explicit ViewAdapter(const Composition &source) {
    candidates_.reserve(source.candidates.size());
    for (const Candidate &candidate : source.candidates) {
      FamoCandidate adapted{};
      adapted.size = static_cast<uint32_t>(sizeof(adapted));
      adapted.text = ViewString(candidate.text);
      adapted.comment = ViewString(candidate.comment);
      adapted.label = ViewString(candidate.label);
      adapted.quality = candidate.quality;
      adapted.flags = candidate.flags;
      candidates_.push_back(adapted);
    }
    preview_candidates_.reserve(source.preview_candidates.size());
    for (const Candidate &candidate : source.preview_candidates) {
      FamoCandidate adapted{};
      adapted.size = static_cast<uint32_t>(sizeof(adapted));
      adapted.text = ViewString(candidate.text);
      adapted.comment = ViewString(candidate.comment);
      adapted.label = ViewString(candidate.label);
      adapted.quality = candidate.quality;
      adapted.flags = candidate.flags;
      preview_candidates_.push_back(adapted);
    }
    view_.size = static_cast<uint32_t>(sizeof(view_));
    // The panel receives raw preedit/caret in both host-inline modes. Its own
    // show_preedit preference alone decides whether the editable header appears.
    view_.preedit = ViewString(source.preedit);
    view_.commit = ViewString(source.commit);
    view_.commit_preview = ViewString(source.commit_preview);
    view_.schema_id = ViewString(source.schema_id);
    view_.schema_name = ViewString(source.schema_name);
    view_.candidates = candidates_.data();
    view_.candidate_count = static_cast<uint32_t>(candidates_.size());
    view_.highlighted_index = source.highlighted_index;
    view_.page_index = source.page_index;
    view_.page_size = source.page_size;
    view_.state_flags = source.state_flags;
    view_.preedit_sel_start = source.preedit_sel_start;
    view_.preedit_sel_end = source.preedit_sel_end;
    view_.preedit_cursor_pos = source.preedit_cursor_pos;
    view_.status_flags = source.status_flags;
    view_.is_last_page = source.is_last_page;
  }

  const FamoCompositionView *get() const { return &view_; }
  const FamoCandidate *preview_candidates() const {
    return preview_candidates_.data();
  }
  uint32_t preview_candidate_count() const {
    return static_cast<uint32_t>(preview_candidates_.size());
  }

private:
  std::vector<FamoCandidate> candidates_;
  std::vector<FamoCandidate> preview_candidates_;
  FamoCompositionView view_{};
};

struct WindowNotifications {
  HANDLE update_event = nullptr;
  std::atomic<uint64_t> *system_generation = nullptr;
  CandidateAutomation *automation = nullptr;
  FamoLayoutResult layout{};
  PreviewSelectionRequest selection_request{};
  PipeClientIdentity selection_owner{};
  uint32_t page_index = 0;
  uint32_t page_size = 0;
  int shadow_margin = 0;
  HWND foreground_window = nullptr;
};

LRESULT WindowProcImpl(HWND window, UINT message, WPARAM wparam,
                       LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (notifications) {
      notifications->system_generation->fetch_add(1,
                                                  std::memory_order_relaxed);
      SetEvent(notifications->update_event);
    }
  }
  if (message == WM_GETOBJECT) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (notifications && notifications->automation &&
        static_cast<LONG>(lparam) == UiaRootObjectId) {
      return notifications->automation->HandleGetObject(wparam, lparam);
    }
  }
  if (message == kCandidateAutomationSelectMessage) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    const uint32_t generation = static_cast<uint32_t>(wparam);
    const uint32_t index = static_cast<uint32_t>(lparam);
    if (notifications && notifications->automation &&
        notifications->automation->IsCurrentItem(generation, index) &&
        notifications->page_size != 0) {
      const uint64_t absolute =
          static_cast<uint64_t>(notifications->page_index) *
              notifications->page_size +
          index;
      if (absolute <= UINT32_MAX) {
        PreviewSelectionRequest request = notifications->selection_request;
        request.absolute_index = static_cast<uint32_t>(absolute);
        SendPreviewSelectionToOwner(window, request,
                                    notifications->selection_owner);
      }
    }
    return 0;
  }
  if (message == WM_LBUTTONUP) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    PreviewSelection selection;
    const int x = static_cast<int16_t>(LOWORD(lparam));
    const int y = static_cast<int16_t>(HIWORD(lparam));
    if (notifications &&
        PreviewSelectionAt(notifications->layout,
                           x - notifications->shadow_margin,
                           y - notifications->shadow_margin,
                           notifications->page_index,
                           notifications->page_size, &selection)) {
      PreviewSelectionRequest request = notifications->selection_request;
      request.absolute_index = selection.absolute_index;
      SendPreviewSelectionToOwner(window, request,
                                  notifications->selection_owner);
      return 0;
    }
  }
  if (message == WM_MOUSEACTIVATE)
    return MA_NOACTIVATE;
  if (message == WM_DESTROY) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (notifications && notifications->automation)
      notifications->automation->WindowDestroyed();
  }
  if (message == WM_NCHITTEST && GetWindowLongPtrW(window, GWLP_USERDATA) == 0)
    return HTTRANSPARENT;
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) noexcept {
  try {
    return WindowProcImpl(window, message, wparam, lparam);
  } catch (...) {
    return message == WM_NCCREATE
               ? FALSE
               : DefWindowProcW(window, message, wparam, lparam);
  }
}

HWND CreateLayeredWindow(const wchar_t *class_name,
                         WindowNotifications *notifications,
                         bool click_through = false) {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  // A NULL class cursor makes Windows keep whatever shape the pointer already
  // had when it enters the window, so a transient busy cursor sticks over the
  // panel until the pointer leaves it.
  window_class.hCursor =
      LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  window_class.lpszClassName = class_name;
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return nullptr;
  }
  DWORD ex_style =
      WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  if (click_through)
    ex_style |= WS_EX_TRANSPARENT;
  return CreateWindowExW(ex_style, class_name, L"", WS_POPUP, 0, 0, 1, 1,
                         nullptr, nullptr, GetModuleHandleW(nullptr),
                         notifications);
}

struct ObservedWindow {
  bool visible = false;
  bool has_bounds = false;
  RECT bounds{};
};

ObservedWindow ObserveWindow(HWND window) {
  ObservedWindow observed;
  observed.visible = IsWindowVisible(window) != FALSE;
  observed.has_bounds = GetWindowRect(window, &observed.bounds) != FALSE;
  return observed;
}

void NotifyCandidateShownOrChanged(HWND window,
                                   const ObservedWindow &before) {
  // Observe the HWND after an already-decided presentation. ShouldShow remains
  // the only visibility policy; this does not create a second state machine.
  const ObservedWindow after = ObserveWindow(window);
  if (!after.visible)
    return;
  DWORD event = 0;
  if (!before.visible) {
    event = EVENT_OBJECT_IME_SHOW;
  } else if (!before.has_bounds || !after.has_bounds ||
             !EqualRect(&before.bounds, &after.bounds)) {
    event = EVENT_OBJECT_IME_CHANGE;
  }
  if (event != 0)
    NotifyWinEvent(event, window, OBJID_CLIENT, CHILDID_SELF);
}

bool MoveVisible(HWND window, int x, int y) {
  const ObservedWindow before = ObserveWindow(window);
  if (!SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0,
                    SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
    return false;
  }
  NotifyCandidateShownOrChanged(window, before);
  return true;
}

bool SubmitCandidateLayered(HWND window, const DibSurface &surface, int x,
                            int y) {
  const ObservedWindow before = ObserveWindow(window);
  if (!SubmitLayered(window, surface, x, y))
    return false;
  NotifyCandidateShownOrChanged(window, before);
  return true;
}

void HideCandidateWindow(HWND window) {
  const bool was_visible = IsWindowVisible(window) != FALSE;
  ShowWindow(window, SW_HIDE);
  if (was_visible && IsWindowVisible(window) == FALSE) {
    auto *notifications = reinterpret_cast<WindowNotifications *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (notifications && notifications->automation)
      notifications->automation->Hide();
    NotifyWinEvent(EVENT_OBJECT_IME_HIDE, window, OBJID_CLIENT, CHILDID_SELF);
  }
}

bool ShouldShow(const RuntimeSnapshot &snapshot) {
  const UiState &ui = snapshot.ui_state;
  return ui.focused && ui.layout_available && ui.show_allowed &&
         snapshot.ui_sequence > snapshot.composition_sequence &&
         !snapshot.composition.candidates.empty();
}

bool ShowModeIndicator(HWND window, DibSurface *surface,
                       FamoTextResources *resources, const FamoSkin *skin,
                       const RuntimeSnapshot &snapshot) {
  if (!window || !surface || !resources || !skin)
    return false;
  const uint32_t dpi = snapshot.ui_state.dpi;
  const int size = ScaleDpi(kModeIndicatorSize, dpi);
  const int inset = ScaleDpi(kModeIndicatorInset, dpi);
  if (!surface->Ensure(size, size))
    return false;
  surface->Clear();
  FamoStatusBarButton button{};
  button.bounds = {inset, inset, size - inset, size - inset};
  button.label = (snapshot.composition.status_flags & FAMO_STATUS_ASCII_MODE)
                     ? "英"
                     : "中";
  FamoStatusBarSpec spec{};
  spec.size = static_cast<uint32_t>(sizeof(spec));
  spec.bar_size = {size, size};
  spec.dpi = dpi;
  spec.button_count = 1;
  spec.buttons = &button;
  if (FamoStatusBarPaint(&spec, skin, resources, surface->dc()) != FAMO_UI_OK)
    return false;

  const UiRect &caret = snapshot.ui_state.caret;
  const UiRect &work = snapshot.ui_state.work_area;
  const FamoRect caret_rect{caret.left, caret.top, caret.right, caret.bottom};
  const FamoRect work_rect{work.left, work.top, work.right, work.bottom};
  int32_t x = 0;
  int32_t y = 0;
  uint32_t flipped = 0;
  FamoComputeAnchor(&caret_rect, &work_rect, {size, size}, &x, &y, &flipped);
  return SubmitLayered(window, *surface, x, y);
}

bool PrewarmRenderer(FamoTextResources *resources, const FamoSkin *skin,
                     uint32_t dpi, DibSurface *surface) {
  // Pay native factory/effect startup offscreen; the real size replaces this
  // single shadow entry on its first frame.
  Composition composition;
  composition.candidates = {Candidate{"x", "", "1", 0, 0}};
  composition.page_size = 8;
  composition.is_last_page = 1;
  ViewAdapter view(composition);
  FamoLayoutInput input{};
  input.size = static_cast<uint32_t>(sizeof(input));
  input.caret_rect = {400, 300, 402, 320};
  input.work_area = {0, 0, 1920, 1080};
  input.dpi = dpi;
  input.measure = &FamoTextMeasure;
  input.measure_user = resources;
  FamoLayoutResult layout{};
  if (FamoCandidateUiLayout(view.get(), skin, &input, &layout) != FAMO_UI_OK)
    return false;
  const int margin = std::max(0, layout.shadow_margin);
  if (!surface->Ensure(layout.content_size.cx + margin * 2,
                       layout.content_size.cy + margin * 2))
    return false;
  surface->Clear();
  return FamoCandidateUiPaint(view.get(), skin, &input, &layout, resources,
                              surface->dc()) == FAMO_UI_OK;
}

} // namespace

bool SendPreviewSelectionToOwner(
    HWND source_window, const PreviewSelectionRequest &request,
    const PipeClientIdentity &selection_owner) noexcept {
  if (!source_window || !IsWindow(source_window) ||
      request.correlation.client_id == 0 ||
      request.composition_sequence == 0 ||
      !request.selection_capability || !selection_owner) {
    return false;
  }
  win::UniqueHandle owner_process(
      AcquirePipeClientIdentityLease(selection_owner));
  if (!owner_process)
    return false;
  DWORD source_process_id = 0;
  if (GetWindowThreadProcessId(source_window, &source_process_id) == 0 ||
      source_process_id != GetCurrentProcessId()) {
    return false;
  }

  wchar_t title[32]{};
  if (swprintf_s(title, L"%llu",
                 static_cast<unsigned long long>(
                     request.correlation.client_id)) <= 0) {
    return false;
  }
  HWND target = nullptr;
  HWND after = nullptr;
  while ((after = FindWindowExW(HWND_MESSAGE, after,
                                kPreviewSelectionWindowClass, title))) {
    DWORD process_id = 0;
    if (GetWindowThreadProcessId(after, &process_id) != 0 &&
        process_id == selection_owner.process_id) {
      target = after;
      break;
    }
  }
  DWORD confirmed_process_id = 0;
  if (!target ||
      GetWindowThreadProcessId(target, &confirmed_process_id) == 0 ||
      confirmed_process_id != selection_owner.process_id ||
      WaitForSingleObject(owner_process.get(), 0) != WAIT_TIMEOUT) {
    return false;
  }

  COPYDATASTRUCT data{static_cast<ULONG_PTR>(kPreviewSelectionCopyDataId),
                      static_cast<DWORD>(sizeof(request)),
                      const_cast<PreviewSelectionRequest *>(&request)};
  DWORD_PTR handled = 0;
  return SendMessageTimeoutW(
             target, WM_COPYDATA, reinterpret_cast<WPARAM>(source_window),
             reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
             100, &handled) != 0 &&
         handled != 0;
}

bool PreviewSelectionAt(const FamoLayoutResult &layout, int x, int y,
                        uint32_t page_index, uint32_t page_size,
                        PreviewSelection *selection) noexcept {
  if (!selection || page_size == 0)
    return false;
  const uint32_t count = (std::min)(layout.preview_candidate_count,
                                    uint32_t{FAMO_MAX_PREVIEW_CANDIDATES});
  for (uint32_t i = 0; i < count; ++i) {
    const FamoRect &bounds = layout.preview_candidates[i].bounds;
    if (x < bounds.left || x >= bounds.right || y < bounds.top ||
        y >= bounds.bottom)
      continue;
    const uint64_t absolute =
        (static_cast<uint64_t>(page_index) + 1) * page_size + i;
    if (absolute > UINT32_MAX)
      return false;
    selection->absolute_index = static_cast<uint32_t>(absolute);
    return true;
  }
  return false;
}

bool PlanScrollTransition(const FamoLayoutResult &previous,
                          const FamoLayoutResult &next,
                          uint32_t previous_page, uint32_t next_page,
                          bool animations_enabled,
                          ScrollTransitionPlan *plan) noexcept {
  if (!plan)
    return false;
  *plan = {};
  int32_t direction = 0;
  if (next_page > previous_page && next_page - previous_page == 1)
    direction = 1;
  else if (previous_page > next_page && previous_page - next_page == 1)
    direction = -1;
  if (!animations_enabled || direction == 0 ||
      previous.candidate_count == 0 || next.candidate_count == 0 ||
      previous.preview_candidate_count == 0 ||
      next.preview_candidate_count == 0 ||
      previous.content_size.cx != next.content_size.cx ||
      previous.content_size.cy != next.content_size.cy ||
      previous.shadow_margin != next.shadow_margin)
    return false;

  const FamoRect &previous_main = previous.candidates[0].bounds;
  const FamoRect &next_main = next.candidates[0].bounds;
  const FamoRect &previous_preview = previous.preview_candidates[0].bounds;
  const FamoRect &next_preview = next.preview_candidates[0].bounds;
  const FamoRect &previous_last =
      previous.preview_candidates[previous.preview_candidate_count - 1].bounds;
  const FamoRect &next_last =
      next.preview_candidates[next.preview_candidate_count - 1].bounds;
  const int32_t row_step = next_preview.top - next_main.top;
  const int32_t clip_top = (std::min)(next_main.top, next.highlight.top);
  if (row_step <= 0 || previous_preview.top - previous_main.top != row_step ||
      previous_main.top != next_main.top ||
      previous_last.bottom != next_last.bottom ||
      clip_top != (std::min)(previous_main.top, previous.highlight.top) ||
      next_last.bottom <= clip_top + row_step)
    return false;

  plan->clip = {0, clip_top, next.content_size.cx, next_last.bottom};
  plan->row_step = row_step;
  plan->direction = direction;
  return true;
}

int32_t ScrollTransitionOffset(uint32_t elapsed_ms,
                               int32_t row_step) noexcept {
  if (row_step <= 0 || elapsed_ms == 0)
    return 0;
  if (elapsed_ms >= kCandidateScrollTransitionMs)
    return row_step;
  const double progress =
      static_cast<double>(elapsed_ms) / kCandidateScrollTransitionMs;
  const double remaining = 1.0 - progress;
  const double eased = 1.0 - remaining * remaining * remaining;
  return (std::min)(row_step,
                    static_cast<int32_t>(row_step * eased + 0.5));
}

namespace {

bool SystemAnimationsEnabled() {
  BOOL enabled = FALSE;
  return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) &&
         enabled != FALSE;
}

bool CopySurface(DibSurface *destination, const DibSurface &source) {
  return destination && destination->Ensure(source.width(), source.height()) &&
         BitBlt(destination->dc(), 0, 0, source.width(), source.height(),
                source.dc(), 0, 0, SRCCOPY) != FALSE;
}

bool ComposeScrollFrame(DibSurface *destination, const DibSurface &previous,
                        const DibSurface &next,
                        const ScrollTransitionPlan &plan, int margin,
                        int32_t offset) {
  if (!CopySurface(destination, next))
    return false;
  const int32_t left = plan.clip.left + margin;
  const int32_t top = plan.clip.top + margin;
  const int32_t right = plan.clip.right + margin;
  const int32_t bottom = plan.clip.bottom + margin;
  const int32_t width = right - left;
  const int32_t height = bottom - top;
  const int saved = SaveDC(destination->dc());
  if (saved == 0 || IntersectClipRect(destination->dc(), left, top, right,
                                      bottom) == ERROR) {
    if (saved != 0)
      RestoreDC(destination->dc(), saved);
    return false;
  }
  BOOL copied = FALSE;
  if (plan.direction > 0) {
    copied = BitBlt(destination->dc(), left, top - offset, width,
                    plan.row_step, previous.dc(), left, top, SRCCOPY) &&
             BitBlt(destination->dc(), left,
                    top + plan.row_step - offset, width, height, next.dc(),
                    left, top, SRCCOPY);
  } else {
    copied = BitBlt(destination->dc(), left,
                    top - plan.row_step + offset, width, height, next.dc(),
                    left, top, SRCCOPY) &&
             BitBlt(destination->dc(), left,
                    bottom - plan.row_step + offset, width, plan.row_step,
                    previous.dc(), left, bottom - plan.row_step, SRCCOPY);
  }
  RestoreDC(destination->dc(), saved);
  return copied != FALSE;
}

} // namespace

struct CandidateWindow::State {
  explicit State(Fault injected_fault) : fault(injected_fault) {
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    update_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    prewarm_complete_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  ~State() {
    if (stop_event)
      CloseHandle(stop_event);
    if (update_event)
      CloseHandle(update_event);
    if (prewarm_complete_event)
      CloseHandle(prewarm_complete_event);
  }

  HANDLE stop_event = nullptr;
  HANDLE update_event = nullptr;
  HANDLE prewarm_complete_event = nullptr;
  std::atomic<std::shared_ptr<const RuntimeSnapshot>> latest;
  std::atomic<std::shared_ptr<const void>> presentation;
  std::atomic<bool> prewarm_requested{false};
  std::atomic<bool> prewarm_succeeded{false};
  std::atomic<uint64_t> duplicate_count{0};
  std::atomic<uint64_t> anchor_only_count{0};
  std::atomic<uint64_t> selection_only_count{0};
  std::atomic<uint64_t> full_count{0};
  std::atomic<uint64_t> device_recovery_count{0};
  std::atomic<uint64_t> mode_indicator_count{0};
  bool device_loss_injected = false;
  bool submitted_visible = false;
  Fault fault;
};

CandidateWindow::~CandidateWindow() { Stop(); }

bool CandidateWindow::Start() {
  try {
    Stop();
    auto state = std::make_shared<State>(fault_);
    if (!state->stop_event || !state->update_event ||
        !state->prewarm_complete_event)
      return false;
    thread_ = std::thread(&CandidateWindow::ThreadMain, state);
    state_ = std::move(state);
    return true;
  } catch (...) {
    return false;
  }
}

bool CandidateWindow::Prewarm() {
  std::shared_ptr<State> state = state_;
  if (!state)
    return false;
  state->prewarm_succeeded.store(false, std::memory_order_release);
  state->prewarm_requested.store(true, std::memory_order_release);
  if (!SetEvent(state->update_event))
    return false;
  HANDLE events[] = {state->stop_event, state->prewarm_complete_event};
  return WaitForMultipleObjects(2, events, FALSE, 2000) == WAIT_OBJECT_0 + 1 &&
         state->prewarm_succeeded.load(std::memory_order_acquire);
}

bool CandidateWindow::PrepareStyle(
    std::string_view text, bool exists,
    std::shared_ptr<const void> *presentation) noexcept {
  try {
    if (!presentation)
      return false;
    CandidateStylePresentation style{FamoSkinDefault(), FamoSkinDefault()};
    if (exists &&
        (!ParseCandidateSkinForTheme(text, false, &style.light) ||
         !ParseCandidateSkinForTheme(text, true, &style.dark)))
      return false;
    *presentation =
        std::make_shared<const CandidateStylePresentation>(std::move(style));
    return true;
  } catch (...) {
    return false;
  }
}

void CandidateWindow::ActivateStyle(
    std::shared_ptr<const RuntimeStyleState> style) noexcept {
  try {
    std::shared_ptr<State> state = state_;
    if (!state || !style)
      return;
    state->presentation.store(style->presentation, std::memory_order_release);
    std::shared_ptr<const RuntimeSnapshot> current =
        state->latest.load(std::memory_order_acquire);
    while (current && current->style != style) {
      auto replacement = std::make_shared<RuntimeSnapshot>(*current);
      replacement->style = style;
      std::shared_ptr<const RuntimeSnapshot> desired = std::move(replacement);
      if (state->latest.compare_exchange_weak(current, desired,
                                              std::memory_order_release,
                                              std::memory_order_acquire))
        break;
    }
    SetEvent(state->update_event);
  } catch (...) {
    // A later engine/UI snapshot carries the same immutable style generation.
  }
}

void CandidateWindow::PrepareForRuntimeReady() noexcept { (void)Prewarm(); }

CandidateWindow::Counters CandidateWindow::counters() const noexcept {
  std::shared_ptr<State> state = state_;
  if (!state)
    return {};
  return {state->duplicate_count.load(std::memory_order_relaxed),
          state->anchor_only_count.load(std::memory_order_relaxed),
          state->selection_only_count.load(std::memory_order_relaxed),
          state->full_count.load(std::memory_order_relaxed),
          state->device_recovery_count.load(std::memory_order_relaxed),
          state->mode_indicator_count.load(std::memory_order_relaxed)};
}

void CandidateWindow::Stop() noexcept {
  try {
    std::shared_ptr<State> state = std::move(state_);
    if (!state)
      return;
    SetEvent(state->stop_event);
    if (thread_.joinable()) {
      if (WaitForSingleObject(thread_.native_handle(), 250) == WAIT_OBJECT_0)
        thread_.join();
      else
        thread_.detach();
    }
  } catch (...) {
    try {
      if (thread_.joinable())
        thread_.detach();
    } catch (...) {
    }
  }
}

void CandidateWindow::Publish(
    std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept {
  std::shared_ptr<State> state = state_;
  if (!state || !snapshot)
    return;
  try {
    auto targeted = std::make_shared<RuntimeSnapshot>(*snapshot);
    targeted->source_window = snapshot->ui_state.focused
                                  ? reinterpret_cast<uintptr_t>(
                                        GetForegroundWindow())
                                  : 0;
    snapshot = std::move(targeted);
  } catch (...) {
    // Rendering remains best effort, but an unbound frame is never clickable.
  }
  std::shared_ptr<const RuntimeSnapshot> current =
      state->latest.load(std::memory_order_acquire);
  for (;;) {
    if (current && current->revision != 0 && snapshot->revision != 0 &&
        snapshot->revision <= current->revision)
      return;
    if (state->latest.compare_exchange_weak(
            current, snapshot, std::memory_order_release,
            std::memory_order_acquire))
      break;
  }
  SetEvent(state->update_event);
}

void CandidateWindow::ThreadMain(std::shared_ptr<State> state) noexcept {
  try {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Candidate painting is brief and user-visible; keep it ahead of ordinary
  // background work without using a real-time priority class.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::atomic<uint64_t> system_generation{0};
  WindowNotifications notifications{state->update_event, &system_generation};
  HWND window =
      state->fault == Fault::Create
          ? nullptr
          : CreateLayeredWindow(kCandidateWindowClassName, &notifications);
  std::unique_ptr<CandidateAutomation> automation =
      CandidateAutomation::Create(window);
  notifications.automation = automation.get();
  // The mode indicator is a short-lived, click-through status toast rather
  // than a candidate/composition surface, so it does not publish IME
  // light-dismiss events.
  HWND mode_window =
      state->fault == Fault::Create
          ? nullptr
          : CreateLayeredWindow(kModeIndicatorClassName, nullptr, true);
  DibSurface surface;
  DibSurface previous_surface;
  DibSurface animation_surface;
  DibSurface mode_surface;
  struct ActiveScrollAnimation {
    bool active = false;
    uint64_t started_ms = 0;
    ScrollTransitionPlan plan{};
    FamoLayoutResult target_layout{};
    Composition target_composition;
    int margin = 0;
    HWND foreground_window = nullptr;
  } animation;
  struct ActiveModeIndicator {
    bool active = false;
    uint64_t expires_ms = 0;
    Correlation last_switch{};
    HWND foreground_window = nullptr;
  } mode_indicator;
  const FamoSkin fallback_skin = FamoSkinDefault();
  std::shared_ptr<const void> active_presentation;
  std::shared_ptr<const CandidateStylePresentation> active_style;
  FamoTextResources *resources = nullptr;
  uint32_t resource_dpi = 0;
  bool resource_skin_dirty = true;
  bool resource_high_contrast = false;
  uint64_t resource_system_generation = 0;
  bool resources_warmed = false;
  std::shared_ptr<const RuntimeSnapshot> presented_snapshot;
  std::shared_ptr<const void> presented_presentation;
  FamoLayoutResult presented_layout{};
  uint32_t presented_dpi = 0;
  bool presented_high_contrast = false;
  uint64_t presented_system_generation = 0;
  HANDLE events[] = {state->stop_event, state->update_event};
  for (;;) {
    DWORD timeout = INFINITE;
    if (animation.active) {
      const uint64_t elapsed = GetTickCount64() - animation.started_ms;
      timeout = elapsed >= kCandidateScrollTransitionMs
                    ? 0
                    : (std::min)(DWORD{16}, static_cast<DWORD>(
                                                   kCandidateScrollTransitionMs -
                                                   elapsed));
    }
    if (mode_indicator.active) {
      const uint64_t now = GetTickCount64();
      const DWORD remaining =
          mode_indicator.expires_ms <= now
              ? 0
              : static_cast<DWORD>((std::min)(
                    mode_indicator.expires_ms - now,
                    static_cast<uint64_t>(MAXDWORD)));
      timeout = timeout == INFINITE ? remaining : (std::min)(timeout, remaining);
    }
    const DWORD wait =
        MsgWaitForMultipleObjects(2, events, FALSE, timeout, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_TIMEOUT) {
      const uint64_t now = GetTickCount64();
      if (mode_indicator.active &&
          (now >= mode_indicator.expires_ms ||
           (mode_indicator.foreground_window &&
            GetForegroundWindow() != mode_indicator.foreground_window))) {
        ShowWindow(mode_window, SW_HIDE);
        mode_indicator.active = false;
      }
      if (!animation.active)
        continue;
      if (animation.foreground_window &&
          GetForegroundWindow() != animation.foreground_window) {
        HideCandidateWindow(window);
        animation.active = false;
        presented_snapshot.reset();
        continue;
      }
      const uint64_t elapsed = GetTickCount64() - animation.started_ms;
      const bool complete = elapsed >= kCandidateScrollTransitionMs;
      const DibSurface *frame = &surface;
      if (!complete) {
        const int32_t offset = ScrollTransitionOffset(
            static_cast<uint32_t>(elapsed), animation.plan.row_step);
        if (!ComposeScrollFrame(&animation_surface, previous_surface, surface,
                                animation.plan, animation.margin, offset)) {
          animation.active = false;
          continue;
        }
        frame = &animation_surface;
      }
      if (!SubmitCandidateLayered(
              window, *frame,
              animation.target_layout.origin_x - animation.margin,
              animation.target_layout.origin_y - animation.margin)) {
        HideCandidateWindow(window);
        animation.active = false;
        presented_snapshot.reset();
        continue;
      }
      if (complete) {
        animation.active = false;
        notifications.layout = animation.target_layout;
        if (automation)
          automation->Present(animation.target_composition,
                              animation.target_layout, animation.margin);
      }
      continue;
    }
    if (wait == WAIT_OBJECT_0 + 2) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      continue;
    }
    if (wait != WAIT_OBJECT_0 + 1)
      break;
    if (animation.active) {
      animation.active = false;
      notifications.layout = animation.target_layout;
      if (!SubmitCandidateLayered(
              window, surface,
              animation.target_layout.origin_x - animation.margin,
              animation.target_layout.origin_y - animation.margin)) {
        HideCandidateWindow(window);
        presented_snapshot.reset();
      } else if (automation) {
        automation->Present(animation.target_composition,
                            animation.target_layout, animation.margin);
      }
    }
    if (state->fault == Fault::Hang) {
      Sleep(5000);
      continue;
    }
    std::shared_ptr<const RuntimeSnapshot> snapshot =
        state->latest.load(std::memory_order_acquire);
    std::shared_ptr<const void> next_presentation;
    if (snapshot && snapshot->style)
      next_presentation = snapshot->style->presentation;
    else
      next_presentation = state->presentation.load(std::memory_order_acquire);
    if (next_presentation != active_presentation) {
      active_presentation = std::move(next_presentation);
      active_style =
          active_presentation
              ? std::static_pointer_cast<const CandidateStylePresentation>(
                    active_presentation)
              : nullptr;
      resource_skin_dirty = true;
      resources_warmed = false;
    }
    const bool system_dark = SystemUsesDarkPalette();
    const FamoSkin &configured_skin = active_style
                                          ? (system_dark ? active_style->dark
                                                         : active_style->light)
                                          : fallback_skin;
    FamoSkin resolved_skin = configured_skin;
    if (resolved_skin.layout_type == FAMO_LAYOUT_AUTO) {
      const bool vertical = snapshot &&
          (snapshot->composition.state_flags & kHostRimeVertical) != 0;
      resolved_skin.layout_type =
          vertical ? FAMO_LAYOUT_VERTICAL : FAMO_LAYOUT_HORIZONTAL;
      resolved_skin.min_width = vertical ? 76 : 210;
    }
    FamoSkin high_contrast_skin = resolved_skin;
    const FamoSkin *skin = &resolved_skin;
    const bool high_contrast = ApplySystemHighContrast(&high_contrast_skin);
    if (high_contrast)
      skin = &high_contrast_skin;
    const bool prewarm =
        state->prewarm_requested.exchange(false, std::memory_order_acq_rel);
    const bool should_show = snapshot && ShouldShow(*snapshot);
    const uint64_t current_system_generation =
        system_generation.load(std::memory_order_relaxed);
    if (resource_system_generation != current_system_generation) {
      resource_skin_dirty = true;
      resources_warmed = false;
      resource_system_generation = current_system_generation;
    }
    uint32_t dpi = snapshot ? snapshot->ui_state.dpi : 0;
    if (prewarm && dpi == 0)
      dpi = GetDpiForSystem();
    bool resources_reconfigured = false;
    if (dpi != 0 &&
        (!resources || resource_dpi != dpi || resource_skin_dirty ||
         resource_high_contrast != high_contrast)) {
      resources_warmed = false;
      if (resources &&
          FamoTextResourcesReconfigure(resources, skin, dpi) != FAMO_UI_OK) {
        FamoTextResourcesDestroy(resources);
        resources = nullptr;
      }
      if (!resources)
        resources = FamoTextResourcesCreate(skin, dpi);
      if (resources) {
        resource_dpi = dpi;
        resource_skin_dirty = false;
        resource_high_contrast = high_contrast;
        resources_reconfigured = true;
      } else {
        resource_dpi = 0;
      }
    }
    const bool run_prewarm = resources && !resources_warmed &&
                             (prewarm ||
                              (!should_show && resources_reconfigured));
    if (run_prewarm)
      resources_warmed =
          PrewarmRenderer(resources, skin, resource_dpi, &surface);
    if (prewarm) {
      state->prewarm_succeeded.store(resources_warmed,
                                     std::memory_order_release);
      SetEvent(state->prewarm_complete_event);
    }
    if (mode_indicator.active &&
        (!snapshot || !snapshot->ui_state.focused ||
         (mode_indicator.foreground_window &&
          GetForegroundWindow() != mode_indicator.foreground_window))) {
      ShowWindow(mode_window, SW_HIDE);
      mode_indicator.active = false;
    }
    if (mode_window && resources && snapshot &&
        snapshot->mode_switch_sequence != 0 &&
        snapshot->ui_sequence > snapshot->mode_switch_sequence &&
        snapshot->ui_state.focused && snapshot->ui_state.layout_available) {
      Correlation switch_id = snapshot->correlation;
      switch_id.sequence = snapshot->mode_switch_sequence;
      if (switch_id != mode_indicator.last_switch) {
        mode_indicator.last_switch = switch_id;
        HWND foreground = reinterpret_cast<HWND>(snapshot->source_window);
        if ((!foreground || GetForegroundWindow() == foreground) &&
            ShowModeIndicator(mode_window, &mode_surface, resources, skin,
                              *snapshot)) {
          mode_indicator.active = true;
          mode_indicator.expires_ms =
              GetTickCount64() + kModeIndicatorDurationMs;
          mode_indicator.foreground_window = foreground;
          state->mode_indicator_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (!window || !snapshot || !should_show) {
      if (window)
        HideCandidateWindow(window);
      presented_snapshot.reset();
      continue;
    }
    if (!resources) {
      HideCandidateWindow(window);
      presented_snapshot.reset();
      continue;
    }

    const bool stable_presentation =
        presented_snapshot && presented_presentation == active_presentation &&
        presented_dpi == dpi &&
        presented_high_contrast == high_contrast &&
        presented_system_generation == current_system_generation &&
        snapshot->source_window == presented_snapshot->source_window &&
        snapshot->selection_owner == presented_snapshot->selection_owner;
    if (stable_presentation &&
        snapshot->composition == presented_snapshot->composition &&
        snapshot->ui_state == presented_snapshot->ui_state) {
      state->duplicate_count.fetch_add(1, std::memory_order_relaxed);
      presented_snapshot = snapshot;
      continue;
    }
    if (stable_presentation &&
        snapshot->composition == presented_snapshot->composition &&
        SameUiExceptPlacement(snapshot->ui_state,
                              presented_snapshot->ui_state) &&
        SameWorkAreaExtent(snapshot->ui_state.work_area,
                           presented_snapshot->ui_state.work_area) &&
        SurfaceFitsWorkArea(presented_layout, snapshot->ui_state.work_area)) {
      const UiRect &caret = snapshot->ui_state.caret;
      const UiRect &work = snapshot->ui_state.work_area;
      const FamoRect caret_rect{caret.left, caret.top, caret.right,
                                caret.bottom};
      const FamoRect work_rect{work.left, work.top, work.right, work.bottom};
      int32_t origin_x = 0;
      int32_t origin_y = 0;
      uint32_t flipped = 0;
      FamoComputeWindowAnchor(
          &caret_rect, &work_rect, presented_layout.content_size,
          presented_layout.shadow_margin, &origin_x, &origin_y, &flipped);
      if (MoveVisible(window, origin_x - presented_layout.shadow_margin,
                      origin_y - presented_layout.shadow_margin)) {
        presented_layout.origin_x = origin_x;
        presented_layout.origin_y = origin_y;
        presented_layout.flipped = flipped;
        if (automation)
          automation->Move(presented_layout,
                           presented_layout.shadow_margin);
        presented_snapshot = snapshot;
        state->anchor_only_count.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      HideCandidateWindow(window);
      presented_snapshot.reset();
      continue;
    }

    ViewAdapter view(snapshot->composition);
    FamoLayoutInput input{};
    input.size = static_cast<uint32_t>(sizeof(input));
    const UiRect &caret = snapshot->ui_state.caret;
    const UiRect &work = snapshot->ui_state.work_area;
    input.caret_rect = {caret.left, caret.top, caret.right, caret.bottom};
    input.work_area = {work.left, work.top, work.right, work.bottom};
    input.dpi = dpi;
    input.measure = &FamoTextMeasure;
    input.measure_user = resources;
    input.preview_candidates = view.preview_candidates();
    input.preview_candidate_count = view.preview_candidate_count();
    input.preview_page_size = snapshot->composition.page_size;
    FamoLayoutResult layout{};
    const bool selection_only =
        stable_presentation &&
        snapshot->ui_state == presented_snapshot->ui_state &&
        snapshot->composition.highlighted_index !=
            presented_snapshot->composition.highlighted_index &&
        SameCompositionExceptSelection(snapshot->composition,
                                       presented_snapshot->composition);
    if (selection_only) {
      layout = presented_layout;
      layout.highlight = {};
      const uint32_t selected = snapshot->composition.highlighted_index;
      const uint32_t previous_selected =
          presented_snapshot->composition.highlighted_index;
      if (selected < layout.candidate_count &&
          previous_selected < layout.candidate_count) {
        const FamoRect previous_bounds =
            layout.candidates[previous_selected].bounds;
        const FamoRect selected_bounds = layout.candidates[selected].bounds;
        layout.highlight = {
            selected_bounds.left + presented_layout.highlight.left -
                previous_bounds.left,
            selected_bounds.top + presented_layout.highlight.top -
                previous_bounds.top,
            selected_bounds.right + presented_layout.highlight.right -
                previous_bounds.right,
            selected_bounds.bottom + presented_layout.highlight.bottom -
                previous_bounds.bottom};
      } else if (selected < layout.candidate_count) {
        layout.highlight = layout.candidates[selected].bounds;
      }
      state->selection_only_count.fetch_add(1, std::memory_order_relaxed);
    } else {
      state->full_count.fetch_add(1, std::memory_order_relaxed);
      if (state->fault == Fault::Layout ||
          FamoCandidateUiLayout(view.get(), skin, &input, &layout) !=
              FAMO_UI_OK) {
        HideCandidateWindow(window);
        presented_snapshot.reset();
        continue;
      }
    }
    ScrollTransitionPlan transition{};
    bool animate_scroll =
        !selection_only && stable_presentation &&
        snapshot->ui_state == presented_snapshot->ui_state &&
        PlanScrollTransition(presented_layout, layout,
                             presented_snapshot->composition.page_index,
                             snapshot->composition.page_index,
                             SystemAnimationsEnabled(), &transition);
    const int margin = std::max(0, layout.shadow_margin);
    const int width = layout.content_size.cx + margin * 2;
    const int height = layout.content_size.cy + margin * 2;
    if (animate_scroll &&
        (surface.width() != width || surface.height() != height ||
         !CopySurface(&previous_surface, surface)))
      animate_scroll = false;
    if (!surface.Ensure(width, height)) {
      HideCandidateWindow(window);
      continue;
    }
    const auto paint = [&]() {
      surface.Clear();
      if (state->fault == Fault::Paint ||
          (state->fault == Fault::PaintAfterVisible &&
           state->submitted_visible))
        return static_cast<int32_t>(FAMO_UI_E_PAINT_FAILED);
      if (state->fault == Fault::DeviceLossOnce &&
          !state->device_loss_injected) {
        state->device_loss_injected = true;
        return static_cast<int32_t>(FAMO_UI_E_PAINT_FAILED);
      }
      return FamoCandidateUiPaint(view.get(), skin, &input, &layout, resources,
                                  surface.dc());
    };
    int32_t paint_result = paint();
    if (paint_result != FAMO_UI_OK) {
      state->device_recovery_count.fetch_add(1, std::memory_order_relaxed);
      FamoTextResourcesDiscardDeviceResources(resources);
      paint_result = paint();
    }
    if (paint_result != FAMO_UI_OK) {
      HideCandidateWindow(window);
      presented_snapshot.reset();
      continue;
    }
    resources_warmed = true;
    if (state->latest.load(std::memory_order_acquire) != snapshot) {
      continue;
    }
    HWND frame_foreground =
        reinterpret_cast<HWND>(snapshot->source_window);
    if (frame_foreground && GetForegroundWindow() != frame_foreground) {
      HideCandidateWindow(window);
      presented_snapshot.reset();
      continue;
    }
    const DibSurface *first_frame = &surface;
    if (animate_scroll) {
      if (ComposeScrollFrame(&animation_surface, previous_surface, surface,
                             transition, margin, 0))
        first_frame = &animation_surface;
      else
        animate_scroll = false;
    }
    if (state->fault == Fault::Submit ||
        !SubmitCandidateLayered(window, *first_frame,
                                layout.origin_x - margin,
                                layout.origin_y - margin)) {
      HideCandidateWindow(window);
      presented_snapshot.reset();
    } else if (frame_foreground &&
               GetForegroundWindow() != frame_foreground) {
      HideCandidateWindow(window);
      notifications.foreground_window = nullptr;
      presented_snapshot.reset();
    } else {
      state->submitted_visible = true;
      notifications.layout = layout;
      if (animate_scroll)
        notifications.layout.preview_candidate_count = 0;
      notifications.selection_request =
          {snapshot->correlation, snapshot->composition_sequence, 0, 0,
           snapshot->ui_state.selection_capability};
      notifications.selection_owner = snapshot->selection_owner;
      notifications.page_index = snapshot->composition.page_index;
      notifications.page_size = snapshot->composition.page_size;
      notifications.shadow_margin = margin;
      notifications.foreground_window = frame_foreground;
      if (automation && !animate_scroll)
        automation->Present(snapshot->composition, layout, margin);
      presented_snapshot = snapshot;
      presented_presentation = active_presentation;
      presented_layout = layout;
      presented_dpi = dpi;
      presented_high_contrast = high_contrast;
      presented_system_generation = current_system_generation;
      if (animate_scroll) {
        animation.active = true;
        animation.started_ms = GetTickCount64();
        animation.plan = transition;
        animation.target_layout = layout;
        animation.target_composition = snapshot->composition;
        animation.margin = margin;
        animation.foreground_window = frame_foreground;
      }
    }
  }
  if (mode_window)
    DestroyWindow(mode_window);
  if (window) {
    HideCandidateWindow(window);
    DestroyWindow(window);
  }
  FamoTextResourcesDestroy(resources);
  if (SUCCEEDED(com))
    CoUninitialize();
  } catch (...) {
    state->prewarm_succeeded.store(false, std::memory_order_release);
    state->prewarm_requested.store(false, std::memory_order_release);
    SetEvent(state->prewarm_complete_event);
  }
}

} // namespace famo::runtime
