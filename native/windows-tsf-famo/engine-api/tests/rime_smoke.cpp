// Tier B smoke: prove FamoRimeEngine.dll builds, links librime, loads,
// initializes against a data root, deploys, and roundtrips keys without crashing.
//
// It does NOT assert candidate correctness: with no deployed schema there may be
// no candidates. Real linguistic behavior is verified on the target machine
// (see task prd/design SS6). Needs rime.dll on the DLL search path (CMake copies
// it next to this exe).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../famo_engine_api.h"
#include "../host/famo_engine_host.h"

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__,  \
                   __LINE__);                                              \
      return 1;                                                            \
    }                                                                      \
  } while (0)

int main() {
  namespace fs = std::filesystem;

  // Minimal bundled fixture written at runtime: a default.yaml with no schemas.
  // Enough for RIME to initialize + deploy without a hand-crafted schema.
  std::error_code ec;
  fs::path data = fs::current_path() / "rime_smoke_data";
  fs::create_directories(data, ec);
  {
    std::ofstream out(data / "default.yaml");
    out << "schema_list: []\n";
  }
  const std::string root = data.string();

  FamoEngineHost host;
  int32_t rc = host.Load(L"FamoRimeEngine.dll", root.c_str());
  if (rc != FAMO_ENGINE_OK) {
    std::fprintf(stderr,
                 "Load FamoRimeEngine.dll failed rc=0x%X (is rime.dll on the "
                 "DLL search path?)\n",
                 static_cast<unsigned>(rc));
    return 1;
  }

  FamoEngineInfo info;
  std::memset(&info, 0, sizeof(info));
  info.size = static_cast<uint32_t>(sizeof(info));
  CHECK(host.api().get_info(&info) == FAMO_ENGINE_OK);
  CHECK(info.engine_name.data && std::strcmp(info.engine_name.data, "FamoRimeEngine") == 0);

  FamoUtf8String empty;
  empty.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  empty.data = "";
  empty.length_bytes = 0;

  FamoUtf8String derr;
  std::memset(&derr, 0, sizeof(derr));
  derr.size = static_cast<uint32_t>(sizeof(derr));
  CHECK(host.api().deploy_schema(&empty, &derr) == FAMO_ENGINE_OK);

  // v1.1: the rebuilt engine must expose the full v1.1 surface.
  CHECK(host.AbiRunnable());

  FamoEngineContext* ctx = nullptr;
  CHECK(host.api().create_context(&empty, &ctx) == FAMO_ENGINE_OK);
  CHECK(ctx != nullptr);

  // Feed a few letter keydowns; with no schema this yields empty composition -
  // we only require no crash, OK return, and a view that spans the v1.1 fields.
  const char* seq = "nihao";
  for (const char* p = seq; *p; ++p) {
    FamoKeyEvent k;
    std::memset(&k, 0, sizeof(k));
    k.size = static_cast<uint32_t>(sizeof(k));
    // rime keysym pass-through (engine no longer VK-maps): ascii 'a'-'z' == X11 keysym.
    k.virtual_key = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    k.is_key_down = 1;
    FamoCompositionView v;
    CHECK(host.api().process_key(ctx, &k, &v) == FAMO_ENGINE_OK);
    CHECK(v.size >= sizeof(FamoCompositionView));  // v1.2 fields are addressable
    // v1.2: is_last_page is a clean boolean; any candidate's label is
    // well-formed (with an empty schema there may be no candidates).
    CHECK(v.is_last_page == 0 || v.is_last_page == 1);
    for (uint32_t i = 0; i < v.candidate_count; ++i) {
      CHECK(v.candidates[i].label.data == nullptr ||
            v.candidates[i].label.length_bytes ==
                std::strlen(v.candidates[i].label.data));
    }
    host.FreeView(&v);
  }

  // get_status: OK, status_flags readable, schema strings non-garbage (with an
  // empty schema they may be empty, but the FamoUtf8String must be well-formed).
  FamoCompositionView st;
  CHECK(host.api().get_status(ctx, &st) == FAMO_ENGINE_OK);
  CHECK(st.size >= sizeof(FamoCompositionView));
  const uint32_t known = FAMO_STATUS_ASCII_MODE | FAMO_STATUS_COMPOSING |
                         FAMO_STATUS_DISABLED | FAMO_STATUS_FULL_SHAPE |
                         FAMO_STATUS_ASCII_PUNCT | FAMO_STATUS_SIMPLIFIED;
  CHECK((st.status_flags & ~known) == 0);  // no stray bits
  CHECK(st.schema_id.data == nullptr ||
        st.schema_id.length_bytes == std::strlen(st.schema_id.data));  // well-formed
  host.FreeView(&st);

  // get_option: OK, boolean read.
  FamoUtf8String opt;
  opt.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  opt.data = "ascii_mode";
  opt.length_bytes = static_cast<uint32_t>(std::strlen(opt.data));
  int32_t optval = -1;
  CHECK(host.api().get_option(ctx, &opt, &optval) == FAMO_ENGINE_OK);
  CHECK(optval == 0 || optval == 1);

  // Thin session ops: no crash, OK return.
  CHECK(host.api().highlight_candidate(ctx, 0) == FAMO_ENGINE_OK);
  CHECK(host.api().change_page(ctx, 0) == FAMO_ENGINE_OK);
  CHECK(host.api().clear_composition(ctx) == FAMO_ENGINE_OK);
  CHECK(host.api().commit_composition(ctx) == FAMO_ENGINE_OK);

  CHECK(host.api().destroy_context(ctx) == FAMO_ENGINE_OK);
  host.Unload();

  std::printf("rime_smoke: OK (build+link+load+init+deploy+roundtrip+v1.1, no crash)\n");
  return 0;
}
