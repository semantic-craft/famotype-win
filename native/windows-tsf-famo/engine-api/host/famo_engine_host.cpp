#include "famo_engine_host.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace {

// The single host allocator. Besides keeping CRT ownership on the host side,
// the live registry is the capability used by the v2 validator: no pointer
// supplied by a DLL is dereferenced until its complete span is proven to be in
// the exact live allocation whose base is the result pointer.
std::mutex g_host_allocations_mutex;
std::unordered_map<void*, size_t> g_host_allocations;
std::atomic<int64_t> g_allocation_failure_countdown{-1};

bool ShouldFailHostAllocation() noexcept {
  int64_t remaining =
      g_allocation_failure_countdown.load(std::memory_order_relaxed);
  while (remaining >= 0) {
    if (remaining == 0)
      return true;
    if (g_allocation_failure_countdown.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return false;
    }
  }
  return false;
}

void* FAMO_ENGINE_CALL HostAlloc(size_t bytes) {
  if (ShouldFailHostAllocation())
    return nullptr;
  void* allocation = std::malloc(bytes);
  if (!allocation)
    return nullptr;
  try {
    std::lock_guard<std::mutex> lock(g_host_allocations_mutex);
    if (!g_host_allocations.emplace(allocation, bytes).second) {
      std::free(allocation);
      return nullptr;
    }
  } catch (...) {
    std::free(allocation);
    return nullptr;
  }
  return allocation;
}

void FAMO_ENGINE_CALL HostFree(void* p) {
  if (!p)
    return;
  bool owned = false;
  try {
    std::lock_guard<std::mutex> lock(g_host_allocations_mutex);
    const auto found = g_host_allocations.find(p);
    if (found != g_host_allocations.end()) {
      g_host_allocations.erase(found);
      owned = true;
    }
  } catch (...) {
    return;
  }
  if (owned)
    std::free(p);
}
void FAMO_ENGINE_CALL HostLog(int32_t level, const FamoUtf8String* domain,
                              const FamoUtf8String* message) {
  (void)domain;
  (void)message;
  // Engine messages are untrusted for privacy: never print composition text.
  std::fprintf(stderr, "[famo-engine][%d] event\n", level);
}

using CreateFn = int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApi*);
using CreateV2Fn = int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApiV2*);

// Baseline v1.0 table: the ten pointers every engine (v1.0 or v1.1) must supply.
bool TableComplete(const FamoEngineApi& a) {
  return a.get_info && a.initialize && a.shutdown && a.create_context &&
         a.destroy_context && a.process_key && a.select_candidate &&
         a.set_option && a.deploy_schema && a.free_view;
}

bool TableCompleteV2(const FamoEngineApiV2& a) {
  return a.get_info && a.initialize && a.shutdown && a.create_context &&
         a.destroy_context && a.execute_action && a.set_option &&
         a.get_option && a.deploy_schema && a.free_result &&
         a.set_property && a.set_notification_handler;
}

bool ValidUtf8Bytes(const char* data, uint32_t length) noexcept {
  if (length != 0 && !data)
    return false;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  uint32_t index = 0;
  while (index < length) {
    const unsigned char first = bytes[index++];
    if (first <= 0x7f)
      continue;
    uint32_t trailing = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;
    if (first >= 0xc2 && first <= 0xdf) {
      trailing = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      trailing = 2;
      if (first == 0xe0)
        second_min = 0xa0;
      else if (first == 0xed)
        second_max = 0x9f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      trailing = 3;
      if (first == 0xf0)
        second_min = 0x90;
      else if (first == 0xf4)
        second_max = 0x8f;
    } else {
      return false;
    }
    if (trailing > length - index)
      return false;
    const unsigned char second = bytes[index++];
    if (second < second_min || second > second_max)
      return false;
    for (uint32_t offset = 1; offset < trailing; ++offset) {
      const unsigned char continuation = bytes[index++];
      if (continuation < 0x80 || continuation > 0xbf)
        return false;
    }
  }
  return true;
}

bool ValidUtf8String(const FamoUtf8String& value,
                     uint32_t max_string_bytes) noexcept {
  return value.size >= FAMO_UTF8_STRING_REQUIRED_SIZE &&
         value.length_bytes <= max_string_bytes &&
         (value.length_bytes == 0 || value.data != nullptr) &&
         (value.length_bytes == 0 ||
          std::memchr(value.data, '\0', value.length_bytes) == nullptr) &&
         ValidUtf8Bytes(value.data, value.length_bytes);
}

bool AllocationContains(const void* allocation,
                        size_t allocation_size,
                        const void* pointer,
                        size_t span) noexcept {
  if (!pointer)
    return span == 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(allocation);
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  if (address < base)
    return false;
  const uintptr_t offset = address - base;
  return offset <= allocation_size &&
         span <= allocation_size - static_cast<size_t>(offset);
}

bool ValidResultEnvelope(const FamoEngineActionResultV2* result,
                         uint32_t expected_action) noexcept {
  if (!result)
    return false;
  std::unique_lock<std::mutex> lock(
      g_host_allocations_mutex, std::defer_lock);
  try {
    lock.lock();
  } catch (...) {
    return false;
  }
  const auto allocation =
      g_host_allocations.find(const_cast<FamoEngineActionResultV2*>(result));
  return allocation != g_host_allocations.end() &&
         allocation->second >= FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE &&
         allocation->second <= FAMO_ENGINE_V2_MAX_RESULT_BYTES &&
         result->struct_size >= FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE &&
         result->action == expected_action &&
         (result->result_flags & ~FAMO_ENGINE_RESULT_RESYNC_REQUIRED) == 0 &&
         result->handled <= 1;
}

bool ValidResultString(const FamoUtf8String& value,
                       uint32_t max_string_bytes,
                       const void* allocation,
                       size_t allocation_size) noexcept {
  return value.size >= FAMO_UTF8_STRING_REQUIRED_SIZE &&
         value.length_bytes <= max_string_bytes &&
         (value.length_bytes == 0 || value.data != nullptr) &&
         AllocationContains(allocation, allocation_size, value.data,
                            value.length_bytes) &&
         (value.length_bytes == 0 ||
          std::memchr(value.data, '\0', value.length_bytes) == nullptr) &&
         ValidUtf8Bytes(value.data, value.length_bytes);
}

bool EqualsAscii(const FamoUtf8String& value,
                 const char* expected) noexcept {
  const size_t expected_length = std::strlen(expected);
  return value.length_bytes == expected_length &&
         (expected_length == 0 ||
          std::memcmp(value.data, expected, expected_length) == 0);
}

bool IsUtf8Boundary(const FamoUtf8String& value, uint32_t offset) noexcept {
  if (offset > value.length_bytes)
    return false;
  return offset == value.length_bytes ||
         (static_cast<unsigned char>(value.data[offset]) & 0xc0u) != 0x80u;
}

// v1.0 struct span: an engine whose size reaches here and fills the ten baseline
// pointers is loadable. The six v1.1 pointers live beyond this offset.
const uint32_t kV1BaselineSize =
    static_cast<uint32_t>(offsetof(FamoEngineApi, get_status));
const uint32_t kV11Size =
    static_cast<uint32_t>(offsetof(FamoEngineApi, peek_candidates));

}  // namespace

FamoEngineActionResultLease::~FamoEngineActionResultLease() { (void)Reset(); }

FamoEngineActionResultLease::FamoEngineActionResultLease(
    FamoEngineActionResultLease&& other) noexcept
    : result_(other.result_), free_result_(other.free_result_) {
  other.result_ = nullptr;
  other.free_result_ = nullptr;
}

FamoEngineActionResultLease& FamoEngineActionResultLease::operator=(
    FamoEngineActionResultLease&& other) noexcept {
  if (this != &other) {
    (void)Reset();
    result_ = other.result_;
    free_result_ = other.free_result_;
    other.result_ = nullptr;
    other.free_result_ = nullptr;
  }
  return *this;
}

int32_t FamoEngineActionResultLease::Reset() {
  FamoEngineActionResultV2* result = result_;
  FreeFn free_result = free_result_;
  result_ = nullptr;
  free_result_ = nullptr;
  if (!result)
    return FAMO_ENGINE_OK;
  return free_result ? free_result(result) : FAMO_ENGINE_E_RUNTIME;
}

void FamoEngineActionResultLease::Adopt(
    FamoEngineActionResultV2* result, FreeFn free_result) {
  result_ = result;
  free_result_ = free_result;
}

FamoEngineHost::FamoEngineHost()
    : module_(nullptr),
      initialized_(false),
      active_abi_version_(0),
      notification_handler_(nullptr),
      notification_user_data_(nullptr) {
  std::memset(&api_, 0, sizeof(api_));
  std::memset(&api_v2_, 0, sizeof(api_v2_));
  std::memset(&host_api_, 0, sizeof(host_api_));
}

FamoEngineHost::~FamoEngineHost() { Unload(); }

void FAMO_ENGINE_CALL FamoEngineHost::NotificationThunk(
    void* user_data,
    FamoEngineContext* context,
    const FamoUtf8String* message_type,
    const FamoUtf8String* message_value,
    const FamoUtf8String* state_label) noexcept {
  auto* self = static_cast<FamoEngineHost*>(user_data);
  constexpr uint32_t kMaxNotificationBytes = 1024u * 1024u;
  if (!self || !self->notification_handler_ || !message_type ||
      !message_value || !state_label ||
      message_type->length_bytes == 0 || message_value->length_bytes == 0 ||
      !ValidUtf8String(*message_type, kMaxNotificationBytes) ||
      !ValidUtf8String(*message_value, kMaxNotificationBytes) ||
      !ValidUtf8String(*state_label, kMaxNotificationBytes) ||
      (!context && !EqualsAscii(*message_type, "deploy"))) {
    return;
  }
  try {
    self->notification_handler_(
        self->notification_user_data_, context, message_type, message_value,
        state_label);
  } catch (...) {
    // A host callback is no-throw by contract. Contain a violating consumer so
    // it can never unwind through the DLL boundary or a librime worker.
  }
}

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
  active_abi_version_ = FAMO_ENGINE_ABI_VERSION;
  return FAMO_ENGINE_OK;
}

int32_t FamoEngineHost::LoadV2(
    const wchar_t* dll_path,
    const char* data_root_utf8,
    FamoEngineNotificationHandlerV2 notification_handler,
    void* notification_user_data) {
  Unload();
  notification_handler_ = notification_handler;
  notification_user_data_ =
      notification_handler ? notification_user_data : nullptr;

  HMODULE mod = ::LoadLibraryW(dll_path);
  if (!mod) {
    notification_handler_ = nullptr;
    notification_user_data_ = nullptr;
    return FAMO_ENGINE_E_RUNTIME;
  }

  auto create = reinterpret_cast<CreateV2Fn>(
      ::GetProcAddress(mod, "FamoCreateEngineApiV2"));
  if (!create) {
    ::FreeLibrary(mod);
    notification_handler_ = nullptr;
    notification_user_data_ = nullptr;
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  }

  std::memset(&api_v2_, 0, sizeof(api_v2_));
  api_v2_.struct_size = static_cast<uint32_t>(sizeof(api_v2_));
  const int32_t create_rc = create(FAMO_ENGINE_ABI_V2, &api_v2_);
  if (create_rc != FAMO_ENGINE_OK ||
      api_v2_.abi_version != FAMO_ENGINE_ABI_V2 ||
      api_v2_.struct_size <
          static_cast<uint32_t>(FAMO_ENGINE_API_V2_REQUIRED_SIZE) ||
      !TableCompleteV2(api_v2_)) {
    ::FreeLibrary(mod);
    std::memset(&api_v2_, 0, sizeof(api_v2_));
    notification_handler_ = nullptr;
    notification_user_data_ = nullptr;
    return create_rc == FAMO_ENGINE_OK ? FAMO_ENGINE_E_UNSUPPORTED_ABI
                                      : create_rc;
  }

  const int32_t notification_rc = api_v2_.set_notification_handler(
      notification_handler ? &FamoEngineHost::NotificationThunk : nullptr,
      notification_handler ? this : nullptr);
  if (notification_rc != FAMO_ENGINE_OK) {
    ::FreeLibrary(mod);
    std::memset(&api_v2_, 0, sizeof(api_v2_));
    notification_handler_ = nullptr;
    notification_user_data_ = nullptr;
    return notification_rc;
  }

  host_api_.size = static_cast<uint32_t>(sizeof(host_api_));
  host_api_.abi_version = FAMO_ENGINE_ABI_V2;
  host_api_.alloc = &HostAlloc;
  host_api_.free = &HostFree;
  host_api_.log = &HostLog;

  FamoUtf8String root;
  root.size = static_cast<uint32_t>(sizeof(root));
  root.data = data_root_utf8 ? data_root_utf8 : "";
  root.length_bytes = static_cast<uint32_t>(std::strlen(root.data));
  const int32_t initialize_rc = api_v2_.initialize(&host_api_, &root);
  if (initialize_rc != FAMO_ENGINE_OK) {
    (void)api_v2_.set_notification_handler(nullptr, nullptr);
    ::FreeLibrary(mod);
    std::memset(&api_v2_, 0, sizeof(api_v2_));
    std::memset(&host_api_, 0, sizeof(host_api_));
    notification_handler_ = nullptr;
    notification_user_data_ = nullptr;
    return initialize_rc;
  }

  module_ = static_cast<void*>(mod);
  initialized_ = true;
  active_abi_version_ = FAMO_ENGINE_ABI_V2;
  return FAMO_ENGINE_OK;
}

void FamoEngineHost::Unload() {
  if (module_) {
    if (initialized_) {
      if (active_abi_version_ == FAMO_ENGINE_ABI_V2 && api_v2_.shutdown) {
        // Disabling waits for any in-flight callback in conforming engines.
        // Keep the user binding alive until shutdown has also joined all
        // engine workers.
        if (api_v2_.set_notification_handler)
          (void)api_v2_.set_notification_handler(nullptr, nullptr);
        api_v2_.shutdown();
      } else if (active_abi_version_ == FAMO_ENGINE_ABI_VERSION &&
                 api_.shutdown) {
        api_.shutdown();
      }
    }
    ::FreeLibrary(static_cast<HMODULE>(module_));
  }
  module_ = nullptr;
  initialized_ = false;
  active_abi_version_ = 0;
  std::memset(&api_, 0, sizeof(api_));
  std::memset(&api_v2_, 0, sizeof(api_v2_));
  std::memset(&host_api_, 0, sizeof(host_api_));
  notification_handler_ = nullptr;
  notification_user_data_ = nullptr;
}

int32_t FamoEngineHost::FreeView(FamoCompositionView* view) {
  if (!module_ || !api_.free_view || !view) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_.free_view(view);
}

int32_t FamoEngineHost::FreeResultV2(FamoEngineActionResultV2* result) {
  if (!module_ || active_abi_version_ != FAMO_ENGINE_ABI_V2 ||
      !api_v2_.free_result || !result) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  return api_v2_.free_result(result);
}

bool FamoEngineHost::V2Runnable() const {
  return module_ && active_abi_version_ == FAMO_ENGINE_ABI_V2 &&
         api_v2_.abi_version == FAMO_ENGINE_ABI_V2 &&
         api_v2_.struct_size >= FAMO_ENGINE_API_V2_REQUIRED_SIZE &&
         TableCompleteV2(api_v2_);
}

FamoEngineActionRequestV2 FamoEngineHost::Action(uint32_t action) {
  FamoEngineActionRequestV2 request{};
  request.struct_size = static_cast<uint32_t>(sizeof(request));
  request.action = action;
  request.view_layout_version = FAMO_COMPOSITION_LAYOUT_V2;
  request.candidate_layout_version = FAMO_CANDIDATE_LAYOUT_V2;
  request.candidate_stride =
      static_cast<uint32_t>(FAMO_CANDIDATE_V2_STRIDE);
  return request;
}

void FamoEngineHost::SetAllocationFailureCountdownForTesting(
    int64_t countdown) noexcept {
  g_allocation_failure_countdown.store(countdown < -1 ? -1 : countdown,
                                       std::memory_order_relaxed);
}

bool FamoEngineHost::ValidateResultV2(
    const FamoEngineActionResultV2* result, uint32_t expected_action,
    uint32_t max_candidates, uint32_t max_string_bytes) noexcept {
  if (!result)
    return false;
  max_candidates =
      (std::min)(max_candidates, FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES);
  max_string_bytes =
      (std::min)(max_string_bytes, FAMO_ENGINE_V2_MAX_STRING_BYTES);
  std::unique_lock<std::mutex> allocation_lock(
      g_host_allocations_mutex, std::defer_lock);
  try {
    allocation_lock.lock();
  } catch (...) {
    return false;
  }
  const auto live_allocation =
      g_host_allocations.find(const_cast<FamoEngineActionResultV2*>(result));
  if (live_allocation == g_host_allocations.end() ||
      live_allocation->second <
          FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE ||
      live_allocation->second > FAMO_ENGINE_V2_MAX_RESULT_BYTES) {
    return false;
  }
  const void* allocation = live_allocation->first;
  const size_t allocation_size = live_allocation->second;

  if (
      result->struct_size < FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE ||
      result->action != expected_action || result->handled > 1 ||
      (result->result_flags & ~FAMO_ENGINE_RESULT_RESYNC_REQUIRED) != 0) {
    return false;
  }
  const bool resync_required =
      (result->result_flags & FAMO_ENGINE_RESULT_RESYNC_REQUIRED) != 0;
  const bool passive =
      expected_action == FAMO_ENGINE_ACTION_STATUS ||
      expected_action == FAMO_ENGINE_ACTION_PEEK_CANDIDATES;
  const bool commit_capable =
      expected_action == FAMO_ENGINE_ACTION_PROCESS_KEY ||
      expected_action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE ||
      expected_action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE ||
      expected_action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION;
  const bool business_action =
      expected_action == FAMO_ENGINE_ACTION_PROCESS_KEY ||
      expected_action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE ||
      expected_action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE ||
      expected_action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION ||
      expected_action == FAMO_ENGINE_ACTION_CLEAR_COMPOSITION ||
      expected_action == FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE ||
      expected_action == FAMO_ENGINE_ACTION_CHANGE_PAGE;
  if (!commit_capable && !passive &&
      expected_action != FAMO_ENGINE_ACTION_CLEAR_COMPOSITION &&
      expected_action != FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE &&
      expected_action != FAMO_ENGINE_ACTION_CHANGE_PAGE) {
    return false;
  }
  if (resync_required && !business_action)
    return false;
  if (passive && result->handled != 0)
    return false;

  const FamoCompositionViewV2& view = result->view;
  constexpr uint32_t kContentFlags =
      FAMO_COMPOSITION_HAS_PREEDIT | FAMO_COMPOSITION_HAS_COMMIT |
      FAMO_COMPOSITION_HAS_CANDIDATES;
  constexpr uint32_t kStatusFlags =
      FAMO_STATUS_ASCII_MODE | FAMO_STATUS_COMPOSING | FAMO_STATUS_DISABLED |
      FAMO_STATUS_FULL_SHAPE | FAMO_STATUS_ASCII_PUNCT |
      FAMO_STATUS_SIMPLIFIED;
  if (view.struct_size < FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE ||
      view.layout_version != FAMO_COMPOSITION_LAYOUT_V2 ||
      view.candidate_layout_version != FAMO_CANDIDATE_LAYOUT_V2 ||
      view.candidate_stride != FAMO_CANDIDATE_V2_STRIDE ||
      (view.state_flags & ~kContentFlags) != 0 ||
      (view.status_flags & ~kStatusFlags) != 0 ||
      view.candidate_count > max_candidates || view.is_last_page > 1 ||
      (view.candidate_count == 0 && view.candidates != nullptr) ||
      (view.candidate_count == 0 && view.highlighted_index != 0) ||
      (view.candidate_count == 0 &&
       (view.page_index != 0 || view.page_size != 0)) ||
      (view.candidate_count > 0 && view.page_size == 0) ||
      (view.candidate_count > 0 &&
       (view.highlighted_index >= view.candidate_count || !view.candidates)) ||
      (view.candidate_count > 0 &&
       reinterpret_cast<uintptr_t>(view.candidates) %
               alignof(FamoCandidateV2) !=
           0)) {
    return false;
  }

  if (view.candidate_count >
      (std::numeric_limits<size_t>::max)() / view.candidate_stride) {
    return false;
  }
  const size_t candidate_span =
      static_cast<size_t>(view.candidate_count) * view.candidate_stride;
  if (!AllocationContains(allocation, allocation_size, view.candidates,
                          candidate_span) ||
      !ValidResultString(view.preedit, max_string_bytes, allocation,
                         allocation_size) ||
      !ValidResultString(view.commit, max_string_bytes, allocation,
                         allocation_size) ||
      !ValidResultString(view.commit_preview, max_string_bytes, allocation,
                         allocation_size) ||
      !ValidResultString(view.schema_id, max_string_bytes, allocation,
                         allocation_size) ||
      !ValidResultString(view.schema_name, max_string_bytes, allocation,
                         allocation_size)) {
    return false;
  }

  if (resync_required &&
      (view.preedit.length_bytes != 0 || view.commit.length_bytes != 0 ||
       view.commit_preview.length_bytes != 0 ||
       view.schema_id.length_bytes != 0 ||
       view.schema_name.length_bytes != 0 || view.candidates != nullptr ||
       view.candidate_count != 0 || view.highlighted_index != 0 ||
       view.page_index != 0 || view.page_size != 0 ||
       view.state_flags != 0 || view.preedit_sel_start != 0 ||
       view.preedit_sel_end != 0 || view.preedit_cursor_pos != 0 ||
       view.status_flags != 0 || view.is_last_page != 0)) {
    return false;
  }

  const bool has_preedit =
      (view.state_flags & FAMO_COMPOSITION_HAS_PREEDIT) != 0;
  const bool has_commit =
      (view.state_flags & FAMO_COMPOSITION_HAS_COMMIT) != 0;
  const bool has_candidates =
      (view.state_flags & FAMO_COMPOSITION_HAS_CANDIDATES) != 0;
  if (has_preedit != (view.preedit.length_bytes != 0) ||
      has_commit != (view.commit.length_bytes != 0) ||
      has_candidates != (view.candidate_count != 0) ||
      (has_commit && (result->handled == 0 || !commit_capable)) ||
      view.preedit_sel_start > view.preedit_sel_end ||
      !IsUtf8Boundary(view.preedit, view.preedit_sel_start) ||
      !IsUtf8Boundary(view.preedit, view.preedit_sel_end) ||
      !IsUtf8Boundary(view.preedit, view.preedit_cursor_pos)) {
    return false;
  }

  const auto* candidate_bytes =
      reinterpret_cast<const unsigned char*>(view.candidates);
  for (uint32_t index = 0; index < view.candidate_count; ++index) {
    const auto* candidate = reinterpret_cast<const FamoCandidateV2*>(
        candidate_bytes + static_cast<size_t>(index) * view.candidate_stride);
    if (candidate->struct_size != FAMO_CANDIDATE_V2_STRIDE ||
        (candidate->flags & ~FAMO_CANDIDATE_FLAG_DEFAULT) != 0 ||
        !ValidResultString(candidate->text, max_string_bytes, allocation,
                           allocation_size) ||
        !ValidResultString(candidate->comment, max_string_bytes, allocation,
                           allocation_size) ||
        !ValidResultString(candidate->label, max_string_bytes, allocation,
                           allocation_size)) {
      return false;
    }
  }
  return true;
}

int32_t FamoEngineHost::CreateContext(
    const FamoUtf8String* schema_id, FamoEngineContext** out_context) {
  if (out_context)
    *out_context = nullptr;
  if (!V2Runnable() || !out_context)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.create_context(schema_id, out_context);
}

int32_t FamoEngineHost::DestroyContext(FamoEngineContext* context) {
  if (!V2Runnable() || !context)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.destroy_context(context);
}

int32_t FamoEngineHost::ExecuteAction(
    FamoEngineContext* context, const FamoEngineActionRequestV2* request,
    FamoEngineActionResultLease* out_result) {
  if (!out_result || !V2Runnable())
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const int32_t reset_result = out_result->Reset();
  if (reset_result != FAMO_ENGINE_OK)
    return reset_result;
  if (!context || !request)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;

  FamoEngineActionResultV2* raw_result = nullptr;
  const int32_t action_result =
      api_v2_.execute_action(context, request, &raw_result);
  if (action_result != FAMO_ENGINE_OK) {
    if (raw_result)
      (void)api_v2_.free_result(raw_result);
    return action_result;
  }
  if (!raw_result)
    return FAMO_ENGINE_E_RUNTIME;
  const uint32_t expected_action =
      request->action == FAMO_ENGINE_ACTION_RECOVER
          ? static_cast<uint32_t>(request->value)
          : request->action;
  if (!ValidResultEnvelope(raw_result, expected_action)) {
    (void)api_v2_.free_result(raw_result);
    return FAMO_ENGINE_E_RUNTIME;
  }
  out_result->Adopt(raw_result, api_v2_.free_result);
  return FAMO_ENGINE_OK;
}

int32_t FamoEngineHost::ExecuteActionRecovering(
    FamoEngineContext* context,
    const FamoEngineActionRequestV2* request,
    uint32_t max_recovery_attempts,
    FamoEngineActionResultLease* out_result,
    FamoEngineRecoveryOutcome* outcome) {
  if (!out_result || !outcome || !request)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *outcome = {};
  outcome->original_action = request->action;

  int32_t action_result = ExecuteAction(context, request, out_result);
  if (action_result != FAMO_ENGINE_OK)
    return action_result;
  if (!*out_result)
    return FAMO_ENGINE_E_RUNTIME;

  const bool business_action =
      request->action == FAMO_ENGINE_ACTION_PROCESS_KEY ||
      request->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE ||
      request->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE ||
      request->action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION ||
      request->action == FAMO_ENGINE_ACTION_CLEAR_COMPOSITION ||
      request->action == FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE ||
      request->action == FAMO_ENGINE_ACTION_CHANGE_PAGE;
  if ((*out_result)->result_flags == 0) {
    outcome->business_dispatched = business_action;
    outcome->handled = (*out_result)->handled != 0;
    return FAMO_ENGINE_OK;
  }
  if (!business_action ||
      (*out_result)->result_flags != FAMO_ENGINE_RESULT_RESYNC_REQUIRED) {
    (void)out_result->Reset();
    return FAMO_ENGINE_E_RUNTIME;
  }

  // ExecuteAction already authenticated the host-allocation envelope, action,
  // flags and handled bit. A RESYNC receipt intentionally has no trusted UI
  // payload: record the irreversible business dispatch before releasing it,
  // even if a buggy engine filled the ignored view with malformed fields.
  outcome->business_dispatched = true;
  outcome->recovery_pending = true;
  outcome->handled = (*out_result)->handled != 0;
  const int32_t release_result = out_result->Reset();
  if (release_result != FAMO_ENGINE_OK)
    return release_result;

  int32_t recovery_result = FAMO_ENGINE_E_RUNTIME;
  for (uint32_t attempt = 0; attempt < max_recovery_attempts; ++attempt) {
    FamoEngineActionRequestV2 recovery =
        Action(FAMO_ENGINE_ACTION_RECOVER);
    recovery.value = static_cast<int32_t>(request->action);
    recovery_result = ExecuteAction(context, &recovery, out_result);
    if (recovery_result != FAMO_ENGINE_OK)
      continue;
    if (!*out_result || (*out_result)->result_flags != 0 ||
        (*out_result)->action != request->action ||
        ((*out_result)->handled != 0) != outcome->handled) {
      (void)out_result->Reset();
      return FAMO_ENGINE_E_RUNTIME;
    }
    outcome->recovery_pending = false;
    return FAMO_ENGINE_OK;
  }
  return recovery_result;
}

int32_t FamoEngineHost::SetOption(FamoEngineContext* context,
                                  const FamoUtf8String* name,
                                  int32_t value) {
  if (!V2Runnable())
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.set_option(context, name, value);
}

int32_t FamoEngineHost::GetOption(FamoEngineContext* context,
                                  const FamoUtf8String* name,
                                  int32_t* out_value) {
  if (out_value)
    *out_value = 0;
  if (!V2Runnable() || !out_value)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.get_option(context, name, out_value);
}

int32_t FamoEngineHost::SetProperty(FamoEngineContext* context,
                                    const FamoUtf8String* name,
                                    const FamoUtf8String* value) {
  if (!V2Runnable())
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.set_property(context, name, value);
}

int32_t FamoEngineHost::DeploySchema(
    const FamoUtf8String* schema_id) {
  if (!V2Runnable())
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return api_v2_.deploy_schema(schema_id);
}

bool FamoEngineHost::AbiRunnable() const {
  // Frozen v1 adapter surface for legacy clients and compatibility canaries.
  return module_ &&
         api_.size >= kV11Size &&
         api_.get_status && api_.get_option && api_.commit_composition &&
         api_.clear_composition && api_.highlight_candidate && api_.change_page;
}

bool FamoEngineHost::CanPeekCandidates() const {
  return module_ &&
         api_.size >= static_cast<uint32_t>(
                          offsetof(FamoEngineApi, select_candidate_absolute)) &&
         api_.peek_candidates;
}

bool FamoEngineHost::CanSelectCandidateAbsolute() const {
  return module_ &&
         api_.size >= static_cast<uint32_t>(sizeof(FamoEngineApi)) &&
         api_.select_candidate_absolute;
}
