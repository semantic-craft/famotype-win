#pragma once

#include <atomic>
#include <iosfwd>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "famo_runtime_service.h"

namespace famo::runtime {

// ─── Floating status bar geometry ────────────────────────────────────────────
// Kept as pure functions of DPI so hit-testing, button order and the option
// mapping are all assertable without creating a window or faking mouse input.

inline constexpr int kStatusBarButtonCount = 4;

struct StatusBarLayout {
  int width = 0;
  int height = 0;
  // Bar-local device px, left to right in draw order.
  struct Button {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };
  // Which schema is being typed with, not a mode of it. Held apart from the
  // toggle strip by a gap in the trough and given its own rounding so it does
  // not read as a fifth toggle. Kept out of `buttons` so every existing
  // index-to-option mapping stays exactly what it was.
  Button schema;
  Button buttons[kStatusBarButtonCount];
};

StatusBarLayout StatusBarLayoutFor(uint32_t dpi);
// Index of the TOGGLE containing a bar-local point, or -1 for the padding, the
// gap and the schema segment -- so a caller that only knows about the four
// options can never mistake the schema segment for one of them.
int StatusBarHitTest(const StatusBarLayout &layout, int x, int y);
// The other half of the routing decision: right-click here opens the schema
// list, anywhere else on the bar opens the option menu.
bool StatusBarHitsSchema(const StatusBarLayout &layout, int x, int y);
// Engine option a button toggles; nullptr out of range.
const char *StatusBarOption(int index);
// Whether a button draws enabled for these status flags.
bool StatusBarButtonOn(uint32_t status_flags, int index);
// The label a button draws: the state the option is currently IN (中 vs 英),
// not the action a click performs, so the bar reads as a status display.
const char *StatusBarLabel(uint32_t status_flags, int index);

// ─── Schema segment ──────────────────────────────────────────────────────────
// The schema list and the display names come from the files rime regenerates
// under <data root>\build on every deploy, not from famo-settings.json -- the
// runtime has no JSON parser and this is not worth adding one for. Both take a
// stream and stop as early as they can: a .schema.yaml runs to tens of
// thousands of lines and a right-click would otherwise read a dozen of them.

// Ids listed under `schema_list:` in build\default.yaml, in order. Entries
// failing the engine's own name rule are dropped rather than failing the parse,
// so one bad line cannot cost the user the whole menu.
std::vector<std::string> StatusBarParseSchemaList(std::istream &yaml);
// The `name:` adjacent to `schema_id: <id>` in build\<id>.schema.yaml.
bool StatusBarParseSchemaName(std::istream &yaml, std::string_view id,
                             std::string *name);
// One glyph for the segment. NOT the name's first character -- that yields 雾
// for 雾凇拼音 and 极 for 极点五笔86, neither of which says anything about the
// input method. Ordered keyword match, first hit wins, first character only as
// a last resort.
std::string StatusBarSchemaGlyph(std::string_view name);

// ─── Floating status bar position persistence ────────────────────────────────
// The bar's screen top-left, then the schema a single click on the segment
// switches back to. The path is a parameter rather than baked in so a round
// trip is testable without touching the real profile.

// A file written before the schema segment existed holds only `x y`. It must
// keep loading, with an empty previous schema -- so the position is never
// invalidated by the newer field being absent.
bool StatusBarSavePosition(const std::wstring &path, int x, int y,
                           std::string_view previous_schema = {});
bool StatusBarLoadPosition(const std::wstring &path, int *x, int *y);
bool StatusBarLoadPreviousSchema(const std::wstring &path, std::string *schema);
// Pull a top-left back onto the nearest monitor's work area. A persisted
// position outlives the display it was saved on, so a restore without this
// leaves the bar stranded off-screen with no way to drag it back.
void StatusBarClampToWorkArea(int *x, int *y, int width, int height);

// ─── Tray icon promotion ─────────────────────────────────────────────────────
// Windows 11 keys notification-icon visibility off the executable path, under
// HKCU\Control Panel\NotifyIconSettings, and defaults a path it has never seen
// to the overflow flyout. The versioned install layout hands it a new path on
// every update, so the icon hides itself again after each one.
//
// The shell stores those paths known-folder relative -- literally
// "{6D809377-...}\Famo\versions\...\FamoRuntime.exe" -- so the entry has to be
// matched against the running module rather than string-compared. Pure so the
// encoding is assertable without touching the real profile.
bool StatusTrayPathMatches(const std::wstring &stored,
                           const std::wstring &module_path);

// Tray icon and its option menu. Runs its own thread on purpose: TrackPopupMenu
// is a blocking modal loop, and a deploy triggered from the menu can take
// seconds, either of which would blow the candidate window's render budget if
// this shared that thread.
//
// Consumes snapshots only for Composition::status_flags, so it can show the
// current mode without asking the engine on every paint.
//
// Also owns the floating status bar: a layered toggle bar mirroring the four
// menu options. It shares this thread rather than the candidate window's, which
// has a 50 ms render budget and must never sit inside a blocking menu loop.
class StatusUi final : public RuntimeSnapshotSink {
public:
  // data_root is where famo-select-schema.txt is written and build\ is read
  // from. Defaulted so the tests, which have no data root, still construct it.
  StatusUi(RuntimeService *service, std::atomic<bool> *running,
           std::wstring data_root = {});
  ~StatusUi() override;
  StatusUi(const StatusUi &) = delete;
  StatusUi &operator=(const StatusUi &) = delete;

  bool Start();
  void Stop();
  void
  Publish(std::shared_ptr<const RuntimeSnapshot> snapshot) noexcept override;
  // The bar paints from the same FamoSkin as the candidate window, so it tracks
  // color-scheme changes instead of carrying a second palette.
  void ActivateStyle(
      std::shared_ptr<const RuntimeStyleState> style) noexcept override;

  // Successful NIM_ADD count. Rises again on every TaskbarCreated, which is the
  // only evidence that an explorer restart does not strand the icon for good.
  uint64_t icon_registrations() const noexcept;

  // Latest mode the icon and menu check marks are drawn from.
  uint32_t status_flags() const noexcept;

  // Opaque, defined in status_ui.cpp. Public only so the window procedure and
  // menu helpers living in that file's anonymous namespace can name it.
  struct State;

private:
  static void ThreadMain(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
  std::thread thread_;
};

} // namespace famo::runtime
