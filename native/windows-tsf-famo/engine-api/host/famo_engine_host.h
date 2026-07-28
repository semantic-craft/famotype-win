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

class FamoEngineActionResultLease {
 public:
  // A live lease calls into its source DLL when reset or destroyed. It must be
  // reset before the FamoEngineHost that created it is unloaded or destroyed.
  FamoEngineActionResultLease() = default;
  ~FamoEngineActionResultLease();
  FamoEngineActionResultLease(const FamoEngineActionResultLease&) = delete;
  FamoEngineActionResultLease& operator=(
      const FamoEngineActionResultLease&) = delete;
  FamoEngineActionResultLease(FamoEngineActionResultLease&& other) noexcept;
  FamoEngineActionResultLease& operator=(
      FamoEngineActionResultLease&& other) noexcept;

  const FamoEngineActionResultV2* get() const { return result_; }
  const FamoEngineActionResultV2& operator*() const { return *result_; }
  const FamoEngineActionResultV2* operator->() const { return result_; }
  explicit operator bool() const { return result_ != nullptr; }
  int32_t Reset();

 private:
  friend class FamoEngineHost;
  using FreeFn = int32_t(FAMO_ENGINE_CALL*)(FamoEngineActionResultV2*);
  void Adopt(FamoEngineActionResultV2* result, FreeFn free_result);

  FamoEngineActionResultV2* result_ = nullptr;
  FreeFn free_result_ = nullptr;
};

struct FamoEngineRecoveryOutcome {
  bool business_dispatched = false;
  bool recovery_pending = false;
  bool handled = false;
  uint32_t original_action = 0;
};

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

  // Explicit ABI v2 load. This never silently falls back to v1; a caller that
  // wants the compatibility adapter must make a separate Load() decision.
  // The notification callback is installed before initialize so deployment
  // events are not lost.
  int32_t LoadV2(
      const wchar_t* dll_path,
      const char* data_root_utf8,
      FamoEngineNotificationHandlerV2 notification_handler = nullptr,
      void* notification_user_data = nullptr);

  // Calls shutdown() (if initialized) and frees the library. Idempotent.
  void Unload();

  bool loaded() const { return module_ != nullptr; }
  const FamoEngineApi& api() const { return api_; }
  const FamoEngineApiV2& v2_api() const { return api_v2_; }
  const FamoEngineHostApi& host_api() const { return host_api_; }

  // Legacy-only compatibility query for old v1 host canaries.
  bool AbiRunnable() const;

  bool CanPeekCandidates() const;
  bool CanSelectCandidateAbsolute() const;

  bool V2Runnable() const;

  // ABI v2 deep seam. Product callers use these methods; they never reach into
  // either function table or infer a v1 fallback.
  static FamoEngineActionRequestV2 Action(uint32_t action);
  // Deterministic allocator seam for engine fault tests. -1 disables failure,
  // 0 fails the next/allocation, and N allows N allocations before failing.
  static void SetAllocationFailureCountdownForTesting(
      int64_t countdown) noexcept;
  // Validate the frozen v2 result/view/candidate layouts and all reachable
  // UTF-8 fields before a product consumer translates them. Limits are
  // consumer-specific (for example, an IPC frame budget).
  static bool ValidateResultV2(const FamoEngineActionResultV2* result,
                               uint32_t expected_action,
                               uint32_t max_candidates,
                               uint32_t max_string_bytes) noexcept;
  int32_t CreateContext(const FamoUtf8String* schema_id,
                        FamoEngineContext** out_context);
  int32_t DestroyContext(FamoEngineContext* context);
  int32_t ExecuteAction(FamoEngineContext* context,
                        const FamoEngineActionRequestV2* request,
                        FamoEngineActionResultLease* out_result);
  // Executes a business action once. A RESYNC_REQUIRED receipt is never
  // exposed as a final view: the host issues only RECOVER control actions,
  // bounded by max_recovery_attempts. On exhaustion, outcome retains the sole
  // handled truth so keyboard consumers can swallow an already-dispatched key.
  int32_t ExecuteActionRecovering(
      FamoEngineContext* context,
      const FamoEngineActionRequestV2* request,
      uint32_t max_recovery_attempts,
      FamoEngineActionResultLease* out_result,
      FamoEngineRecoveryOutcome* outcome);
  int32_t SetOption(FamoEngineContext* context,
                    const FamoUtf8String* name, int32_t value);
  int32_t GetOption(FamoEngineContext* context,
                    const FamoUtf8String* name, int32_t* out_value);
  int32_t SetProperty(FamoEngineContext* context,
                      const FamoUtf8String* name,
                      const FamoUtf8String* value);
  int32_t DeploySchema(const FamoUtf8String* schema_id);

  // Release a view the engine filled. Always route view teardown through here.
  int32_t FreeView(FamoCompositionView* view);
  int32_t FreeResultV2(FamoEngineActionResultV2* result);

 private:
  static void FAMO_ENGINE_CALL NotificationThunk(
      void* user_data,
      FamoEngineContext* context,
      const FamoUtf8String* message_type,
      const FamoUtf8String* message_value,
      const FamoUtf8String* state_label) noexcept;

  void* module_;  // HMODULE, kept opaque to keep <windows.h> out of the header
  FamoEngineApi api_;
  FamoEngineApiV2 api_v2_;
  FamoEngineHostApi host_api_;
  bool initialized_;
  uint32_t active_abi_version_;
  FamoEngineNotificationHandlerV2 notification_handler_;
  void* notification_user_data_;
};
