#include "famo_status_ui.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include "../../famo-candidate-ui/famo_candidate_ui.h"
#include "candidate_skin.h"
#include "dib_surface.h"

namespace famo::runtime {
namespace {

constexpr wchar_t kClassName[] = L"FamoRuntimeStatusUi";
// A separate class from the hidden tray window: the bar is visible, needs a
// real cursor, and the tray probe finds its own window by class name.
constexpr wchar_t kBarClassName[] = L"FamoRuntimeStatusBar";
constexpr wchar_t kTip[] = L"法墨";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kStatusChanged = WM_APP + 2;
constexpr UINT kStyleChanged = WM_APP + 3;
// Posted by the switch worker once ControlSelectSchema has actually succeeded,
// so the "previously used" bookkeeping is committed on the UI thread that owns
// it rather than raced into from the worker.
constexpr UINT kSchemaSwitched = WM_APP + 4;
constexpr UINT kSummonToolboxGesture = WM_APP + 5;
constexpr UINT kSummonQuickPhrase = WM_APP + 6;
constexpr UINT kSummonToolboxHotKey = WM_APP + 7;

constexpr wchar_t kNotifyIconSettings[] = L"Control Panel\\NotifyIconSettings";
constexpr UINT_PTR kPromoteTimer = 1;
// The shell writes the entry a beat after NIM_ADD, so on a path it has never
// seen the first look always misses. Retry across ~5s, then stop scanning.
constexpr UINT kPromoteInterval = 500;
constexpr int kPromoteAttempts = 10;
constexpr UINT_PTR kKeyboardHookTimer = 2;
constexpr UINT kKeyboardHookInterval = 500;
constexpr int kKeyboardHookAttempts = 3;

// Bar metrics in logical px @96dpi. The bar is a segmented control: the window
// is the trough and the segments tile it, inset all round, sharing edges.
constexpr int kBarCell = 34;    // square toggle segment
constexpr int kBarInset = 3;    // trough padding around the segment strip
constexpr int kBarMargin = 12;  // gap to the work-area corner
constexpr int kBarDragThreshold = 4;
// The schema segment: wider than a toggle because it carries a word-ish glyph
// rather than a single mode character, and held off the toggle strip by a run
// of bare trough so it reads as a different kind of control.
constexpr int kSchemaCell = 44;
constexpr int kSchemaGap = 7;

// Internal hit code for the schema segment. Distinct from -1 (nothing) so the
// public StatusBarHitTest can keep meaning "which toggle" and nothing else.
constexpr int kSchemaSegment = -2;
// The candidate list's text font is sized for a popup and would nearly fill a
// 34px cell. Keep the skin's face for consistency, shrink the size.
constexpr float kBarFontPoint = 12.0f;

enum : UINT {
  kCmdAsciiMode = 1,
  kCmdAsciiPunct = 2,
  kCmdSimplification = 3,
  kCmdFullShape = 4,
  kCmdSettings = 10,
  kCmdDeploy = 11,
  kCmdExit = 12,
  // Schema menu entries are kCmdSchemaFirst + index into the parsed list.
  kCmdSchemaFirst = 100,
};

struct Toggle {
  UINT command;
  uint32_t status_flag;
  const char *option;
  const wchar_t *label;
  // Bar glyphs (UTF-8), naming the state the option is in rather than the
  // action a click performs -- the bar has no room to spell either out.
  const char *bar_on;
  const char *bar_off;
};

// The four options rime applies with zero deploy, matching the legacy tray menu
// (weasel-fork/features/tray-options.patch) and the macOS menu bar. Labels name
// the state the check mark means, so a checked item is never ambiguous the way
// a bare pair name like "simplified/traditional" would be.
constexpr Toggle kToggles[] = {
    {kCmdAsciiMode, FAMO_STATUS_ASCII_MODE, "ascii_mode", L"英文模式", "英",
     "中"},
    {kCmdAsciiPunct, FAMO_STATUS_ASCII_PUNCT, "ascii_punct", L"英文标点", ".",
     "。"},
    {kCmdSimplification, FAMO_STATUS_SIMPLIFIED, "traditionalization",
     L"简体字", "简", "繁"},
    {kCmdFullShape, FAMO_STATUS_FULL_SHAPE, "full_shape", L"全角", "全", "半"},
};
static_assert(sizeof(kToggles) / sizeof(kToggles[0]) == kStatusBarButtonCount,
              "the bar draws one button per menu toggle");

bool InRange(int index) {
  return index >= 0 && index < kStatusBarButtonCount;
}

// Round rather than truncate: at 125%/175% truncation accumulates a visible
// error across the button strip. Matches the renderer's own Scale.
int ScaleDpi(int value, uint32_t dpi) {
  if (dpi == 0)
    dpi = 96;
  return static_cast<int>((static_cast<int64_t>(value) * dpi + 48) / 96);
}

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  return path;
}

std::wstring ModuleDirectory() {
  const std::wstring path = ModulePath();
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

HICON LoadTrayIcon(const std::wstring &file) {
  return static_cast<HICON>(LoadImageW(nullptr, file.c_str(), IMAGE_ICON, 0, 0,
                                       LR_LOADFROMFILE | LR_DEFAULTSIZE));
}

// The default data root, and where the dragged position lives. Under the
// profile rather than beside the binary: Program Files is not writable by the
// logged-in user.
std::wstring LocalFamoDirectory() {
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &local)))
    return {};
  std::wstring directory(local);
  CoTaskMemFree(local);
  directory += L"\\Famo";
  CreateDirectoryW(directory.c_str(), nullptr);
  return directory;
}

std::wstring StatusBarStatePath() {
  const std::wstring directory = LocalFamoDirectory();
  return directory.empty() ? std::wstring() : directory + L"\\famo-statusbar.txt";
}

std::string_view TrimAscii(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

// The engine's own rule for a schema id, from runtime_control_engine.cpp's
// SafeName. Deliberately a second copy rather than widening the service header
// for a five-line predicate: ids that pass here are the only ones ever written
// to famo-select-schema.txt, and the engine re-validates on read regardless.
bool SafeSchemaName(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](char ch) {
           const auto c = static_cast<unsigned char>(ch);
           return std::isalnum(c) || ch == '_' || ch == '-' || ch == '.';
         });
}

// Strip one layer of matching quotes -- rime writes display names quoted.
std::string_view Unquote(std::string_view value) {
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
      value.back() == value.front())
    return value.substr(1, value.size() - 2);
  return value;
}

// Leading UTF-8 code point, whole. A byte would split a CJK character into
// mojibake, and the fallback glyph is precisely the case where the name is not
// ASCII.
std::string FirstUtf8Char(std::string_view value) {
  if (value.empty())
    return {};
  const auto lead = static_cast<unsigned char>(value.front());
  size_t length = 1;
  if ((lead & 0xF8u) == 0xF0u)
    length = 4;
  else if ((lead & 0xF0u) == 0xE0u)
    length = 3;
  else if ((lead & 0xE0u) == 0xC0u)
    length = 2;
  return std::string(value.substr(0, (std::min)(length, value.size())));
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool WriteSelectedSchema(const std::wstring &data_root, const std::string &id) {
  if (data_root.empty() || !SafeSchemaName(id))
    return false;
  // Binary and unterminated: SafeName rejects newlines, and the reader trims.
  std::ofstream file(data_root + L"\\famo-select-schema.txt",
                     std::ios::trunc | std::ios::binary);
  if (!file)
    return false;
  file << id;
  return file.good();
}

} // namespace

bool ParseGlobalHotKeyBinding(std::string_view text,
                              GlobalHotKeyBinding *binding) noexcept {
  if (!binding || text.empty())
    return false;
  uint32_t modifiers = MOD_NOREPEAT;
  int modifier_count = 0;
  size_t start = 0;
  for (;;) {
    const size_t plus = text.find('+', start);
    const std::string_view part = text.substr(start, plus - start);
    if (plus == std::string_view::npos) {
      if (part.size() != 1 || part[0] < 'A' || part[0] > 'Z' ||
          modifier_count < 2)
        return false;
      binding->modifiers = modifiers;
      binding->virtual_key = static_cast<uint32_t>(part[0]);
      return true;
    }
    uint32_t flag = 0;
    if (part == "Ctrl")
      flag = MOD_CONTROL;
    else if (part == "Alt")
      flag = MOD_ALT;
    else if (part == "Shift")
      flag = MOD_SHIFT;
    else
      return false;
    if ((modifiers & flag) != 0)
      return false;
    modifiers |= flag;
    ++modifier_count;
    start = plus + 1;
  }
}

bool GlobalHotKeyBindingMatches(const GlobalHotKeyBinding &binding,
                                uint32_t virtual_key, bool control, bool alt,
                                bool shift, bool windows) noexcept {
  if (windows || binding.virtual_key == 0 || binding.virtual_key != virtual_key)
    return false;
  uint32_t modifiers = MOD_NOREPEAT;
  if (control)
    modifiers |= MOD_CONTROL;
  if (alt)
    modifiers |= MOD_ALT;
  if (shift)
    modifiers |= MOD_SHIFT;
  return modifiers == binding.modifiers;
}

bool ToolboxPolicyAllows(std::string_view compact_json,
                         bool require_menu_enabled) noexcept {
  if (compact_json.find("\"cloudEnabled\":true") == std::string_view::npos ||
      (require_menu_enabled &&
       compact_json.find("\"selectionMenuEnabled\":false") !=
           std::string_view::npos))
    return false;
  for (const char *key : {"askAnythingSkillEnabled", "polishSkillEnabled",
                          "sourceCheckSkillEnabled", "researchAssistSkillEnabled",
                          "publishFormattingSkillEnabled", "translationSkillEnabled",
                          "promptOptimizeSkillEnabled"}) {
    if (compact_json.find("\"" + std::string(key) + "\":false") ==
        std::string_view::npos)
      return true;
  }
  return false;
}

std::vector<std::string> StatusBarParseSchemaList(std::istream &yaml) {
  std::vector<std::string> ids;
  std::string line;
  bool inside = false;
  while (std::getline(yaml, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!inside) {
      inside = TrimAscii(line) == "schema_list:";
      continue;
    }
    const std::string_view trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed.front() == '#')
      continue;
    // Anything that is not another `- schema:` entry ends the block -- the next
    // top-level key, a differently shaped list item, anything.
    if (trimmed.substr(0, 9) != "- schema:")
      break;
    const std::string_view id = Unquote(TrimAscii(trimmed.substr(9)));
    if (SafeSchemaName(id))
      ids.emplace_back(id);
  }
  return ids;
}

bool StatusBarParseSchemaName(std::istream &yaml, std::string_view id,
                              std::string *name) {
  if (!name || id.empty())
    return false;
  std::string line;
  std::string candidate;
  while (std::getline(yaml, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string_view trimmed = TrimAscii(line);
    if (trimmed.substr(0, 5) == "name:") {
      candidate = std::string(Unquote(TrimAscii(trimmed.substr(5))));
      continue;
    }
    if (trimmed.substr(0, 10) == "schema_id:") {
      // The pair is adjacent, so a schema_id reached without a name just ahead
      // of it is a different key that happens to share the prefix.
      if (Unquote(TrimAscii(trimmed.substr(10))) == id && !candidate.empty()) {
        *name = candidate;
        return true;  // stop here: these files run to tens of thousands of lines
      }
      candidate.clear();
      continue;
    }
    // Only the immediately preceding line counts as the pair's first half.
    if (!trimmed.empty())
      candidate.clear();
  }
  return false;
}

std::string StatusBarSchemaGlyph(std::string_view name) {
  // Ordered: 双拼 must win before 拼音/拼, or every double-pinyin schema reads
  // as plain pinyin; 五笔 + 拼音 must win before either alone.
  if (Contains(name, "双拼"))
    return "双";
  if (Contains(name, "五笔") && Contains(name, "拼音"))
    return "混";
  if (Contains(name, "五笔"))
    return "五";
  if (Contains(name, "九"))
    return "九";
  if (Contains(name, "拼音") || Contains(name, "拼"))
    return "拼";
  return FirstUtf8Char(name);
}

std::string StatusBarSchemaSwitchTarget(
    std::string_view current_schema, std::string_view previous_schema,
    const std::vector<std::string> &schema_list) {
  const bool previous_available =
      std::find_if(schema_list.begin(), schema_list.end(),
                   [&](const std::string &id) { return id == previous_schema; }) !=
      schema_list.end();
  if (previous_schema != current_schema && previous_available)
    return std::string(previous_schema);
  for (const std::string &id : schema_list) {
    if (id != current_schema)
      return id;
  }
  return {};
}

namespace {

bool Inside(const StatusBarLayout::Button &button, int x, int y) {
  return x >= button.left && x < button.right && y >= button.top &&
         y < button.bottom;
}

} // namespace

StatusBarLayout StatusBarLayoutFor(uint32_t dpi) {
  StatusBarLayout layout;
  const int cell = ScaleDpi(kBarCell, dpi);
  const int inset = ScaleDpi(kBarInset, dpi);
  const int schema = ScaleDpi(kSchemaCell, dpi);
  const int gap = ScaleDpi(kSchemaGap, dpi);
  layout.height = inset * 2 + cell;
  layout.width =
      inset * 2 + schema + gap + kStatusBarButtonCount * cell;
  layout.schema = {inset, inset, inset + schema, inset + cell};
  int left = layout.schema.right + gap;
  for (StatusBarLayout::Button &button : layout.buttons) {
    // Toggles abut each other: within the strip only the shared edges divide
    // them, so a click anywhere on it lands on some option. The bare trough
    // before the strip is what sets the schema segment apart.
    button = {left, inset, left + cell, inset + cell};
    left += cell;
  }
  return layout;
}

int StatusBarHitTest(const StatusBarLayout &layout, int x, int y) {
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    if (Inside(layout.buttons[index], x, y))
      return index;
  }
  return -1;
}

bool StatusBarHitsSchema(const StatusBarLayout &layout, int x, int y) {
  return Inside(layout.schema, x, y);
}

const char *StatusBarOption(int index) {
  return InRange(index) ? kToggles[index].option : nullptr;
}

const char *StatusBarSecondaryOption(int index) {
  return index == 2 ? "zh_trad" : nullptr;
}

bool StatusBarNextOptionValue(uint32_t status_flags, int index) {
  if (!InRange(index))
    return false;
  const bool on = (status_flags & kToggles[index].status_flag) != 0;
  return index == 2 ? on : !on;
}

bool StatusBarButtonOn(uint32_t status_flags, int index) {
  return InRange(index) && (status_flags & kToggles[index].status_flag) != 0;
}

const char *StatusBarLabel(uint32_t status_flags, int index) {
  if (!InRange(index))
    return nullptr;
  return StatusBarButtonOn(status_flags, index) ? kToggles[index].bar_on
                                                : kToggles[index].bar_off;
}

bool StatusBarSavePosition(const std::wstring &path, int x, int y,
                           std::string_view previous_schema) {
  if (path.empty())
    return false;
  std::ofstream file(path, std::ios::trunc);
  if (!file)
    return false;
  // Position first so a reader that only knows the old two-integer format keeps
  // working against a file written by this one.
  file << x << ' ' << y << '\n';
  if (SafeSchemaName(previous_schema))
    file << previous_schema << '\n';
  return file.good();
}

bool StatusBarLoadPosition(const std::wstring &path, int *x, int *y) {
  if (path.empty() || !x || !y)
    return false;
  std::ifstream file(path);
  int left = 0;
  int top = 0;
  if (!file || !(file >> left >> top))
    return false;
  *x = left;
  *y = top;
  return true;
}

bool StatusBarLoadPreviousSchema(const std::wstring &path,
                                 std::string *schema) {
  if (path.empty() || !schema)
    return false;
  std::ifstream file(path);
  int left = 0;
  int top = 0;
  std::string previous;
  // Absent third line is the normal case for every file written before the
  // schema segment existed, and is not a failure -- there is simply no previous
  // schema yet. Only a malformed position is.
  if (!file || !(file >> left >> top))
    return false;
  if (!(file >> previous) || !SafeSchemaName(previous))
    previous.clear();
  *schema = std::move(previous);
  return true;
}

void StatusBarClampToWorkArea(int *x, int *y, int width, int height) {
  if (!x || !y || width <= 0 || height <= 0)
    return;
  const POINT centre{*x + width / 2, *y + height / 2};
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST),
                       &info))
    return;
  if (*x + width > info.rcWork.right)
    *x = info.rcWork.right - width;
  if (*y + height > info.rcWork.bottom)
    *y = info.rcWork.bottom - height;
  // Left/top last: on a work area narrower than the bar, staying reachable
  // beats staying fully visible.
  if (*x < info.rcWork.left)
    *x = info.rcWork.left;
  if (*y < info.rcWork.top)
    *y = info.rcWork.top;
}

bool StatusTrayPathMatches(const std::wstring &stored,
                           const std::wstring &module_path) {
  const auto same = [](const std::wstring &a, const std::wstring &b) {
    // Ordinal, not locale-aware: these are filesystem paths, and the shell
    // writes drive letters in whatever case the launcher used.
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
  };
  if (stored.empty() || module_path.empty())
    return false;
  if (stored.front() != L'{')
    return same(stored, module_path);
  const size_t brace = stored.find(L'}');
  if (brace == std::wstring::npos)
    return false;
  // Compare what follows the known-folder id as a suffix instead of resolving
  // the id: the versioned directory in the middle already makes the tail
  // unique. Strictly shorter, so a rootless path cannot match everything.
  // ponytail: a stored tail equal to this module's path minus its root would
  // match falsely, which takes a deliberately built layout to hit. Resolve the
  // id through SHGetKnownFolderPath if a real collision ever turns up.
  const std::wstring tail = stored.substr(brace + 1);
  return !tail.empty() && tail.size() < module_path.size() &&
         same(module_path.substr(module_path.size() - tail.size()), tail);
}

// What the engine last reported it is typing with. Published as one immutable
// pair so the UI thread can never read a name belonging to a different id.
struct SchemaState {
  std::string id;
  std::string name;
};

struct StatusUi::State {
  RuntimeService *service = nullptr;
  std::atomic<bool> *running = nullptr;
  std::wstring data_root;  // set before the thread starts, read-only after

  // Written by the engine thread, read by the UI thread.
  std::atomic<uint32_t> status_flags{FAMO_STATUS_SIMPLIFIED};
  std::atomic<bool> focused{false};
  std::atomic<uint64_t> snapshot_revision{0};
  std::mutex publish_mutex;
  std::atomic<HWND> window{nullptr};
  std::atomic<bool> ready{false};
  std::atomic<bool> keyboard_hook_ready{false};
  std::atomic<uint32_t> keyboard_hook_error{ERROR_SUCCESS};
  std::atomic<uint64_t> icon_registrations{0};
  std::atomic<std::shared_ptr<const void>> presentation;
  std::atomic<std::shared_ptr<const SchemaState>> schema;

  // UI thread only.
  UINT taskbar_created = 0;
  HICON icon_zh = nullptr;
  HICON icon_ascii = nullptr;
  bool icon_added = false;
  int promote_attempts = 0;
  std::future<void> deploy;
  HHOOK keyboard_hook = nullptr;
  int keyboard_hook_attempts = 0;
  int injected_keyboard_hook_failures = 0;
  HHOOK mouse_hook = nullptr;
  AltDoubleTapDetector alt_double_tap;
  GlobalHotKeyBinding quick_phrase_hotkey;
  GlobalHotKeyBinding selection_toolbox_hotkey;
  DWORD swallowed_hotkey = 0;

  // Floating bar, UI thread only.
  HWND bar = nullptr;
  FamoTextResources *resources = nullptr;
  std::shared_ptr<const CandidateStylePresentation> bar_style;
  bool bar_dark = false;
  uint32_t resource_dpi = 0;
  uint32_t bar_dpi = 96;
  DibSurface bar_surface;
  std::wstring bar_state_path;
  int bar_x = 0;
  int bar_y = 0;
  bool user_moved = false;  // dragged, or restored from the state file
  int hover = -1;
  int pressed = -1;
  bool maybe_drag = false;  // button down, threshold not yet crossed
  bool dragging = false;
  POINT drag_cursor{};  // cursor at button-down, screen coords
  POINT drag_origin{};  // window top-left at button-down

  // Schema segment, UI thread only. The glyph is resolved from a file, so it is
  // cached against the id it was resolved for rather than re-read per paint.
  std::string glyph_id;
  std::string glyph;
  std::string previous_schema;  // what a single click switches back to
  std::string pending_previous; // committed only once a switch actually lands
};

namespace {

StatusUi::State *g_hook_state = nullptr;

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0 && g_hook_state) {
    const auto *key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam);
    if ((key->flags & LLKHF_INJECTED) == 0) {
      const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
      const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
      const bool alt = key->vkCode == VK_MENU || key->vkCode == VK_LMENU ||
                       key->vkCode == VK_RMENU;
      if (!g_hook_state->focused.load()) {
        g_hook_state->alt_double_tap.Reset();
        g_hook_state->swallowed_hotkey = 0;
        return CallNextHookEx(nullptr, code, wparam, lparam);
      }
      if (key->vkCode == g_hook_state->swallowed_hotkey) {
        if (up)
          g_hook_state->swallowed_hotkey = 0;
        return 1;
      }
      const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      const bool windows = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
      if (down && GlobalHotKeyBindingMatches(
                      g_hook_state->quick_phrase_hotkey, key->vkCode, control,
                      alt_down, shift, windows)) {
        g_hook_state->swallowed_hotkey = key->vkCode;
        PostMessageW(g_hook_state->window.load(), kSummonQuickPhrase, 0, 0);
        return 1;
      }
      if (down && GlobalHotKeyBindingMatches(
                      g_hook_state->selection_toolbox_hotkey, key->vkCode,
                      control, alt_down, shift, windows)) {
        g_hook_state->swallowed_hotkey = key->vkCode;
        PostMessageW(g_hook_state->window.load(), kSummonToolboxHotKey, 0, 0);
        return 1;
      }
      const bool chord = control || shift || windows;
      if (chord)
        g_hook_state->alt_double_tap.Reset();
      else if ((down || up) && g_hook_state->alt_double_tap.Process(
                                   alt, down, GetTickCount64()))
        PostMessageW(g_hook_state->window.load(), kSummonToolboxGesture, 0, 0);
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0 && g_hook_state && wparam != WM_MOUSEMOVE)
    g_hook_state->alt_double_tap.Reset();
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool ToolboxEnabled(const std::wstring &data_root, bool require_menu_enabled) {
  std::ifstream file(data_root + L"\\famo-settings.json", std::ios::binary);
  if (!file)
    return false;
  std::string json((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  json.erase(std::remove_if(json.begin(), json.end(), [](unsigned char ch) {
               return std::isspace(ch) != 0;
             }),
             json.end());
  return ToolboxPolicyAllows(json, require_menu_enabled);
}

void OpenPage(std::wstring_view page) {
  const std::wstring directory = ModuleDirectory() + L"\\settings";
  const std::wstring settings = directory + L"\\FamoSettings.exe";
  const std::wstring arguments = L"--page " + std::wstring(page);
  ShellExecuteW(nullptr, L"open", settings.c_str(), arguments.c_str(),
                directory.c_str(), SW_SHOWNORMAL);
}

void OpenToolbox(StatusUi::State *state, bool require_menu_enabled) {
  if (ToolboxEnabled(state->data_root, require_menu_enabled))
    OpenPage(L"ai-chat");
}

std::string SettingsJson(const std::wstring &data_root) {
  std::ifstream file(data_root + L"\\famo-settings.json", std::ios::binary);
  if (!file)
    return {};
  std::string json((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  json.erase(std::remove_if(json.begin(), json.end(), [](unsigned char ch) {
               return std::isspace(ch) != 0;
             }),
             json.end());
  return json;
}

std::string SettingsString(std::string_view json, std::string_view name) {
  const std::string key = "\"" + std::string(name) + "\":\"";
  const size_t begin = json.find(key);
  if (begin == std::string::npos)
    return {};
  const size_t value = begin + key.size();
  const size_t end = json.find('"', value);
  return end == std::string::npos
             ? std::string()
             : std::string(json.substr(value, end - value));
}

void RefreshHotKeys(StatusUi::State *state) {
  const std::string json = SettingsJson(state->data_root);
  const auto parse_one = [&](std::string_view name) {
    GlobalHotKeyBinding binding;
    ParseGlobalHotKeyBinding(SettingsString(json, name), &binding);
    return binding;
  };
  state->quick_phrase_hotkey = parse_one("quickPhrasePanel");
  state->selection_toolbox_hotkey = parse_one("selectionToolbox");
}

void FillIconData(NOTIFYICONDATAW *data, HWND window) {
  *data = {};
  data->cbSize = sizeof(*data);
  data->hWnd = window;
  data->uID = kTrayIconId;
}

HICON IconFor(StatusUi::State *state, uint32_t flags) {
  HICON icon =
      (flags & FAMO_STATUS_ASCII_MODE) ? state->icon_ascii : state->icon_zh;
  // A missing or unreadable .ico must still leave a clickable icon behind --
  // the menu is the only entry point the runtime has.
  return icon ? icon
              : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
}

void AddOrUpdateIcon(StatusUi::State *state, HWND window, bool add) {
  NOTIFYICONDATAW data;
  FillIconData(&data, window);
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayCallback;
  data.hIcon = IconFor(state, state->status_flags.load());
  if (state->keyboard_hook_ready.load() ||
      state->keyboard_hook_error.load() == ERROR_SUCCESS) {
    wcsncpy_s(data.szTip, kTip, _TRUNCATE);
  } else {
    swprintf_s(data.szTip, L"法墨 - 全局快捷键不可用 (错误 %lu)",
               state->keyboard_hook_error.load());
  }
  if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data))
    return;
  state->icon_added = true;
  if (add)
    state->icon_registrations.fetch_add(1);
}

bool InstallKeyboardHook(StatusUi::State *state, HWND window) {
  if (state->keyboard_hook)
    return true;
  if (state->injected_keyboard_hook_failures > 0) {
    --state->injected_keyboard_hook_failures;
    SetLastError(ERROR_ACCESS_DENIED);
  } else {
    state->keyboard_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
  }
  const DWORD error = state->keyboard_hook ? ERROR_SUCCESS : GetLastError();
  state->keyboard_hook_ready.store(state->keyboard_hook != nullptr);
  state->keyboard_hook_error.store(error);
  AddOrUpdateIcon(state, window, !state->icon_added);
  return state->keyboard_hook != nullptr;
}

void StartKeyboardHookRecovery(StatusUi::State *state, HWND window) {
  if (state->keyboard_hook)
    return;
  state->keyboard_hook_attempts = 0;
  if (InstallKeyboardHook(state, window)) {
    KillTimer(window, kKeyboardHookTimer);
    return;
  }
  SetTimer(window, kKeyboardHookTimer, kKeyboardHookInterval, nullptr);
}

// NIM_ADD on a live id fails, so a genuine re-registration has to delete first.
// This is also what makes a promotion take effect: the shell reads IsPromoted
// when the icon is added, not while it is already sitting in the tray.
void ReAddIcon(StatusUi::State *state, HWND window) {
  NOTIFYICONDATAW data;
  FillIconData(&data, window);
  Shell_NotifyIconW(NIM_DELETE, &data);
  state->icon_added = false;
  AddOrUpdateIcon(state, window, true);
}

enum class Promote {
  Pending,   // the shell has not written the entry yet -- look again
  Settled,   // a visibility is already recorded, and it is not ours to change
  Promoted,  // just written; the icon needs re-adding for it to take effect
};

// Fills in a missing visibility only. A value that is already there was chosen
// -- by the user dragging the icon out of the taskbar, or by this on an earlier
// run -- and a later demotion has to stick instead of being undone every start.
Promote PromoteTrayIcon() {
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconSettings, 0, KEY_READ,
                    &root) != ERROR_SUCCESS)
    return Promote::Settled;  // nothing to write to, and nothing to retry
  const std::wstring module_path = ModulePath();
  Promote result = Promote::Pending;
  for (DWORD index = 0;; ++index) {
    // Subkey names are a hash the shell derives from the path, so the entry is
    // found by reading ExecutablePath back rather than by computing the name.
    wchar_t name[64];
    DWORD name_size = 64;
    if (RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr,
                      nullptr) != ERROR_SUCCESS)
      break;
    wchar_t stored[MAX_PATH * 2];
    DWORD stored_size = sizeof(stored);
    if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr,
                     stored, &stored_size) != ERROR_SUCCESS)
      continue;
    if (!StatusTrayPathMatches(stored, module_path))
      continue;
    DWORD promoted = 0;
    DWORD promoted_size = sizeof(promoted);
    if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr,
                     &promoted, &promoted_size) == ERROR_SUCCESS) {
      result = Promote::Settled;
      break;
    }
    HKEY entry = nullptr;
    if (RegOpenKeyExW(root, name, 0, KEY_SET_VALUE, &entry) == ERROR_SUCCESS) {
      const DWORD show = 1;
      if (RegSetValueExW(entry, L"IsPromoted", 0, REG_DWORD,
                         reinterpret_cast<const BYTE *>(&show),
                         sizeof(show)) == ERROR_SUCCESS)
        result = Promote::Promoted;
      RegCloseKey(entry);
    }
    break;
  }
  RegCloseKey(root);
  return result;
}

void RunCommand(StatusUi::State *state, UINT command) {
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    const Toggle &toggle = kToggles[index];
    if (command != toggle.command)
      continue;
    const bool next =
        StatusBarNextOptionValue(state->status_flags.load(), index);
    state->service->SetOption(toggle.option, next);
    if (const char *secondary = StatusBarSecondaryOption(index))
      state->service->SetOption(secondary, next);
    return;
  }
  if (command == kCmdSettings) {
    const std::wstring directory = ModuleDirectory() + L"\\settings";
    const std::wstring settings = directory + L"\\FamoSettings.exe";
    ShellExecuteW(nullptr, L"open", settings.c_str(), nullptr,
                  directory.c_str(), SW_SHOWNORMAL);
    return;
  }
  if (command == kCmdDeploy) {
    // Off the UI thread: a deploy takes seconds and would leave the tray icon
    // unresponsive. The future is joined in Stop().
    state->deploy = std::async(std::launch::async, [state] {
      state->service->ExecuteControl(Command::ControlDeploy);
    });
    return;
  }
  if (command == kCmdExit)
    state->running->store(false);
}

bool DeployBusy(StatusUi::State *state) {
  return state->deploy.valid() &&
         state->deploy.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready;
}

// ─── Schema segment ──────────────────────────────────────────────────────────

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty())
    return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr,
                                         0);
  if (needed <= 0)
    return {};
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), needed);
  return wide;
}

std::shared_ptr<const SchemaState> CurrentSchema(StatusUi::State *state) {
  return state->schema.load();
}

std::vector<std::string> SchemaList(StatusUi::State *state) {
  if (state->data_root.empty())
    return {};
  std::ifstream file(state->data_root + L"\\build\\default.yaml");
  if (!file)
    return {};
  return StatusBarParseSchemaList(file);
}

// Display name for a schema, falling back to the raw id -- a schema whose
// build output is missing must still be selectable by name in the menu.
std::string SchemaDisplayName(StatusUi::State *state, const std::string &id) {
  if (state->data_root.empty() || !SafeSchemaName(id))
    return id;
  std::ifstream file(state->data_root + L"\\build\\" + Widen(id) +
                     L".schema.yaml");
  std::string name;
  if (file && StatusBarParseSchemaName(file, id, &name) && !name.empty())
    return name;
  return id;
}

// Ask the engine to switch, off the UI thread. Reuses the deploy future rather
// than adding a second: only one control operation should ever be in flight.
void SwitchSchema(StatusUi::State *state, HWND window, const std::string &id) {
  std::shared_ptr<const SchemaState> current = CurrentSchema(state);
  if (id.empty() || !SafeSchemaName(id) || DeployBusy(state) ||
      (current && current->id == id))
    return;
  state->pending_previous = current ? current->id : std::string();
  const std::wstring data_root = state->data_root;
  state->deploy = std::async(std::launch::async, [state, window, id,
                                                  data_root] {
    if (!WriteSelectedSchema(data_root, id))
      return;
    // A failed switch is left visible: the segment keeps painting whatever the
    // engine last published, so the bar never claims a schema that is not live.
    // Three ids in a stock schema_list have no compiled prism and land here.
    if (state->service->ExecuteControl(Command::ControlSelectSchema) !=
        ControlError::None)
      return;
    try {
      state->schema.store(std::make_shared<const SchemaState>(
          SchemaState{id, SchemaDisplayName(state, id)}));
    } catch (...) {
      return;
    }
    PostMessageW(window, kSchemaSwitched, 0, 0);
  });
}

// Single click: alt-tab back to the last schema. With none recorded yet, the
// first list entry that is not the current one, so a fresh profile still
// switches instead of doing nothing.
void SwitchToPreviousSchema(StatusUi::State *state, HWND window) {
  std::shared_ptr<const SchemaState> current = CurrentSchema(state);
  const std::string target = StatusBarSchemaSwitchTarget(
      current ? current->id : std::string_view(), state->previous_schema,
      SchemaList(state));
  if (!target.empty())
    SwitchSchema(state, window, target);
}

void ShowSchemaMenu(StatusUi::State *state, HWND window) {
  const std::vector<std::string> ids = SchemaList(state);
  if (ids.empty())
    return;  // nothing parsed: fall silent rather than pop an empty menu
  HMENU menu = CreatePopupMenu();
  if (!menu)
    return;
  std::shared_ptr<const SchemaState> current = CurrentSchema(state);
  const bool busy = DeployBusy(state);
  for (size_t index = 0; index < ids.size(); ++index) {
    const bool active = current && current->id == ids[index];
    AppendMenuW(menu,
                MF_STRING | (active ? MF_CHECKED : MF_UNCHECKED) |
                    (busy ? MF_GRAYED : 0),
                kCmdSchemaFirst + index,
                Widen(SchemaDisplayName(state, ids[index])).c_str());
  }
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const UINT chosen = static_cast<UINT>(TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y,
      0, window, nullptr));
  DestroyMenu(menu);
  PostMessageW(window, WM_NULL, 0, 0);
  const size_t picked = static_cast<size_t>(chosen) - kCmdSchemaFirst;
  if (chosen >= kCmdSchemaFirst && picked < ids.size())
    SwitchSchema(state, window, ids[picked]);
}

void ShowMenu(StatusUi::State *state, HWND window) {
  HMENU menu = CreatePopupMenu();
  if (!menu)
    return;
  const uint32_t flags = state->status_flags.load();
  for (const Toggle &toggle : kToggles) {
    AppendMenuW(menu,
                MF_STRING | ((flags & toggle.status_flag) ? MF_CHECKED
                                                          : MF_UNCHECKED),
                toggle.command, toggle.label);
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdSettings, L"设置…");
  AppendMenuW(menu, MF_STRING | (DeployBusy(state) ? MF_GRAYED : 0), kCmdDeploy,
              L"重新部署");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdExit, L"退出法墨");

  POINT cursor{};
  GetCursorPos(&cursor);
  // Documented tray-menu contract: without the foreground handoff and the
  // trailing post, the menu refuses to dismiss when the user clicks elsewhere.
  SetForegroundWindow(window);
  const UINT chosen = static_cast<UINT>(TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y,
      0, window, nullptr));
  DestroyMenu(menu);
  PostMessageW(window, WM_NULL, 0, 0);
  if (chosen)
    RunCommand(state, chosen);
}

// ─── Floating bar ────────────────────────────────────────────────────────────

// Used until a style arrives, and if a style fails to parse.
const FamoSkin &FallbackSkin() {
  static const FamoSkin skin = FamoSkinDefault();
  return skin;
}

// The bar's text resources are built from the skin with a compact font: the
// colors below still come from the unmodified skin, so bar and candidate window
// stay one palette.
FamoSkin BarTextSkin(const FamoSkin &skin) {
  FamoSkin text = skin;
  text.text_font.point_size = kBarFontPoint;
  return text;
}

// Bottom-right of the primary work area until the user drags it. Deliberately
// not caret-following: a bar that chases focus jumps on every window switch.
void PlaceBar(StatusUi::State *state) {
  const StatusBarLayout layout = StatusBarLayoutFor(state->bar_dpi);
  MONITORINFO info{sizeof(info)};
  if (!state->user_moved &&
      GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY),
                      &info)) {
    const int margin = ScaleDpi(kBarMargin, state->bar_dpi);
    state->bar_x = info.rcWork.right - layout.width - margin;
    state->bar_y = info.rcWork.bottom - layout.height - margin;
  }
  StatusBarClampToWorkArea(&state->bar_x, &state->bar_y, layout.width,
                           layout.height);
}

void PaintBar(StatusUi::State *state) {
  if (!state->bar)
    return;
  std::shared_ptr<const void> presentation = state->presentation.load();
  std::shared_ptr<const CandidateStylePresentation> style =
      presentation
          ? std::static_pointer_cast<const CandidateStylePresentation>(
                presentation)
          : nullptr;
  const bool dark = SystemUsesDarkPalette();
  const FamoSkin &active = style ? (dark ? style->dark : style->light)
                                 : FallbackSkin();
  const uint32_t dpi = state->bar_dpi;
  if (!state->resources || state->bar_style != style ||
      state->bar_dark != dark || state->resource_dpi != dpi) {
    const FamoSkin text_skin = BarTextSkin(active);
    if (state->resources &&
        FamoTextResourcesReconfigure(state->resources, &text_skin, dpi) !=
            FAMO_UI_OK) {
      FamoTextResourcesDestroy(state->resources);
      state->resources = nullptr;
    }
    if (!state->resources)
      state->resources = FamoTextResourcesCreate(&text_skin, dpi);
    state->bar_style = style;
    state->bar_dark = dark;
    state->resource_dpi = dpi;
  }
  // A renderer that will not start leaves the bar hidden; typing is unaffected
  // and the tray menu still reaches every option the bar would have.
  if (!state->resources)
    return;

  const StatusBarLayout layout = StatusBarLayoutFor(dpi);
  if (!state->bar_surface.Ensure(layout.width, layout.height))
    return;
  state->bar_surface.Clear();

  // Resolving the glyph reads a file, so it is cached against the id it came
  // from and only redone when the engine reports a different schema.
  std::shared_ptr<const SchemaState> schema = CurrentSchema(state);
  const std::string id = schema ? schema->id : std::string();
  if (state->glyph_id != id) {
    // Prefer the name the engine already published; only fall back to reading
    // build\<id>.schema.yaml when the snapshot carried no name.
    const std::string name = schema && !schema->name.empty()
                                 ? schema->name
                                 : SchemaDisplayName(state, id);
    state->glyph = StatusBarSchemaGlyph(name);
    state->glyph_id = id;
  }

  const uint32_t flags = state->status_flags.load();
  FamoStatusBarButton buttons[kStatusBarButtonCount + 1]{};
  buttons[0].bounds = {layout.schema.left, layout.schema.top,
                       layout.schema.right, layout.schema.bottom};
  buttons[0].label = state->glyph.c_str();
  buttons[0].hover = state->hover == kSchemaSegment ? 1u : 0u;
  buttons[0].pressed = state->pressed == kSchemaSegment ? 1u : 0u;
  for (int index = 0; index < kStatusBarButtonCount; ++index) {
    const StatusBarLayout::Button &bounds = layout.buttons[index];
    FamoStatusBarButton &button = buttons[index + 1];
    button.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    button.label = StatusBarLabel(flags, index);
    button.on = StatusBarButtonOn(flags, index) ? 1u : 0u;
    button.hover = state->hover == index ? 1u : 0u;
    button.pressed = state->pressed == index ? 1u : 0u;
  }
  FamoStatusBarSpec spec{};
  spec.size = static_cast<uint32_t>(sizeof(spec));
  spec.bar_size = {layout.width, layout.height};
  spec.dpi = dpi;
  spec.button_count = kStatusBarButtonCount + 1;
  spec.buttons = buttons;
  if (FamoStatusBarPaint(&spec, &active, state->resources,
                         state->bar_surface.dc()) != FAMO_UI_OK)
    return;
  SubmitLayered(state->bar, state->bar_surface, state->bar_x, state->bar_y);
}

// kSchemaSegment, a toggle index, or -1. Right-click routing and hover both
// need to tell the schema segment apart from the toggles.
int HitTestBar(StatusUi::State *state, LPARAM lparam) {
  const StatusBarLayout layout = StatusBarLayoutFor(state->bar_dpi);
  const int x = GET_X_LPARAM(lparam);
  const int y = GET_Y_LPARAM(lparam);
  if (StatusBarHitsSchema(layout, x, y))
    return kSchemaSegment;
  return StatusBarHitTest(layout, x, y);
}

LRESULT CALLBACK BarProc(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
  auto *state = reinterpret_cast<StatusUi::State *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (!state)
    return DefWindowProcW(window, message, wparam, lparam);

  switch (message) {
  case WM_MOUSEACTIVATE:
    // Toggling a mode must not pull focus out of the app being typed into.
    return MA_NOACTIVATE;
  case WM_LBUTTONDOWN: {
    SetCapture(window);
    GetCursorPos(&state->drag_cursor);
    RECT bounds{};
    GetWindowRect(window, &bounds);
    state->drag_origin = {bounds.left, bounds.top};
    state->maybe_drag = true;
    state->dragging = false;
    state->pressed = HitTestBar(state, lparam);
    state->hover = state->pressed;
    PaintBar(state);
    return 0;
  }
  case WM_MOUSEMOVE: {
    const int over = HitTestBar(state, lparam);
    if (over != state->hover) {
      state->hover = over;
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
      TrackMouseEvent(&track);
      PaintBar(state);
    }
    if (!state->maybe_drag || (wparam & MK_LBUTTON) == 0)
      return 0;
    POINT cursor{};
    GetCursorPos(&cursor);
    const int dx = cursor.x - state->drag_cursor.x;
    const int dy = cursor.y - state->drag_cursor.y;
    // Threshold, so a click from a less than steady hand is still a click.
    if (!state->dragging &&
        std::abs(dx) + std::abs(dy) > ScaleDpi(kBarDragThreshold,
                                               state->bar_dpi))
      state->dragging = true;
    if (state->dragging)
      SetWindowPos(window, HWND_TOPMOST, state->drag_origin.x + dx,
                   state->drag_origin.y + dy, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
    return 0;
  }
  case WM_LBUTTONUP: {
    const bool dragged = state->dragging;
    const int pressed = state->pressed;
    const int released = HitTestBar(state, lparam);
    state->pressed = -1;
    state->maybe_drag = false;
    state->dragging = false;
    if (GetCapture() == window)
      ReleaseCapture();
    if (dragged) {
      // The drag moved the window directly; read back where it landed.
      RECT bounds{};
      GetWindowRect(window, &bounds);
      state->bar_x = bounds.left;
      state->bar_y = bounds.top;
      state->user_moved = true;
      StatusBarSavePosition(state->bar_state_path, bounds.left, bounds.top,
                            state->previous_schema);
    } else if (pressed == kSchemaSegment && released == pressed) {
      SwitchToPreviousSchema(state, window);
    } else if (pressed >= 0 && released == pressed) {
      RunCommand(state, kToggles[pressed].command);
    }
    PaintBar(state);
    return 0;
  }
  case WM_MOUSELEAVE:
    if (state->hover != -1) {
      state->hover = -1;
      PaintBar(state);
    }
    return 0;
  case WM_CAPTURECHANGED:
    state->maybe_drag = false;
    state->dragging = false;
    if (state->pressed != -1) {
      state->pressed = -1;
      PaintBar(state);
    }
    return 0;
  case WM_RBUTTONUP:
    // Routed: the schema segment owns the schema list, the rest of the bar
    // keeps opening the option menu the tray shows.
    if (HitTestBar(state, lparam) == kSchemaSegment)
      ShowSchemaMenu(state, window);
    else
      ShowMenu(state, window);
    return 0;
  case kSchemaSwitched:
    // The switch landed, so the schema it replaced becomes what a single click
    // goes back to. Persisted next to the position, in the same file.
    state->previous_schema = state->pending_previous;
    state->pending_previous.clear();
    StatusBarSavePosition(state->bar_state_path, state->bar_x, state->bar_y,
                          state->previous_schema);
    PaintBar(state);
    return 0;
  case WM_DPICHANGED: {
    state->bar_dpi = HIWORD(wparam) ? HIWORD(wparam) : GetDpiForWindow(window);
    const auto *suggested = reinterpret_cast<const RECT *>(lparam);
    // Windows already computed where a moved bar belongs on the new monitor;
    // an un-moved one goes back to that monitor's corner in PlaceBar.
    if (state->user_moved && suggested) {
      state->bar_x = suggested->left;
      state->bar_y = suggested->top;
    }
    PlaceBar(state);
    PaintBar(state);
    return 0;
  }
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void CreateBar(std::shared_ptr<StatusUi::State> &state) {
  WNDCLASSW bar_class{};
  bar_class.lpfnWndProc = BarProc;
  bar_class.hInstance = GetModuleHandleW(nullptr);
  // A NULL class cursor makes Windows keep whatever shape the pointer already
  // had when it enters the window, so a transient busy cursor sticks over the
  // bar until the pointer leaves it.
  bar_class.hCursor =
      LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  bar_class.lpszClassName = kBarClassName;
  if (!RegisterClassW(&bar_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return;
  state->bar = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
      kBarClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
      GetModuleHandleW(nullptr), state.get());
  if (!state->bar)
    return;
  state->bar_dpi = GetDpiForWindow(state->bar);
  state->bar_state_path = StatusBarStatePath();
  int x = 0;
  int y = 0;
  if (StatusBarLoadPosition(state->bar_state_path, &x, &y)) {
    state->bar_x = x;
    state->bar_y = y;
    state->user_moved = true;
  }
  StatusBarLoadPreviousSchema(state->bar_state_path, &state->previous_schema);
  PlaceBar(state.get());
  PaintBar(state.get());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  auto *state = reinterpret_cast<StatusUi::State *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (!state)
    return DefWindowProcW(window, message, wparam, lparam);

  // Explorer restarts drop every tray icon. Without re-adding here the icon is
  // gone until the runtime itself restarts.
  if (state->taskbar_created && message == state->taskbar_created) {
    // The broadcast can arrive while our icon is still registered, and NIM_ADD
    // on a live id fails -- which would strand the icon at its last glyph.
    ReAddIcon(state, window);
    StartKeyboardHookRecovery(state, window);
    return 0;
  }
  switch (message) {
  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
    PaintBar(state);
    return 0;
  case WM_POWERBROADCAST:
    if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND)
      StartKeyboardHookRecovery(state, window);
    return TRUE;
  case WM_TIMER: {
    if (wparam == kKeyboardHookTimer) {
      if (InstallKeyboardHook(state, window) ||
          ++state->keyboard_hook_attempts >= kKeyboardHookAttempts)
        KillTimer(window, kKeyboardHookTimer);
      return 0;
    }
    if (wparam != kPromoteTimer)
      break;
    const Promote promote = PromoteTrayIcon();
    if (promote == Promote::Promoted)
      ReAddIcon(state, window);
    // Stop on an answer, or once the shell has plainly not recorded the icon:
    // an unbounded timer would rescan the registry for the life of the process.
    if (promote != Promote::Pending ||
        ++state->promote_attempts >= kPromoteAttempts)
      KillTimer(window, kPromoteTimer);
    return 0;
  }
  case kStatusChanged:
    // Re-adds instead of modifying if the icon is missing, so a mode change
    // recovers a registration that failed earlier.
    AddOrUpdateIcon(state, window, !state->icon_added);
    PaintBar(state);
    return 0;
  case kStyleChanged:
    RefreshHotKeys(state);
    StartKeyboardHookRecovery(state, window);
    PaintBar(state);
    return 0;
  case kSummonToolboxGesture:
    OpenToolbox(state, true);
    return 0;
  case kSummonToolboxHotKey:
    OpenToolbox(state, false);
    return 0;
  case kSummonQuickPhrase:
    OpenPage(L"quick-phrase-picker");
    return 0;
  case kTrayCallback:
    if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP)
      ShowMenu(state, window);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

StatusUi::StatusUi(RuntimeService *service, std::atomic<bool> *running,
                   std::wstring data_root)
    : state_(std::make_shared<State>()) {
  state_->service = service;
  state_->running = running;
  // Same default the runtime itself derives, so a caller that does not pass one
  // still finds the profile's build\ output.
  state_->data_root =
      data_root.empty() ? LocalFamoDirectory() : std::move(data_root);
  char *failures = nullptr;
  size_t failures_size = 0;
  if (_dupenv_s(&failures, &failures_size,
                "FAMO_TEST_KEYBOARD_HOOK_FAILURES") == 0 &&
      failures)
    state_->injected_keyboard_hook_failures =
        std::clamp(std::atoi(failures), 0, kKeyboardHookAttempts);
  std::free(failures);
}

StatusUi::~StatusUi() { Stop(); }

void StatusUi::ThreadMain(std::shared_ptr<State> state) noexcept {
  // The bar is a per-monitor DPI aware layered window, and its D2D renderer
  // plus ShellExecuteW both want an initialised apartment on this thread.
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const std::wstring icons = ModuleDirectory() + L"\\data\\";
  state->icon_zh = LoadTrayIcon(icons + L"famo_zh.ico");
  state->icon_ascii = LoadTrayIcon(icons + L"famo_ascii.ico");

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kClassName;
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    state->ready.store(true);
    if (SUCCEEDED(com))
      CoUninitialize();
    return;
  }
  // Message-only windows never receive the broadcast TaskbarCreated, so this is
  // an ordinary hidden window instead of HWND_MESSAGE.
  HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 0, 0, 0, 0,
                      nullptr, nullptr, GetModuleHandleW(nullptr), state.get());
  if (!window) {
    state->ready.store(true);
    if (SUCCEEDED(com))
      CoUninitialize();
    return;
  }
  state->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  AddOrUpdateIcon(state.get(), window, true);
  // The shell records the icon a beat after the add, so a path it has never
  // seen has no entry to fix yet and this cannot run inline here.
  SetTimer(window, kPromoteTimer, kPromoteInterval, nullptr);
  // After the tray icon: a bar that cannot be created must not cost the runtime
  // its only other entry point.
  CreateBar(state);
  state->window.store(window);
  RefreshHotKeys(state.get());
  g_hook_state = state.get();
  StartKeyboardHookRecovery(state.get(), window);
  state->mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook,
                                        GetModuleHandleW(nullptr), 0);
  state->ready.store(true);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (state->icon_added) {
    NOTIFYICONDATAW data;
    FillIconData(&data, window);
    Shell_NotifyIconW(NIM_DELETE, &data);
    state->icon_added = false;
  }
  if (state->keyboard_hook)
    UnhookWindowsHookEx(state->keyboard_hook);
  if (state->mouse_hook)
    UnhookWindowsHookEx(state->mouse_hook);
  g_hook_state = nullptr;
  state->window.store(nullptr);
  if (state->bar) {
    DestroyWindow(state->bar);
    state->bar = nullptr;
  }
  DestroyWindow(window);
  // D2D/DirectWrite and the GDI+ token belong to this thread; State outlives it.
  FamoTextResourcesDestroy(state->resources);
  state->resources = nullptr;
  state->bar_style.reset();
  if (SUCCEEDED(com))
    CoUninitialize();
}

bool StatusUi::Start() {
  if (thread_.joinable())
    return true;
  thread_ = std::thread(&StatusUi::ThreadMain, state_);
  while (!state_->ready.load())
    Sleep(1);
  return state_->window.load() != nullptr;
}

void StatusUi::Stop() {
  if (!thread_.joinable())
    return;
  if (HWND window = state_->window.load())
    PostMessageW(window, WM_QUIT, 0, 0);
  thread_.join();
  if (state_->deploy.valid())
    state_->deploy.wait();
}

uint64_t StatusUi::icon_registrations() const noexcept {
  return state_->icon_registrations.load();
}

uint32_t StatusUi::status_flags() const noexcept {
  return state_->status_flags.load();
}

bool StatusUi::keyboard_hook_ready() const noexcept {
  return state_->keyboard_hook_ready.load();
}

uint32_t StatusUi::keyboard_hook_error() const noexcept {
  return state_->keyboard_hook_error.load();
}

void StatusUi::Publish(
    std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept {
  // Revision acceptance and all fields derived from the accepted snapshot are
  // one transaction. A CAS on the revision alone still allowed an older
  // publisher paused after the CAS to overwrite a newer publisher's fields.
  std::lock_guard<std::mutex> publish_lock(state_->publish_mutex);
  // A defocused publish clears the composition wholesale, taking status_flags
  // to zero with it. Honouring that would flip the icon back to Chinese every
  // time focus leaves, so only focused snapshots carry mode here.
  if (snapshot && snapshot->revision != 0) {
    uint64_t seen = state_->snapshot_revision.load(std::memory_order_acquire);
    do {
      if (snapshot->revision <= seen)
        return;
    } while (!state_->snapshot_revision.compare_exchange_weak(
        seen, snapshot->revision, std::memory_order_acq_rel,
        std::memory_order_acquire));
  }
  state_->focused.store(snapshot && snapshot->ui_state.focused);
  if (!snapshot || !snapshot->ui_state.focused)
    return;
  const uint32_t flags = snapshot->composition.status_flags;
  bool changed = state_->status_flags.exchange(flags) != flags;
  // Same gate for the schema: a defocused publish clears schema_id too, and
  // acting on that would blank the segment on every focus change. This is the
  // authoritative source for external changes. A status-bar click also updates
  // this state, but only after the engine confirms the switch.
  const std::string &id = snapshot->composition.schema_id;
  std::shared_ptr<const SchemaState> current = state_->schema.load();
  if (!id.empty() && (!current || current->id != id ||
                      current->name != snapshot->composition.schema_name)) {
    try {
      state_->schema.store(std::make_shared<const SchemaState>(
          SchemaState{id, snapshot->composition.schema_name}));
      changed = true;
    } catch (...) {
      // Keep the last known schema rather than dropping the whole publish.
    }
  }
  if (!changed)
    return;
  if (HWND window = state_->window.load())
    PostMessageW(window, kStatusChanged, 0, 0);
}

void StatusUi::ActivateStyle(
    std::shared_ptr<const RuntimeStyleState> style) noexcept {
  if (!style)
    return;
  // Stored unconditionally: a style can be activated before Start(), and the
  // first paint then picks it up instead of flashing the fallback palette.
  state_->presentation.store(style->presentation);
  if (HWND window = state_->window.load())
    PostMessageW(window, kStyleChanged, 0, 0);
}

} // namespace famo::runtime
