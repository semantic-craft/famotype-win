// Tier A self-check: prove the host loader + FamoTestEngine roundtrip.
//
// Uses a CHECK macro (not assert): assert() is compiled out under
// NDEBUG/Release, which would make the check pass vacuously. CHECK returns exit
// code 1 on the first failure regardless of build config, so the build's
// validate step can rely on EXIT=0.
//
// UTF-8 expectations are written as explicit byte escapes so correctness does
// not depend on the compiler's source charset.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../famo_engine_api.h"
#include "../host/famo_engine_host.h"

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__,      \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

namespace {

using CreateFn = int32_t(FAMO_ENGINE_CALL *)(uint32_t, FamoEngineApi *);

const char kNi[] = "\xE4\xBD\xA0"; // 你
const char kNiSecond[] = "\xE5\xB0\xBC"; // 尼

FamoUtf8String Str(const char *s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = static_cast<uint32_t>(std::strlen(s));
  return v;
}

bool Eq(const FamoUtf8String &v, const char *s) {
  return v.data != nullptr && std::strcmp(v.data, s) == 0;
}

FamoKeyEvent KeyDown(uint32_t vk) {
  FamoKeyEvent k;
  std::memset(&k, 0, sizeof(k));
  k.size = static_cast<uint32_t>(sizeof(FamoKeyEvent));
  k.virtual_key = vk;
  k.is_key_down = 1;
  return k;
}

} // namespace

int main() {
  const wchar_t *kDll = L"FamoTestEngine.dll";

  // --- Negative ABI negotiation, straight against FamoCreateEngineApi ---
  HMODULE mod = ::LoadLibraryW(kDll);
  CHECK(mod != nullptr);
  auto create =
      reinterpret_cast<CreateFn>(::GetProcAddress(mod, "FamoCreateEngineApi"));
  CHECK(create != nullptr);

  FamoEngineApi probe;
  std::memset(&probe, 0, sizeof(probe));
  probe.size = static_cast<uint32_t>(sizeof(FamoEngineApi));
  CHECK(create(999u, &probe) == FAMO_ENGINE_E_UNSUPPORTED_ABI); // bad version

  FamoEngineApi tiny;
  std::memset(&tiny, 0, sizeof(tiny));
  tiny.size = 4; // caller struct too small to hold the v1 table
  CHECK(create(FAMO_ENGINE_ABI_VERSION, &tiny) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);

  FamoEngineApi v12;
  std::memset(&v12, 0, sizeof(v12));
  v12.size = static_cast<uint32_t>(offsetof(FamoEngineApi, peek_candidates));
  CHECK(create(FAMO_ENGINE_ABI_VERSION, &v12) == FAMO_ENGINE_OK);
  CHECK(v12.size == static_cast<uint32_t>(offsetof(FamoEngineApi, peek_candidates)));
  CHECK(v12.change_page != nullptr);
  ::FreeLibrary(mod);

  // --- Positive roundtrip through the host loader ---
  FamoEngineHost host;
  CHECK(host.Load(kDll, "") == FAMO_ENGINE_OK);

  FamoEngineInfo info;
  std::memset(&info, 0, sizeof(info));
  info.size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  CHECK(host.api().get_info(&info) == FAMO_ENGINE_OK);
  CHECK(info.abi_version == FAMO_ENGINE_ABI_VERSION);
  CHECK(info.capabilities == 0);
  CHECK(Eq(info.engine_name, "FamoTestEngine"));

  FamoEngineContext *ctx = nullptr;
  FamoUtf8String schema = Str("test");
  CHECK(host.api().create_context(&schema, &ctx) == FAMO_ENGINE_OK);
  CHECK(ctx != nullptr);

  FamoCompositionView view;

  FamoKeyEvent kn = KeyDown('n');
  CHECK(host.api().process_key(ctx, &kn, &view) == FAMO_ENGINE_OK);
  CHECK((view.state_flags & FAMO_COMPOSITION_HANDLED) != 0);
  host.FreeView(&view);

  FamoKeyEvent ki = KeyDown('i');
  CHECK(host.api().process_key(ctx, &ki, &view) == FAMO_ENGINE_OK);
  CHECK((view.state_flags & FAMO_COMPOSITION_HANDLED) != 0);
  CHECK(Eq(view.preedit, "ni"));
  CHECK(view.candidate_count > 0);
  CHECK((view.state_flags & FAMO_COMPOSITION_HAS_CANDIDATES) != 0);
  CHECK(view.candidates != nullptr);
  CHECK(Eq(view.candidates[0].text, kNi)); // 你 is the default candidate
  CHECK(host.CanPeekCandidates());
  FamoCompositionView preview;
  CHECK(host.api().peek_candidates(ctx, 1, 2, &preview) == FAMO_ENGINE_OK);
  CHECK(preview.candidate_count == 2);
  CHECK(Eq(preview.candidates[0].text, kNiSecond));
  CHECK(Eq(preview.candidates[0].label, "2"));
  host.FreeView(&preview);
  host.FreeView(&view);
  CHECK(view.preedit.data == nullptr); // free_view zeroed the view

  FamoCompositionView sel;
  CHECK(host.api().select_candidate(ctx, 0, &sel) == FAMO_ENGINE_OK);
  CHECK(Eq(sel.commit, kNi));
  CHECK((sel.state_flags & FAMO_COMPOSITION_HAS_COMMIT) != 0);
  CHECK((sel.state_flags & FAMO_COMPOSITION_HANDLED) != 0);
  CHECK((sel.state_flags & FAMO_COMPOSITION_HAS_PREEDIT) ==
        0); // composition cleared
  host.FreeView(&sel);

  FamoKeyEvent unhandled = KeyDown(0x7b); // F12
  CHECK(host.api().process_key(ctx, &unhandled, &view) == FAMO_ENGINE_OK);
  CHECK((view.state_flags & FAMO_COMPOSITION_HANDLED) == 0);
  host.FreeView(&view);

  // --- v1.1 ABI surface: size negotiation + new fields/functions ---
  CHECK(host.AbiRunnable()); // FamoTestEngine is rebuilt to v1.1

  // Re-type "ni" and inspect the v1.1 view fields (size must span
  // status_flags).
  FamoKeyEvent kn2 = KeyDown('n');
  CHECK(host.api().process_key(ctx, &kn2, &view) == FAMO_ENGINE_OK);
  host.FreeView(&view);
  FamoKeyEvent ki2 = KeyDown('i');
  CHECK(host.api().process_key(ctx, &ki2, &view) == FAMO_ENGINE_OK);
  CHECK(view.size >= sizeof(FamoCompositionView));
  CHECK(view.preedit_sel_end == 2); // deterministic: end == buffer len "ni"
  CHECK(view.preedit_cursor_pos == 2);
  CHECK(Eq(view.schema_id, "test"));
  CHECK((view.status_flags & FAMO_STATUS_COMPOSING) != 0);
  CHECK(Eq(view.commit_preview, kNi)); // preview of default candidate 你
  // v1.2 fields: label per candidate (1-based digit) + is_last_page.
  CHECK(view.candidate_count > 0);
  CHECK(Eq(view.candidates[0].label, "1")); // deterministic (i+1)%10 -> "1"
  CHECK(view.is_last_page == 1);            // deterministic single-page engine
  host.FreeView(&view);
  CHECK(view.schema_id.data == nullptr); // free_view cleared v1.1 strings too

  // get_status: same view shape, no commit consumed.
  FamoCompositionView st;
  CHECK(host.api().get_status(ctx, &st) == FAMO_ENGINE_OK);
  CHECK(Eq(st.schema_id, "test"));
  CHECK((st.state_flags & FAMO_COMPOSITION_HAS_COMMIT) == 0);
  host.FreeView(&st);

  // get_option: stub reads back off.
  FamoUtf8String opt = Str("ascii_mode");
  int32_t optval = 7;
  CHECK(host.api().get_option(ctx, &opt, &optval) == FAMO_ENGINE_OK);
  CHECK(optval == 0);

  // Thin session ops: no crash, OK return.
  CHECK(host.api().highlight_candidate(ctx, 0) == FAMO_ENGINE_OK);
  CHECK(host.api().change_page(ctx, 0) == FAMO_ENGINE_OK);
  CHECK(host.api().clear_composition(ctx) == FAMO_ENGINE_OK);
  CHECK(host.api().commit_composition(ctx) == FAMO_ENGINE_OK);

  CHECK(host.api().destroy_context(ctx) == FAMO_ENGINE_OK);
  host.Unload(); // triggers shutdown()

  std::printf("roundtrip_selfcheck: OK\n");
  return 0;
}
