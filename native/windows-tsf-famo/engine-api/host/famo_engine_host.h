// Host-side loader for a Famo engine DLL.
//
// Loads a DLL exporting FamoCreateEngineApi, negotiates the ABI version and
// struct size, supplies the host allocator/logger, and drives the engine
// lifecycle. This is pure FamoEngineApi plumbing: no librime, no Weasel, no TSF
// dependency, so it is reusable by FamoRuntime, the self-check, and any engine.
//
// Memory model: every string/array an engine returns in a FamoCompositionView is
// allocated with the host allocator this loader installs, and released by the
// engine's free_view (which also uses the host allocator). One allocator on both
// sides -> no CRT ownership crosses the DLL boundary directly.
#pragma once

#include "../famo_engine_api.h"

class FamoEngineHost {
 public:
  FamoEngineHost();
  ~FamoEngineHost();

  FamoEngineHost(const FamoEngineHost&) = delete;
  FamoEngineHost& operator=(const FamoEngineHost&) = delete;

  // Load dll_path, resolve FamoCreateEngineApi, negotiate ABI/size, verify the
  // whole v1 function table is present, then call initialize(host, data_root).
  // Returns FAMO_ENGINE_OK on success; on any failure the library is unloaded
  // and a FamoEngineResult error code is returned.
  int32_t Load(const wchar_t* dll_path, const char* data_root_utf8);

  // Calls shutdown() (if initialized) and frees the library. Idempotent.
  void Unload();

  bool loaded() const { return module_ != nullptr; }
  const FamoEngineApi& api() const { return api_; }
  const FamoEngineHostApi& host_api() const { return host_api_; }

  // True iff the loaded engine exposes the full v1.1 surface (size covers, and
  // all v1.1 pointers are non-null). The FamoRuntime reroute calls this to
  // decide "abi runnable"; a v1.0 engine returns false -> fall back to legacy.
  bool AbiRunnable() const;

  bool CanPeekCandidates() const;

  // Release a view the engine filled. Always route view teardown through here.
  int32_t FreeView(FamoCompositionView* view);

 private:
  void* module_;  // HMODULE, kept opaque to keep <windows.h> out of the header
  FamoEngineApi api_;
  FamoEngineHostApi host_api_;
  bool initialized_;
};
