// Tier B real-candidate verification (manual tool, not a ctest).
//
// Unlike rime_smoke (empty schema -> no-crash only), this points FamoRimeEngine
// at a REAL deployed rime data root and checks that typing produces genuine
// Chinese candidates. It is argv-driven and needs deployed schemas present, so
// it is a manual verification tool, not part of the automated suite.
//
//   rime_verify <data_root> <schema_id> <ascii_keys> [--deploy]
//   e.g. rime_verify C:\temp\famo-data rime_ice nihao   -> expect 你好 ...
//
// Exit 0 iff the final composition has >=1 candidate containing multi-byte
// UTF-8 (i.e. real CJK), so the check is meaningful even when piped. Needs
// rime.dll on the DLL search path (CMake copies it beside this exe).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../famo_engine_api.h"
#include "../host/famo_engine_host.h"

namespace {

FamoUtf8String Str(const std::string& s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s.c_str();
  v.length_bytes = static_cast<uint32_t>(s.size());
  return v;
}

bool HasMultibyte(const char* s) {
  if (!s) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
    if (*p >= 0x80) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  ::SetConsoleOutputCP(CP_UTF8);
  if (argc < 4 || argc > 5 ||
      (argc == 5 && std::strcmp(argv[4], "--deploy") != 0)) {
    std::fprintf(
        stderr,
        "usage: rime_verify <data_root> <schema_id> <ascii_keys> [--deploy]\n");
    return 2;
  }
  const std::string data_root = argv[1];
  const std::string schema_id = argv[2];
  const std::string keys = argv[3];

  FamoEngineHost host;
  int32_t rc = host.Load(L"FamoRimeEngine.dll", data_root.c_str());
  if (rc != FAMO_ENGINE_OK) {
    std::fprintf(stderr, "Load FamoRimeEngine.dll failed rc=0x%X\n", static_cast<unsigned>(rc));
    return 1;
  }

  FamoUtf8String schema = Str(schema_id);
  if (argc == 5) {
    FamoUtf8String deploy_error{};
    deploy_error.size = static_cast<uint32_t>(sizeof(deploy_error));
    if (host.api().deploy_schema(&schema, &deploy_error) != FAMO_ENGINE_OK) {
      std::fprintf(stderr, "deploy failed\n");
      return 1;
    }
  }
  FamoEngineContext* ctx = nullptr;
  if (host.api().create_context(&schema, &ctx) != FAMO_ENGINE_OK || !ctx) {
    std::fprintf(stderr, "create_context(%s) failed\n", schema_id.c_str());
    return 1;
  }

  FamoCompositionView view;
  std::memset(&view, 0, sizeof(view));
  for (char c : keys) {
    FamoKeyEvent k;
    std::memset(&k, 0, sizeof(k));
    k.size = static_cast<uint32_t>(sizeof(k));
    // rime keysym pass-through (engine no longer VK-maps): ascii char == X11 keysym.
    k.virtual_key = static_cast<uint32_t>(static_cast<unsigned char>(c));
    k.is_key_down = 1;
    host.FreeView(&view);
    host.api().process_key(ctx, &k, &view);
  }

  std::printf("schema=%s  keys=%s\n", schema_id.c_str(), keys.c_str());
  std::printf("preedit: %s\n", view.preedit.data ? view.preedit.data : "");
  std::printf("candidates (%u):\n", view.candidate_count);
  bool real = false;
  for (uint32_t i = 0; i < view.candidate_count; ++i) {
    const char* t = view.candidates[i].text.data;
    std::printf("  [%u] %s\n", i, t ? t : "");
    if (HasMultibyte(t)) real = true;
  }

  host.FreeView(&view);
  host.api().destroy_context(ctx);
  host.Unload();

  if (!real) {
    std::fprintf(stderr, "FAIL: no real (multi-byte) candidate produced\n");
    return 1;
  }
  std::printf("rime_verify: OK (real candidates produced)\n");
  return 0;
}
