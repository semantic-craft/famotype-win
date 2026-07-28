#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "dib_surface.h"
#include "famo_status_ui.h"

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #value, __FILE__,     \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace {

bool DoubleAltRequiresTwoShortCleanTaps() {
  AltDoubleTapDetector detector;
  CHECK(!detector.Process(true, true, 100));
  CHECK(!detector.Process(true, false, 200));
  CHECK(!detector.Process(true, true, 400));
  CHECK(detector.Process(true, false, 450));

  CHECK(!detector.Process(true, true, 1000));
  CHECK(!detector.Process(true, false, 1600));
  CHECK(!detector.Process(true, true, 1700));
  CHECK(!detector.Process(true, false, 1750));

  CHECK(!detector.Process(true, true, 2300));
  CHECK(!detector.Process(true, false, 2350));
  CHECK(!detector.Process(false, true, 2400));
  CHECK(!detector.Process(true, true, 2450));
  CHECK(!detector.Process(true, false, 2500));
  return true;
}

bool GlobalHotKeysAcceptOnlyRestrictedCanonicalBindings() {
  GlobalHotKeyBinding binding;
  CHECK(ParseGlobalHotKeyBinding("Ctrl+Alt+J", &binding));
  CHECK((binding.modifiers & MOD_CONTROL) != 0);
  CHECK((binding.modifiers & MOD_ALT) != 0);
  CHECK(binding.virtual_key == 'J');
  CHECK(GlobalHotKeyBindingMatches(binding, 'J', true, true, false, false));
  CHECK(!GlobalHotKeyBindingMatches(binding, 'J', true, true, true, false));
  CHECK(!GlobalHotKeyBindingMatches(binding, 'J', true, true, false, true));
  CHECK(!ParseGlobalHotKeyBinding("Ctrl+J", &binding));
  CHECK(!ParseGlobalHotKeyBinding("Win+Alt+J", &binding));
  CHECK(!ParseGlobalHotKeyBinding("Ctrl+Ctrl+J", &binding));
  CHECK(!ParseGlobalHotKeyBinding("Ctrl+Alt+1", &binding));
  return true;
}

bool ToolboxPolicyMatchesGestureAndRecordedHotKeySemantics() {
  const std::string enabled =
      R"({"ai":{"cloudEnabled":true,"selectionMenuEnabled":true}})";
  CHECK(ToolboxPolicyAllows(enabled, true));
  CHECK(ToolboxPolicyAllows(enabled, false));

  const std::string menu_disabled =
      R"({"ai":{"cloudEnabled":true,"selectionMenuEnabled":false}})";
  CHECK(!ToolboxPolicyAllows(menu_disabled, true));
  CHECK(ToolboxPolicyAllows(menu_disabled, false));

  CHECK(!ToolboxPolicyAllows(
      R"({"ai":{"cloudEnabled":false,"selectionMenuEnabled":true}})",
      false));
  CHECK(!ToolboxPolicyAllows(
      R"({"ai":{"cloudEnabled":true,"askAnythingSkillEnabled":false,"polishSkillEnabled":false,"sourceCheckSkillEnabled":false,"researchAssistSkillEnabled":false,"publishFormattingSkillEnabled":false,"translationSkillEnabled":false,"promptOptimizeSkillEnabled":false}})",
      false));
  return true;
}

struct WindowSearch {
  const wchar_t *class_name;
  HWND found;
};

BOOL CALLBACK FindStatusUi(HWND window, LPARAM parameter) {
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != GetCurrentProcessId())
    return TRUE;
  auto *search = reinterpret_cast<WindowSearch *>(parameter);
  wchar_t name[64]{};
  GetClassNameW(window, name, static_cast<int>(std::size(name)));
  if (std::wstring_view(name) != search->class_name)
    return TRUE;
  search->found = window;
  return FALSE;
}

HWND ProbeClass(const wchar_t *class_name) {
  WindowSearch search{class_name, nullptr};
  EnumWindows(&FindStatusUi, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

HWND Probe() { return ProbeClass(L"FamoRuntimeStatusUi"); }

bool WaitFor(const StatusUi &ui, uint64_t registrations) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (ui.icon_registrations() >= registrations)
      return true;
    Sleep(10);
  }
  return false;
}

std::shared_ptr<RuntimeSnapshot> Snapshot(bool focused, uint32_t status_flags) {
  auto snapshot = std::make_shared<RuntimeSnapshot>();
  snapshot->ui_state.focused = focused;
  snapshot->composition.status_flags = status_flags;
  return snapshot;
}

// The icon must come back on its own after an explorer restart; the legacy
// stack got this from CSystemTray, and nothing in the new runtime does unless
// TaskbarCreated is handled.
bool TrayReregistersAfterTaskbarCreated() {
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  CHECK(ui.status_flags() == FAMO_STATUS_SIMPLIFIED);
  CHECK(ui.Start());
  CHECK(WaitFor(ui, 1));

  HWND window = Probe();
  CHECK(window != nullptr);

  const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  CHECK(taskbar_created != 0);
  CHECK(PostMessageW(window, taskbar_created, 0, 0));
  CHECK(WaitFor(ui, 2));

  ui.Stop();
  CHECK(Probe() == nullptr);
  return true;
}

bool KeyboardHookFailureIsVisibleAndRecovers() {
  _putenv_s("FAMO_TEST_KEYBOARD_HOOK_FAILURES", "2");
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  CHECK(ui.Start());
  CHECK(!ui.keyboard_hook_ready());
  CHECK(ui.keyboard_hook_error() == ERROR_ACCESS_DENIED);
  for (int attempt = 0; attempt < 300 && !ui.keyboard_hook_ready(); ++attempt)
    Sleep(10);
  CHECK(ui.keyboard_hook_ready());
  CHECK(ui.keyboard_hook_error() == ERROR_SUCCESS);
  ui.Stop();
  _putenv_s("FAMO_TEST_KEYBOARD_HOOK_FAILURES", "");
  return true;
}

// A defocused publish clears the whole composition, status_flags included.
// Treating that as "mode is now Chinese" would flip the icon on every focus
// change, so unfocused snapshots must not move it.
bool DefocusedSnapshotsDoNotMoveTheIcon() {
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  CHECK(ui.Start());

  ui.Publish(Snapshot(true, FAMO_STATUS_ASCII_MODE));
  CHECK(ui.status_flags() == FAMO_STATUS_ASCII_MODE);

  ui.Publish(Snapshot(false, 0));
  CHECK(ui.status_flags() == FAMO_STATUS_ASCII_MODE);

  ui.Publish(Snapshot(true, 0));
  CHECK(ui.status_flags() == 0);

  ui.Stop();
  return true;
}

bool OpenSessionOn(RuntimeService *service) {
  std::string error;
  CHECK(service->Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service->InitializeControlState() == ControlError::None);
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {11, 12, 13, 0, 0, 0};
  CHECK(service->Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {11, 12, 13, 14, 15, 1};
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service->Dispatch(open).status == Status::Ok);
  return true;
}

// The menu's write path. The test engine accepts options without modelling
// them, so this pins the reporting contract rather than the resulting mode:
// no live session or a rejecting engine must not read as success.
bool SetOptionReportsHonestly() {
  RuntimeService idle;
  CHECK(!idle.SetOption("ascii_mode", true));

  RuntimeService service;
  CHECK(OpenSessionOn(&service));
  CHECK(service.SetOption("ascii_mode", true));

  // A status-bar toggle is global runtime state: a later text field must
  // inherit it instead of silently returning to the config default.
  _putenv_s("FAMO_TEST_FAIL_OPTION", "ascii_mode");
  Frame hello;
  hello.command = Command::Hello;
  hello.correlation = {21, 22, 23, 0, 0, 0};
  CHECK(service.Dispatch(hello).status == Status::Ok);
  Frame open;
  open.command = Command::OpenSession;
  open.correlation = {21, 22, 23, 24, 25, 1};
  std::string error;
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.Dispatch(open).status == Status::EngineError);
  _putenv_s("FAMO_TEST_FAIL_OPTION", "");
  service.Stop();

  _putenv_s("FAMO_TEST_FAIL_OPTION", "ascii_mode");
  RuntimeService rejecting;
  const bool opened = OpenSessionOn(&rejecting);
  const bool applied = opened && rejecting.SetOption("ascii_mode", true);
  rejecting.Stop();
  _putenv_s("FAMO_TEST_FAIL_OPTION", "");
  CHECK(opened);
  CHECK(!applied);
  return true;
}

// Opening and focusing a context must publish the engine's option state before
// the first key. Otherwise the empty OpenSession snapshot renders simplified as
// traditional until ProcessKey finally refreshes it.
bool OpenSessionPublishesStatusBeforeFirstKey() {
  _putenv_s("FAMO_TEST_SIMPLIFIED", "1");
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  service.SetSnapshotSink(&ui);
  CHECK(ui.Start());

  ui.Publish(Snapshot(true, 0));
  CHECK(ui.status_flags() == 0);
  CHECK(OpenSessionOn(&service));

  Frame update;
  update.command = Command::UpdateUiState;
  update.correlation = {11, 12, 13, 14, 15, 2};
  UiState focused;
  focused.focused = true;
  std::string error;
  CHECK(EncodeUiState(focused, &update.payload, &error));
  CHECK(service.Dispatch(update).status == Status::Ok);
  CHECK(ui.status_flags() == FAMO_STATUS_SIMPLIFIED);

  service.Stop();
  ui.Stop();
  _putenv_s("FAMO_TEST_SIMPLIFIED", "");
  return true;
}

// The bar sits over whatever the user is typing into. Losing any one of these
// styles turns it from an overlay into a window that steals the caret.
bool BarNeverStealsFocus() {
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  CHECK(ui.Start());

  HWND bar = ProbeClass(L"FamoRuntimeStatusBar");
  CHECK(bar != nullptr);
  const LONG_PTR extended = GetWindowLongPtrW(bar, GWL_EXSTYLE);
  CHECK((extended & WS_EX_LAYERED) != 0);
  CHECK((extended & WS_EX_NOACTIVATE) != 0);
  CHECK((extended & WS_EX_TOOLWINDOW) != 0);
  CHECK((extended & WS_EX_TOPMOST) != 0);
  CHECK((GetWindowLongPtrW(bar, GWL_STYLE) & WS_POPUP) != 0);
  CHECK(SendMessageW(bar, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
  // A NULL class cursor leaves whatever shape the pointer arrived with, so a
  // busy cursor from another app sticks over the bar until it leaves.
  CHECK(GetClassLongPtrW(bar, GCLP_HCURSOR) != 0);

  ui.Stop();
  CHECK(ProbeClass(L"FamoRuntimeStatusBar") == nullptr);
  return true;
}

// Hit-testing is where a click becomes an option, so it is checked as pure
// geometry rather than through synthesised mouse input.
bool BarHitTestMapsToOptions() {
  const StatusBarLayout layout = StatusBarLayoutFor(96);
  CHECK(layout.width > 0 && layout.height > 0);
  CHECK(StatusBarHitTest(layout, -1, -1) < 0);
  CHECK(StatusBarHitTest(layout, layout.width, layout.height) < 0);

  static const char *const expected[] = {"ascii_mode", "ascii_punct",
                                         "traditionalization", "full_shape"};
  static_assert(std::size(expected) == kStatusBarButtonCount, "one per button");
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    const StatusBarLayout::Button &button = layout.buttons[index];
    CHECK(button.left >= 0 && button.right <= layout.width);
    CHECK(button.top >= 0 && button.bottom <= layout.height);
    const int x = (button.left + button.right) / 2;
    const int y = (button.top + button.bottom) / 2;
    CHECK(StatusBarHitTest(layout, x, y) == index);
    CHECK(std::string_view(StatusBarOption(index)) == expected[index]);
    if (index == 2)
      CHECK(std::string_view(StatusBarSecondaryOption(index)) == "zh_trad");
    else
      CHECK(StatusBarSecondaryOption(index) == nullptr);
  }
  CHECK(StatusBarOption(-1) == nullptr);
  CHECK(StatusBarOption(kStatusBarButtonCount) == nullptr);
  CHECK(StatusBarNextOptionValue(0, 0));
  CHECK(!StatusBarNextOptionValue(FAMO_STATUS_ASCII_MODE, 0));
  CHECK(!StatusBarNextOptionValue(0, 2));
  CHECK(StatusBarNextOptionValue(FAMO_STATUS_SIMPLIFIED, 2));
  // Only the trough inset is drag surface. The segments abut, so the shared
  // edge belongs to the segment on its right rather than falling in a gap.
  CHECK(StatusBarHitTest(layout, 0, layout.height / 2) < 0);
  CHECK(StatusBarHitTest(layout, layout.width / 2, 0) < 0);
  CHECK(StatusBarHitTest(layout, layout.buttons[0].right, layout.height / 2) ==
        1);
  CHECK(layout.buttons[1].left == layout.buttons[0].right);

  // Every logical metric scales, so a 150% bar is bigger and still maps.
  const StatusBarLayout scaled = StatusBarLayoutFor(144);
  CHECK(scaled.width > layout.width);
  CHECK(scaled.height > layout.height);
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    const StatusBarLayout::Button &button = scaled.buttons[index];
    const int x = (button.left + button.right) / 2;
    const int y = (button.top + button.bottom) / 2;
    CHECK(StatusBarHitTest(scaled, x, y) == index);
  }
  return true;
}

// A persisted position outlives the display it was saved on, so a restore that
// does not clamp leaves the bar unreachable off-screen.
bool BarPositionPersistsAndClamps() {
  wchar_t directory[MAX_PATH]{};
  CHECK(GetTempPathW(static_cast<DWORD>(std::size(directory)), directory) != 0);
  const std::wstring path = std::wstring(directory) + L"famo-statusbar-test.txt";
  DeleteFileW(path.c_str());

  int x = 0;
  int y = 0;
  CHECK(!StatusBarLoadPosition(path, &x, &y));
  CHECK(StatusBarSavePosition(path, 321, -654));
  CHECK(StatusBarLoadPosition(path, &x, &y));
  CHECK(x == 321);
  CHECK(y == -654);
  CHECK(DeleteFileW(path.c_str()));

  const StatusBarLayout layout = StatusBarLayoutFor(96);
  for (const POINT stranded : {POINT{500000, 500000}, POINT{-500000, -500000}}) {
    int left = stranded.x;
    int top = stranded.y;
    StatusBarClampToWorkArea(&left, &top, layout.width, layout.height);
    const POINT centre{left + layout.width / 2, top + layout.height / 2};
    MONITORINFO info{sizeof(info)};
    CHECK(GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST),
                          &info));
    CHECK(left >= info.rcWork.left);
    CHECK(top >= info.rcWork.top);
    CHECK(left + layout.width <= info.rcWork.right);
    CHECK(top + layout.height <= info.rcWork.bottom);
  }
  return true;
}

// What the bar draws must track the engine's real mode, and must survive the
// defocused publish that clears status_flags wholesale.
bool BarDrawnStateFollowsStatusFlags() {
  RuntimeService service;
  std::atomic<bool> running{true};
  StatusUi ui(&service, &running);
  CHECK(ui.Start());

  ui.Publish(Snapshot(true, FAMO_STATUS_ASCII_PUNCT | FAMO_STATUS_FULL_SHAPE));
  const uint32_t mixed = ui.status_flags();
  CHECK(!StatusBarButtonOn(mixed, 0));
  CHECK(StatusBarButtonOn(mixed, 1));
  CHECK(!StatusBarButtonOn(mixed, 2));
  CHECK(StatusBarButtonOn(mixed, 3));
  // Labels name the state the option is in, not the action a click performs.
  CHECK(std::string_view(StatusBarLabel(mixed, 0)) == "中");
  CHECK(std::string_view(StatusBarLabel(mixed, 1)) == ".");
  CHECK(std::string_view(StatusBarLabel(mixed, 2)) == "繁");
  CHECK(std::string_view(StatusBarLabel(mixed, 3)) == "全");
  CHECK(StatusBarLabel(mixed, kStatusBarButtonCount) == nullptr);

  ui.Publish(Snapshot(false, 0));
  CHECK(ui.status_flags() == mixed);

  ui.Publish(Snapshot(true, FAMO_STATUS_ASCII_MODE | FAMO_STATUS_SIMPLIFIED));
  const uint32_t flipped = ui.status_flags();
  CHECK(StatusBarButtonOn(flipped, 0));
  CHECK(!StatusBarButtonOn(flipped, 1));
  CHECK(StatusBarButtonOn(flipped, 2));
  CHECK(!StatusBarButtonOn(flipped, 3));
  CHECK(std::string_view(StatusBarLabel(flipped, 0)) == "英");
  CHECK(std::string_view(StatusBarLabel(flipped, 1)) == "。");
  CHECK(std::string_view(StatusBarLabel(flipped, 2)) == "简");
  CHECK(std::string_view(StatusBarLabel(flipped, 3)) == "半");

  ui.Stop();
  return true;
}

// The trap the bar exists inside: text drawn with GDI onto the premultiplied
// DIB behind UpdateLayeredWindow lands with alpha 0 and is invisible on screen,
// which no geometry assertion would catch. Paint one frame and read the bitmap.
bool BarPaintProducesVisiblePixels() {
  const StatusBarLayout layout = StatusBarLayoutFor(96);
  const FamoSkin skin = FamoSkinDefault();
  FamoSkin text_skin = skin;
  text_skin.text_font.point_size = 12.0f;
  FamoTextResources *resources = FamoTextResourcesCreate(&text_skin, 96);
  CHECK(resources != nullptr);

  DibSurface surface;
  CHECK(surface.Ensure(layout.width, layout.height));
  surface.Clear();

  FamoStatusBarButton buttons[kStatusBarButtonCount]{};
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    const StatusBarLayout::Button &bounds = layout.buttons[index];
    buttons[index].bounds = {bounds.left, bounds.top, bounds.right,
                             bounds.bottom};
    buttons[index].label = StatusBarLabel(0, index);
  }
  // Enable 简/繁, leaving 。 plain: the label carries the idle state, so both
  // still use the card fill and the centroid checks can read ink by darkness.
  buttons[2].on = 1u;
  FamoStatusBarSpec spec{};
  spec.size = static_cast<uint32_t>(sizeof(spec));
  spec.bar_size = {layout.width, layout.height};
  spec.dpi = 96;
  spec.button_count = kStatusBarButtonCount;
  spec.buttons = buttons;
  const int32_t painted =
      FamoStatusBarPaint(&spec, &skin, resources, surface.dc());
  CHECK(painted == FAMO_UI_OK);

  auto *bitmap = static_cast<HBITMAP>(GetCurrentObject(surface.dc(), OBJ_BITMAP));
  DIBSECTION section{};
  CHECK(GetObjectW(bitmap, sizeof(section), &section) == sizeof(section));
  const auto *pixels = static_cast<const uint32_t *>(section.dsBm.bmBits);
  const int stride = section.dsBm.bmWidthBytes / 4;
  const auto at = [&](int x, int y) {
    return pixels[static_cast<size_t>(y) * stride + x];
  };

  // Rounded corners stay cut out, and the trough body is opaque.
  CHECK((at(0, 0) >> 24) == 0);
  const uint32_t trough = at(layout.width / 2, 1);
  CHECK((trough >> 24) == 0xffu);
  CHECK(trough == skin.card2_color);
  // Segment fill sampled above the glyph; the trough must be distinguishable
  // from it or the segmented control reads as one flat slab.
  const uint32_t fill = at((layout.buttons[0].left + layout.buttons[0].right) / 2,
                           layout.buttons[0].top + 1);
  CHECK((fill >> 24) == 0xffu);
  CHECK(fill != trough);
  // Enabled state must not leave a persistent accent block behind the label.
  CHECK(at((layout.buttons[2].left + layout.buttons[2].right) / 2,
            layout.buttons[2].top + 1) == fill);

  FamoSkin legacy = skin;
  legacy.size = static_cast<uint32_t>(offsetof(FamoSkin, card2_color));
  surface.Clear();
  CHECK(FamoStatusBarPaint(&spec, &legacy, resources, surface.dc()) == FAMO_UI_OK);
  CHECK(at(layout.width / 2, 1) == 0xFFF0EEEAu);

  // Glyph ink is the skin's dark text color; card, trough and divider are all
  // light, so a luminance threshold isolates the glyph without knowing which.
  const auto inked = [&](uint32_t p) {
    return (((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff)) / 3 < 160;
  };
  // Centre of mass of the ink, versus the centre of the segment it sits in.
  // Before centring on the inked extents, 。 landed low and left of centre by
  // several px while the square glyphs were already centred -- so the same
  // tolerance has to hold for both, or the correction was a blanket offset.
  const auto ink_offset = [&](int index, double *dx, double *dy) {
    const StatusBarLayout::Button &button = layout.buttons[index];
    double sum_x = 0;
    double sum_y = 0;
    int count = 0;
    for (int y = button.top; y < button.bottom; ++y)
      for (int x = button.left; x < button.right; ++x)
        if (inked(at(x, y))) {
          sum_x += x;
          sum_y += y;
          ++count;
        }
    if (count == 0)
      return false;
    *dx = sum_x / count - (button.left + button.right) / 2.0;
    *dy = sum_y / count - (button.top + button.bottom) / 2.0;
    return true;
  };

  // Button 1 is 。 -- ink in one corner of a full-width em box, the glyph the
  // correction exists for. Button 0 is 中, whose ink already fills its box.
  for (const int index : {0, 1}) {
    double dx = 0;
    double dy = 0;
    CHECK(ink_offset(index, &dx, &dy));
    // Measured: box-centred 。 lands at (-5.5, +5.5) and fails this; centred on
    // its ink it lands at (-0.5, -0.5). 中 reads (-1.0, -0.7) either way, which
    // is what makes the correction glyph-relative rather than a blanket offset.
    CHECK(std::abs(dx) <= 1.5);
    CHECK(std::abs(dy) <= 1.5);
  }

  // Mouse-down stays neutral too: state is communicated by the label alone.
  buttons[2].pressed = 1u;
  surface.Clear();
  CHECK(FamoStatusBarPaint(&spec, &skin, resources, surface.dc()) == FAMO_UI_OK);
  const uint32_t pressed_fill =
      at((layout.buttons[2].left + layout.buttons[2].right) / 2,
         layout.buttons[2].top + 1);
  CHECK(pressed_fill != skin.hilited_back_color);
  buttons[2].pressed = 0u;
  FamoTextResourcesDestroy(resources);

  // A dark skin must use its exact card2 token too.
  FamoSkin dark = skin;
  dark.back_color = 0xFF262321u;         // shenda dark card
  dark.card2_color = 0xFF211E1Cu;        // shenda dark card2
  dark.text_color = 0xFFECE4D8u;         // ink
  dark.hilited_back_color = 0xFFE06A8Eu; // accent
  dark.hilited_text_color = 0xFF1A1816u; // onAccent
  FamoTextResources *dark_res = FamoTextResourcesCreate(&text_skin, 96);
  CHECK(dark_res != nullptr);
  surface.Clear();
  const int32_t dark_painted =
      FamoStatusBarPaint(&spec, &dark, dark_res, surface.dc());
  FamoTextResourcesDestroy(dark_res);
  CHECK(dark_painted == FAMO_UI_OK);
  const auto luma = [](uint32_t p) {
    return (((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff)) / 3;
  };
  const uint32_t dark_trough = at(layout.width / 2, 1);
  CHECK(dark_trough == dark.card2_color);
  const uint32_t dark_fill = at((layout.buttons[0].left + layout.buttons[0].right) / 2,
                                layout.buttons[0].top + 1);
  CHECK(dark_trough != dark_fill);
  CHECK(luma(dark_trough) < luma(dark_fill));
  CHECK(luma(trough) < luma(fill));
  return true;
}

// Windows keys tray visibility off the executable path, and the versioned
// install layout hands it a new one on every update -- so the icon falls back
// to the overflow flyout each time. Fixing that means finding the shell's entry
// for this build, and the shell stores those paths known-folder relative rather
// than literal, which no plain string compare would ever match.
bool TrayPathMatchesTheShellsStoredForm() {
  const std::wstring installed =
      L"C:\\Program Files\\Famo\\versions\\1.4.7-B18B070B-2026\\FamoRuntime.exe";
  // FOLDERID_ProgramFiles, in the form the shell actually writes.
  const std::wstring folder = L"{6D809377-6AF0-444B-8957-A3773F02200E}";
  CHECK(StatusTrayPathMatches(
      folder + L"\\Famo\\versions\\1.4.7-B18B070B-2026\\FamoRuntime.exe",
      installed));
  // The previous update's icon is a different identity, not this one.
  CHECK(!StatusTrayPathMatches(
      folder + L"\\Famo\\versions\\1.4.6-A0000000-2026\\FamoRuntime.exe",
      installed));
  // Neither is a sibling executable shipped in the same directory.
  CHECK(!StatusTrayPathMatches(
      folder + L"\\Famo\\versions\\1.4.7-B18B070B-2026\\FamoSettings.exe",
      installed));
  // Dev and portable builds are stored as plain paths, cased however they were
  // launched -- these are filesystem paths, so the compare is case-insensitive.
  CHECK(StatusTrayPathMatches(
      L"c:\\program files\\famo\\versions\\1.4.7-b18b070b-2026\\famoruntime.exe",
      installed));
  CHECK(
      !StatusTrayPathMatches(L"D:\\build\\Release\\FamoRuntime.exe", installed));
  // The tail has to sit under some root, or a rootless module path would match
  // the entry of anything ending the same way.
  CHECK(!StatusTrayPathMatches(
      folder + L"\\Famo\\versions\\1.4.7-B18B070B-2026\\FamoRuntime.exe",
      L"\\Famo\\versions\\1.4.7-B18B070B-2026\\FamoRuntime.exe"));
  CHECK(!StatusTrayPathMatches(L"", installed));
  CHECK(!StatusTrayPathMatches(installed, L""));
  CHECK(!StatusTrayPathMatches(folder, installed));
  CHECK(!StatusTrayPathMatches(L"{6D809377-6AF0", installed));
  return true;
}

// default.yaml is regenerated by rime on every deploy, so the block is
// authoritative but the file around it is not ours to assume much about.
bool SchemaListParsesFromDefaultYaml() {
  const auto parse = [](const char *yaml) {
    std::istringstream stream(yaml);
    return StatusBarParseSchemaList(stream);
  };

  const std::vector<std::string> plain = parse(
      "schema_list:\n"
      "  - schema: rime_ice\n"
      "  - schema: wubi86_jidian\n"
      "  - schema: double_pinyin_flypy\n");
  CHECK(plain.size() == 3);
  CHECK(plain[0] == "rime_ice");
  CHECK(plain[1] == "wubi86_jidian");
  CHECK(plain[2] == "double_pinyin_flypy");

  // The real file continues into other top-level keys straight after the list.
  const std::vector<std::string> followed = parse(
      "config_version: \"1\"\n"
      "schema_list:\n"
      "  - schema: rime_ice\n"
      "  - schema: t9\n"
      "switcher:\n"
      "  abbreviate_options: true\n"
      "  - schema: not_reached\n");
  CHECK(followed.size() == 2);
  CHECK(followed[1] == "t9");

  CHECK(parse("switcher:\n  caption: x\n").empty());
  CHECK(parse("").empty());
  CHECK(parse("schema_list:\n").empty());
  // A truncated deploy can leave the key with nothing under it.
  CHECK(parse("schema_list:\nswitcher:\n").empty());

  // One unusable id must cost only itself, not the rest of the menu.
  const std::vector<std::string> dirty = parse(
      "schema_list:\n"
      "  - schema: rime_ice\n"
      "  - schema: ../../etc/passwd\n"
      "  - schema: has space\n"
      "  - schema:\n"
      "  - schema: t9\n");
  CHECK(dirty.size() == 2);
  CHECK(dirty[0] == "rime_ice");
  CHECK(dirty[1] == "t9");
  return true;
}

// The name sits ~800 lines into a 50 KB file, adjacent to its schema_id.
bool SchemaNameParsesFromSchemaYaml() {
  const auto find = [](const char *yaml, const char *id, std::string *name) {
    std::istringstream stream(yaml);
    return StatusBarParseSchemaName(stream, id, name);
  };
  std::string name;

  CHECK(find("schema:\n  name: \"雾凇拼音\"\n  schema_id: rime_ice\n",
             "rime_ice", &name));
  CHECK(name == "雾凇拼音");

  // Unquoted, and preceded by plenty of unrelated content.
  name.clear();
  CHECK(find("speller:\n  alphabet: abc\nschema:\n  name: 中文九键\n"
             "  schema_id: t9\n  version: 3.0.0\n",
             "t9", &name));
  CHECK(name == "中文九键");

  // Absent, wrong id, and a schema_id whose name is not adjacent -- all fall
  // back rather than returning a name belonging to something else.
  name.clear();
  CHECK(!find("schema:\n  schema_id: rime_ice\n", "rime_ice", &name));
  CHECK(!find("schema:\n  name: \"雾凇拼音\"\n  schema_id: rime_ice\n", "t9",
              &name));
  CHECK(!find("  name: \"雾凇拼音\"\n  version: 1\n  schema_id: rime_ice\n",
              "rime_ice", &name));
  CHECK(!find("", "rime_ice", &name));
  return true;
}

// The first character of the name is meaningless for most schemas (雾, 极), so
// the glyph is an ordered keyword match instead.
bool SchemaGlyphNamesTheInputMethod() {
  CHECK(StatusBarSchemaGlyph("雾凇拼音") == "拼");
  CHECK(StatusBarSchemaGlyph("极点五笔86") == "五");
  // 双拼 contains 拼: the ordering is what keeps this off the pinyin glyph.
  CHECK(StatusBarSchemaGlyph("小鹤双拼") == "双");
  CHECK(StatusBarSchemaGlyph("中文九键") == "九");
  // Both families named: neither alone describes it.
  CHECK(StatusBarSchemaGlyph("五笔拼音混输") == "混");
  // No keyword: the first character is all there is, and it must survive as a
  // whole UTF-8 code point rather than a lead byte.
  CHECK(StatusBarSchemaGlyph("仓颉") == "仓");
  CHECK(StatusBarSchemaGlyph("Bopomofo") == "B");
  CHECK(StatusBarSchemaGlyph("").empty());
  return true;
}

// A stale previous-schema value equal to the live schema used to make every
// click a silent no-op. A click must still select another available schema.
bool SchemaClickAlwaysHasADifferentTarget() {
  const std::vector<std::string> schemas = {"rime_ice", "wubi86_jidian"};
  CHECK(StatusBarSchemaSwitchTarget("rime_ice", "wubi86_jidian", schemas) ==
        "wubi86_jidian");
  CHECK(StatusBarSchemaSwitchTarget("rime_ice", "rime_ice", schemas) ==
        "wubi86_jidian");
  CHECK(StatusBarSchemaSwitchTarget("rime_ice", "removed_schema", schemas) ==
        "wubi86_jidian");
  CHECK(StatusBarSchemaSwitchTarget("wubi86_jidian", "", schemas) ==
        "rime_ice");
  CHECK(StatusBarSchemaSwitchTarget("rime_ice", "", {"rime_ice"}).empty());
  return true;
}

// Every state file written before the schema segment existed holds only `x y`.
bool PreviousSchemaPersistsBesidePosition() {
  wchar_t directory[MAX_PATH]{};
  CHECK(GetTempPathW(static_cast<DWORD>(std::size(directory)), directory) != 0);
  const std::wstring path = std::wstring(directory) + L"famo-statusbar-schema.txt";
  DeleteFileW(path.c_str());

  int x = 0;
  int y = 0;
  std::string previous;
  CHECK(StatusBarSavePosition(path, 120, 340, "rime_ice"));
  CHECK(StatusBarLoadPosition(path, &x, &y));
  CHECK(x == 120 && y == 340);
  CHECK(StatusBarLoadPreviousSchema(path, &previous));
  CHECK(previous == "rime_ice");

  // A legacy two-integer file keeps its position and simply has no previous.
  std::ofstream(path, std::ios::trunc) << "77 88\n";
  x = y = 0;
  previous = "stale";
  CHECK(StatusBarLoadPosition(path, &x, &y));
  CHECK(x == 77 && y == 88);
  CHECK(StatusBarLoadPreviousSchema(path, &previous));
  CHECK(previous.empty());

  // A rejected id must not come back out as one.
  std::ofstream(path, std::ios::trunc) << "5 6\n../../etc/passwd\n";
  previous = "stale";
  CHECK(StatusBarLoadPreviousSchema(path, &previous));
  CHECK(previous.empty());

  // Saving after a drag, with no schema ever switched, stays loadable.
  CHECK(StatusBarSavePosition(path, 9, 10));
  CHECK(StatusBarLoadPosition(path, &x, &y));
  CHECK(x == 9 && y == 10);
  CHECK(StatusBarLoadPreviousSchema(path, &previous));
  CHECK(previous.empty());

  CHECK(DeleteFileW(path.c_str()));
  CHECK(!StatusBarLoadPreviousSchema(path, &previous));
  return true;
}

// Right-click now has two destinations, so the segment boundary is the routing
// decision and has to be exact.
bool SchemaSegmentRoutesItsOwnRightClick() {
  for (const uint32_t dpi : {96u, 144u}) {
    const StatusBarLayout layout = StatusBarLayoutFor(dpi);
    const StatusBarLayout::Button &schema = layout.schema;
    CHECK(schema.right > schema.left);
    // Wider than a toggle, and set apart from the strip by bare trough.
    CHECK(schema.right - schema.left >
          layout.buttons[0].right - layout.buttons[0].left);
    CHECK(layout.buttons[0].left > schema.right);

    const int y = (schema.top + schema.bottom) / 2;
    const int centre = (schema.left + schema.right) / 2;
    // On the schema segment: schema list, and never mistaken for a toggle.
    CHECK(StatusBarHitsSchema(layout, centre, y));
    CHECK(StatusBarHitTest(layout, centre, y) < 0);
    // On a toggle: option menu, and never mistaken for the schema segment.
    for (int index = 0; index < kStatusBarButtonCount; ++index) {
      const StatusBarLayout::Button &button = layout.buttons[index];
      const int x = (button.left + button.right) / 2;
      CHECK(StatusBarHitTest(layout, x, y) == index);
      CHECK(!StatusBarHitsSchema(layout, x, y));
    }
    // The gap between them is neither -- it is drag surface.
    const int gap = (schema.right + layout.buttons[0].left) / 2;
    CHECK(!StatusBarHitsSchema(layout, gap, y));
    CHECK(StatusBarHitTest(layout, gap, y) < 0);
    // Boundaries: right edge is exclusive, left edge inclusive.
    CHECK(StatusBarHitsSchema(layout, schema.left, y));
    CHECK(!StatusBarHitsSchema(layout, schema.right, y));
    CHECK(!StatusBarHitsSchema(layout, centre, schema.top - 1));
    CHECK(!StatusBarHitsSchema(layout, centre, schema.bottom));
  }
  return true;
}

bool DumpBarFrame() {
  // Manual visualization harness, not a check: paints one frame and dumps it to
  // a BMP so a human can eyeball the schema segment. Both env vars are required
  // input; under ctest neither is set and getenv returns nullptr -- feeding
  // that to string_view/ofstream was a guaranteed segfault. Absent vars mean
  // "not asked to dump", and the selfcheck stays green.
  const char *bar_name = std::getenv("FAMO_BAR_NAME");
  const char *dump_path = std::getenv("FAMO_BAR_DUMP");
  if (!bar_name || !dump_path)
    return true;
  const uint32_t dpi = 144;
  const StatusBarLayout layout = StatusBarLayoutFor(dpi);
  const FamoSkin skin = FamoSkinDefault();
  FamoSkin text_skin = skin;
  text_skin.text_font.point_size = 12.0f;
  FamoTextResources *res = FamoTextResourcesCreate(&text_skin, dpi);
  CHECK(res != nullptr);
  DibSurface surface;
  CHECK(surface.Ensure(layout.width, layout.height));
  surface.Clear();
  const uint32_t flags = FAMO_STATUS_SIMPLIFIED;
  const std::string glyph = StatusBarSchemaGlyph(bar_name);
  FamoStatusBarButton buttons[kStatusBarButtonCount + 1]{};
  buttons[0].bounds = {layout.schema.left, layout.schema.top,
                       layout.schema.right, layout.schema.bottom};
  buttons[0].label = glyph.c_str();
  for (int i = 0; i < kStatusBarButtonCount; ++i) {
    const StatusBarLayout::Button &b = layout.buttons[i];
    buttons[i + 1].bounds = {b.left, b.top, b.right, b.bottom};
    buttons[i + 1].label = StatusBarLabel(flags, i);
    buttons[i + 1].on = StatusBarButtonOn(flags, i) ? 1u : 0u;
  }
  FamoStatusBarSpec spec{};
  spec.size = static_cast<uint32_t>(sizeof(spec));
  spec.bar_size = {layout.width, layout.height};
  spec.dpi = dpi;
  spec.button_count = kStatusBarButtonCount + 1;
  spec.buttons = buttons;
  CHECK(FamoStatusBarPaint(&spec, &skin, res, surface.dc()) == FAMO_UI_OK);
  FamoTextResourcesDestroy(res);
  auto *bmp = static_cast<HBITMAP>(GetCurrentObject(surface.dc(), OBJ_BITMAP));
  DIBSECTION ds{};
  CHECK(GetObjectW(bmp, sizeof(ds), &ds) == sizeof(ds));
  const auto *px = static_cast<const uint32_t *>(ds.dsBm.bmBits);
  const int stride = ds.dsBm.bmWidthBytes / 4;
  std::vector<uint32_t> out(static_cast<size_t>(layout.width) * layout.height);
  for (int y = 0; y < layout.height; ++y)
    for (int x = 0; x < layout.width; ++x) {
      const uint32_t p = px[static_cast<size_t>(y) * stride + x];
      const uint32_t a = p >> 24;
      const auto over = [&](uint32_t s) { return ((p >> s) & 0xff) + 0xBEu * (255u - a) / 255u; };
      out[static_cast<size_t>(y) * layout.width + x] = (over(16) << 16) | (over(8) << 8) | over(0);
    }
  BITMAPFILEHEADER fh{0x4D42, 0, 0, 0, sizeof(fh) + sizeof(BITMAPINFOHEADER)};
  BITMAPINFOHEADER ih{sizeof(ih), layout.width, -layout.height, 1, 32, BI_RGB};
  fh.bfSize = fh.bfOffBits + static_cast<DWORD>(out.size() * 4);
  std::ofstream f(dump_path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
  f.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
  f.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size() * 4));
  std::printf("dumped %dx%d glyph=%s\n", layout.width, layout.height, glyph.c_str());
  return true;
}

} // namespace

int main() {
  if (!DoubleAltRequiresTwoShortCleanTaps() ||
      !GlobalHotKeysAcceptOnlyRestrictedCanonicalBindings() ||
      !ToolboxPolicyMatchesGestureAndRecordedHotKeySemantics() ||
      !SchemaListParsesFromDefaultYaml() || !SchemaNameParsesFromSchemaYaml() ||
      !SchemaGlyphNamesTheInputMethod() ||
      !SchemaClickAlwaysHasADifferentTarget() ||
      !PreviousSchemaPersistsBesidePosition() ||
      !SchemaSegmentRoutesItsOwnRightClick())
    return 1;
  if (!TrayPathMatchesTheShellsStoredForm() ||
      !TrayReregistersAfterTaskbarCreated() ||
      !KeyboardHookFailureIsVisibleAndRecovers() ||
      !DefocusedSnapshotsDoNotMoveTheIcon() || !SetOptionReportsHonestly() ||
      !OpenSessionPublishesStatusBeforeFirstKey() ||
      !BarNeverStealsFocus() || !BarHitTestMapsToOptions() ||
      !BarPositionPersistsAndClamps() || !BarDrawnStateFollowsStatusFlags() ||
      !BarPaintProducesVisiblePixels() || !DumpBarFrame())
    return 1;
  std::printf("status_ui_selfcheck: OK\n");
  return 0;
}
