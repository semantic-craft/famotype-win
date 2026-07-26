#include "famo_engine_host.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The single host allocator. Engines allocate view memory through these and
// release it through free_view, so all view memory lives in the host CRT.
void* FAMO_ENGINE_CALL HostAlloc(size_t bytes) { return std::malloc(bytes); }
void FAMO_ENGINE_CALL HostFree(void* p) { std::free(p); }
void FAMO_ENGINE_CALL HostLog(int32_t level, const FamoUtf8String* domain,
                              const FamoUtf8String* message) {
  (void)domain;
  (void)message;
  // Engine messages are untrusted for privacy: never print composition text.
  std::fprintf(stderr, "[famo-engine][%d] event\n", level);
}

using CreateFn = int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApi*);

// Baseline v1.0 table: the ten pointers every engine (v1.0 or v1.1) must supply.
bool TableComplete(const FamoEngineApi& a) {
  return a.get_info && a.initialize && a.shutdown && a.create_context &&
         a.destroy_context && a.process_key && a.select_candidate &&
         a.set_option && a.deploy_schema && a.free_view;
}

// v1.0 struct span: an engine whose size reaches here and fills the ten baseline
// pointers is loadable. The six v1.1 pointers live beyond this offset.
const uint32_t kV1BaselineSize =
    static_cast<uint32_t>(offsetof(FamoEngineApi, get_status));
const uint32_t kV11Size =
    static_cast<uint32_t>(offsetof(FamoEngineApi, peek_candidates));

}  // namespace

FamoEngineHost::FamoEngineHost() : module_(nullptr), initialized_(false) {
  std::memset(&api_, 0, sizeof(api_));
  std::memset(&host_api_, 0, sizeof(host_api_));
}

FamoEngineHost::~FamoEngineHost() { Unload(); }

int32_t FamoEngineHost::Load(const wchar_t* dll_path, const char* data_root_utf8) {
  Unload();

  HMODULE mod = ::LoadLibraryW(dll_path);
  if (!mod) return FAMO_ENGINE_E_RUNTIME;

  auto create = reinterpret_cast<CreateFn>(::GetProcAddress(mod, "FamoCreateEngineApi"));
  if (!create) {
    ::FreeLibrary(mod);
    return FAMO_ENGINE_E_RUNTIME;
  }

  std::memset(&api_, 0, sizeof(api_));
  api_.size = static_cast<uint32_t>(sizeof(FamoEngineApi));
  int32_t rc = create(FAMO_ENGINE_ABI_VERSION, &api_);
  if (rc != FAMO_ENGINE_OK) {
    ::FreeLibrary(mod);
    std::memset(&api_, 0, sizeof(api_));
    return rc;
  }

  // Negotiation guards: version match, enough struct to hold the v1.0 baseline,
  // full baseline table. A v1.0 engine (smaller size, no v1.1 pointers) still
  // loads; abi_runnable() below reports whether the v1.1 surface is usable.
  if (api_.abi_version != FAMO_ENGINE_ABI_VERSION || api_.size < kV1BaselineSize ||
      !TableComplete(api_)) {
    ::FreeLibrary(mod);
    std::memset(&api_, 0, sizeof(api_));
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  }

  host_api_.size = static_cast<uint32_t>(sizeof(FamoEngineHostApi));
  host_api_.abi_version = FAMO_ENGINE_ABI_VERSION;
  host_api_.alloc = &HostAlloc;
  host_api_.free = &HostFree;
  host_api_.log = &HostLog;

  FamoUtf8String root;
  root.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  root.data = data_root_utf8 ? data_root_utf8 : "";
  root.length_bytes = static_cast<uint32_t>(std::strlen(root.data));

  rc = api_.initialize(&host_api_, &root);
  if (rc != FAMO_ENGINE_OK) {
    ::FreeLibrary(mod);
    std::memset(&api_, 0, sizeof(api_));
    std::memset(&host_api_, 0, sizeof(host_api_));
    return rc;
  }

  module_ = static_cast<void*>(mod);
  initialized_ = true;
  return FAMO_ENGINE_OK;
}

void FamoEngineHost::Unload() {
  if (module_) {
    if (initialized_ && api_.shutdown) api_.shutdown();
    ::FreeLibrary(static_cast<HMODULE>(module_));
  }
  module_ = nullptr;
  initialized_ = false;
  std::memset(&api_, 0, sizeof(api_));
  std::memset(&host_api_, 0, sizeof(host_api_));
}

int32_t FamoEngineHost::FreeView(FamoCompositionView* view) {
  if (!module_ || !api_.free_view || !view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_.free_view(view);
}

bool FamoEngineHost::AbiRunnable() const {
  // The runtime reroute (get_status/get_option + the four thin session ops) is
  // usable only if the engine's table both spans the v1.1 pointers and fills
  // them. A v1.0 engine fails one or the other -> caller falls back to legacy.
  return module_ &&
         api_.size >= kV11Size &&
         api_.get_status && api_.get_option && api_.commit_composition &&
         api_.clear_composition && api_.highlight_candidate && api_.change_page;
}

bool FamoEngineHost::CanPeekCandidates() const {
  return module_ &&
         api_.size >= static_cast<uint32_t>(sizeof(FamoEngineApi)) &&
         api_.peek_candidates;
}
