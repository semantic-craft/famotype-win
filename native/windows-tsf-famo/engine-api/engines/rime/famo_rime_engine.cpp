// FamoRimeEngine.dll - librime adapter for FamoEngineApi v1.
//
// Implements the engine ABI by delegating to the RimeApi function table
// (rime_get_api()), linking librime (rime.lib). No RIME struct escapes the DLL:
// RIME output is copied into host-allocated UTF-8 and the RIME structs are freed
// immediately; the host later releases the view via free_view().
//
// Behavioral correctness of candidates depends on deployed schemas/dicts and is
// verified on the target machine (see task prd/design). On this machine the
// smoke proves build+link+load+init+deploy+roundtrip without crashing.
#include <algorithm>
#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "../../famo_engine_api.h"
#include "../action_v2_result.h"
#include "../view_abi.h"
#include "keymap.h"

#include <rime_api.h>

// Opaque context: one RIME session plus the per-session recovery receipt. A
// retained RimeCommit is owned by this context until its text is safely copied
// into either a published result or the pending recovery snapshot.
struct FamoEngineContext {
  RimeSessionId session = 0;
  uint64_t incarnation = 0;
  bool recovery_required = false;
  uint32_t recovery_action = 0;
  bool recovery_handled = false;
  bool recovery_consume_commit = false;
  bool has_pending_snapshot = false;
  famo_action_v2::Snapshot pending_snapshot;
  bool has_pending_rime_commit = false;
  RimeCommit pending_rime_commit{};
};

namespace {

FamoEngineHostApi g_host;
RimeApi* g_rime = nullptr;
bool g_rime_initialized = false;
std::string g_data_root;
uint32_t g_initialized_abi_version = 0;
famo_action_v2::ResultStore g_v2_results;
std::mutex g_context_mutex;
struct PendingNotification {
  std::string message_type;
  std::string message_value;
  std::string state_label;
};
struct ContextBinding {
  FamoEngineContext* context = nullptr;
  uint64_t incarnation = 0;
  bool published = false;
  bool retiring = false;
  size_t active_notifications = 0;
  std::vector<PendingNotification> pending;
};
std::unordered_map<RimeSessionId, ContextBinding> g_contexts;
// librime's callback carries only the numeric session id, not a per-session
// generation. Keep every allocated session alive until engine shutdown so
// librime cannot recycle its numeric id while a late callback from that
// incarnation could still exist. In an epoch that has never exposed
// notifications, sessions can instead be destroyed immediately because no
// callback route exists. g_seen_session_ids is both the bounded quarantine
// and the shutdown destroy list for anything not released immediately.
std::unordered_set<RimeSessionId> g_seen_session_ids;
constexpr size_t kMaxSeenSessionIds = 1024u;
std::mutex g_context_creation_mutex;
bool g_session_registry_poisoned = false;
uint64_t g_next_context_incarnation = 1;
std::condition_variable g_context_lifetime_cv;
std::mutex g_notification_mutex;
std::mutex g_notification_registration_mutex;
FamoEngineNotificationHandlerV2 g_notification_handler = nullptr;
void* g_notification_user_data = nullptr;
// Hosts register before initialize. This bit therefore belongs to the whole
// loaded-engine epoch and must survive initialization; disabling a handler
// later does not make an earlier callback incarnation safe to recycle.
bool g_notification_ever_enabled = false;
std::mutex g_callback_lifetime_mutex;
std::condition_variable g_callback_lifetime_cv;
bool g_callback_gate_closed = true;
size_t g_active_callbacks = 0;
thread_local size_t g_callback_depth = 0;

size_t SeenSessionLimit() noexcept {
  char configured[32]{};
  const DWORD configured_size = ::GetEnvironmentVariableA(
      "FAMO_TEST_MAX_SEEN_SESSION_IDS", configured,
      static_cast<DWORD>(sizeof(configured)));
  if (configured_size == 0 || configured_size >= sizeof(configured)) {
    return kMaxSeenSessionIds;
  }
  char* end = nullptr;
  const unsigned long long parsed =
      std::strtoull(configured, &end, 10);
  const bool valid =
      end != configured && *end == '\0' && parsed != 0 &&
      parsed <= kMaxSeenSessionIds;
  if (!valid) {
    return kMaxSeenSessionIds;
  }
  return static_cast<size_t>(parsed);
}

bool TestSwitch(const char* name) noexcept {
  char value[2]{};
  return ::GetEnvironmentVariableA(
             name, value, static_cast<DWORD>(sizeof(value))) != 0;
}

void PauseNotificationEnableForTest() noexcept {
  if (!TestSwitch("FAMO_TEST_RIME_NOTIFICATION_ENABLE_PAUSE"))
    return;
  (void)::SetEnvironmentVariableA(
      "FAMO_TEST_RIME_NOTIFICATION_ENABLE_ENTERED", "1");
  const ULONGLONG deadline = ::GetTickCount64() + 5000;
  while (!TestSwitch("FAMO_TEST_RIME_NOTIFICATION_ENABLE_RELEASE") &&
         ::GetTickCount64() < deadline) {
    ::Sleep(1);
  }
}

bool NotificationEverEnabled() noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_notification_mutex);
    return g_notification_ever_enabled;
  } catch (...) {
    // Retaining is the safe failure mode.
    return true;
  }
}

void ReleaseUnpublishedSession(RimeSessionId session) noexcept {
  if (!session)
    return;
  try {
    std::lock_guard<std::mutex> creation_lock(g_context_creation_mutex);
    if (NotificationEverEnabled())
      return;
    if (!g_rime || !g_rime->destroy_session(session))
      return;
    std::lock_guard<std::mutex> context_lock(g_context_mutex);
    g_seen_session_ids.erase(session);
  } catch (...) {
    // Keep the session in the bounded shutdown quarantine.
  }
}

class CallbackLifetime {
 public:
  CallbackLifetime() noexcept {
    try {
      std::lock_guard<std::mutex> lock(g_callback_lifetime_mutex);
      if (!g_callback_gate_closed) {
        ++g_active_callbacks;
        ++g_callback_depth;
        entered_ = true;
      }
    } catch (...) {
    }
  }

  ~CallbackLifetime() {
    if (!entered_)
      return;
    if (g_callback_depth > 0)
      --g_callback_depth;
    try {
      std::lock_guard<std::mutex> lock(g_callback_lifetime_mutex);
      if (g_active_callbacks > 0)
        --g_active_callbacks;
      if (g_active_callbacks == 0)
        g_callback_lifetime_cv.notify_all();
    } catch (...) {
    }
  }

  explicit operator bool() const noexcept { return entered_; }

 private:
  bool entered_ = false;
};

void ReleasePendingCommit(FamoEngineContext* context) noexcept {
  if (!context || !context->has_pending_rime_commit)
    return;
  if (g_rime) {
    try {
      g_rime->free_commit(&context->pending_rime_commit);
    } catch (...) {
      // The context still relinquishes this retained handle exactly once.
    }
  }
  context->pending_rime_commit = {};
  context->has_pending_rime_commit = false;
}

void DisableNotifications() noexcept {
  // Serialize registration state changes without taking the handler mutex
  // across librime. set_notification_handler may call back synchronously.
  try {
    std::lock_guard<std::mutex> registration_lock(
        g_notification_registration_mutex);
    try {
      std::lock_guard<std::mutex> lock(g_callback_lifetime_mutex);
      // Close before unregistering. A callback that races the librime call can
      // now enter only far enough to observe the closed gate.
      g_callback_gate_closed = true;
    } catch (...) {
    }
    if (g_rime) {
      try {
        g_rime->set_notification_handler(nullptr, nullptr);
      } catch (...) {
        // Continue to the in-flight callback barrier below.
      }
    }
  } catch (...) {
  }
  try {
    std::unique_lock<std::mutex> lifetime_lock(g_callback_lifetime_mutex);
    g_callback_lifetime_cv.wait(
        lifetime_lock, [] { return g_active_callbacks == 0; });
  } catch (...) {
    // Continue to clear the borrowed host target below.
  }
  try {
    std::lock_guard<std::mutex> notification_lock(g_notification_mutex);
    g_notification_handler = nullptr;
    g_notification_user_data = nullptr;
  } catch (...) {
    // std::mutex::lock should not fail for a valid process mutex. Keep this
    // cleanup path no-unwind even if the runtime reports otherwise.
  }
}

void ResetRimeState() noexcept {
  // Stop new callbacks and wait for any callback already inside the host
  // boundary before finalizing librime or clearing opaque-context mappings.
  DisableNotifications();
  const bool audit_quarantine =
      TestSwitch("FAMO_TEST_RIME_QUARANTINE_AUDIT");
  bool notifications_drained = false;
  if (audit_quarantine) {
    try {
      {
        std::lock_guard<std::mutex> callback_lock(
            g_callback_lifetime_mutex);
        notifications_drained =
            g_callback_gate_closed && g_active_callbacks == 0;
      }
      std::lock_guard<std::mutex> context_lock(g_context_mutex);
      notifications_drained =
          notifications_drained &&
          std::all_of(g_contexts.begin(), g_contexts.end(),
                      [](const auto& entry) {
                        return entry.second.active_notifications == 0;
                      });
    } catch (...) {
      notifications_drained = false;
    }
  }
  g_v2_results.Drain(g_host);
  try {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    for (auto& entry : g_contexts)
      ReleasePendingCommit(entry.second.context);
  } catch (...) {
    // Continue with the no-unwind shutdown path.
  }
  size_t attempted_quarantined_sessions = 0;
  size_t destroyed_quarantined_sessions = 0;
  size_t failed_quarantined_sessions = 0;
  const bool fail_quarantine_destroy_for_test =
      audit_quarantine &&
      TestSwitch("FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILURE");
  if (g_rime && g_rime_initialized) {
    try {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      for (const RimeSessionId session : g_seen_session_ids) {
        ++attempted_quarantined_sessions;
        bool destroyed = false;
        try {
          destroyed =
              !fail_quarantine_destroy_for_test &&
              g_rime->destroy_session(session) != False;
        } catch (...) {
          destroyed = false;
        }
        if (destroyed)
          ++destroyed_quarantined_sessions;
        else
          ++failed_quarantined_sessions;
      }
    } catch (...) {
      // finalize below is the last-resort owner for any quarantined session
      // that could not be destroyed individually.
    }
    try {
      g_rime->finalize();
    } catch (...) {
      // This is the last-resort ABI cleanup path; never unwind to the host.
    }
  }
  try {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    g_contexts.clear();
    g_seen_session_ids.clear();
    g_session_registry_poisoned = false;
    g_next_context_incarnation = 1;
  } catch (...) {
    // Cleanup remains no-unwind; host lifecycle requires contexts to have
    // already been destroyed, so this is only a defensive final sweep.
  }
  g_rime_initialized = false;
  g_rime = nullptr;
  g_data_root.clear();
  g_initialized_abi_version = 0;
  try {
    std::lock_guard<std::mutex> notification_lock(g_notification_mutex);
    g_notification_ever_enabled = false;
  } catch (...) {
  }
  std::memset(&g_host, 0, sizeof(g_host));
  if (audit_quarantine) {
    char attempted_count[32]{};
    char destroyed_count[32]{};
    char failed_count[32]{};
    _ui64toa_s(static_cast<unsigned long long>(
                   attempted_quarantined_sessions),
               attempted_count, std::size(attempted_count), 10);
    _ui64toa_s(static_cast<unsigned long long>(
                   destroyed_quarantined_sessions),
               destroyed_count, std::size(destroyed_count), 10);
    _ui64toa_s(static_cast<unsigned long long>(
                   failed_quarantined_sessions),
               failed_count, std::size(failed_count), 10);
    ::SetEnvironmentVariableA("FAMO_TEST_RIME_QUARANTINE_ATTEMPTED",
                              attempted_count);
    ::SetEnvironmentVariableA("FAMO_TEST_RIME_QUARANTINE_DESTROYED",
                              destroyed_count);
    ::SetEnvironmentVariableA("FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED",
                              failed_count);
    ::SetEnvironmentVariableA("FAMO_TEST_RIME_NOTIFICATIONS_DRAINED",
                              notifications_drained ? "1" : "0");
  }
}

FamoUtf8String DupN(const char* s, size_t n) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = nullptr;
  v.length_bytes = 0;
  if (!g_host.alloc) return v;
  char* p = static_cast<char*>(g_host.alloc(n + 1));
  if (!p) return v;
  if (n) std::memcpy(p, s, n);
  p[n] = '\0';
  v.data = p;
  v.length_bytes = static_cast<uint32_t>(n);
  return v;
}

FamoUtf8String DupC(const char* s) { return DupN(s ? s : "", s ? std::strlen(s) : 0); }

FamoUtf8String Static(const char* s) {
  FamoUtf8String v;
  v.size = static_cast<uint32_t>(sizeof(FamoUtf8String));
  v.data = s;
  v.length_bytes = static_cast<uint32_t>(std::strlen(s));
  return v;
}

FamoUtf8String Borrowed(const std::string& value) {
  FamoUtf8String result{};
  result.size = static_cast<uint32_t>(sizeof(result));
  result.data = value.empty() ? nullptr : value.data();
  result.length_bytes = static_cast<uint32_t>(value.size());
  return result;
}

void DeliverNotification(
    FamoEngineContext* context,
    const PendingNotification& notification) noexcept {
  CallbackLifetime lifetime;
  if (!lifetime)
    return;
  try {
    FamoEngineNotificationHandlerV2 handler = nullptr;
    void* user_data = nullptr;
    {
      std::lock_guard<std::mutex> notification_lock(g_notification_mutex);
      handler = g_notification_handler;
      user_data = g_notification_user_data;
    }
    if (!handler)
      return;
    const FamoUtf8String type = Borrowed(notification.message_type);
    const FamoUtf8String value = Borrowed(notification.message_value);
    const FamoUtf8String label = Borrowed(notification.state_label);
    // The host target is external code. Never invoke it while holding either
    // the notification-handler mutex or the context registry mutex.
    handler(user_data, context, &type, &value, &label);
  } catch (...) {
    // Never unwind through librime or a create_context ABI call.
  }
}

class ContextNotificationBorrow {
 public:
  explicit ContextNotificationBorrow(RimeSessionId session) noexcept
      : session_(session) {
    try {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      const auto found = g_contexts.find(session_);
      if (found == g_contexts.end() || found->second.retiring ||
          !found->second.context) {
        return;
      }
      context_ = found->second.context;
      incarnation_ = found->second.incarnation;
      ++found->second.active_notifications;
      active_ = true;
    } catch (...) {
    }
  }

  ContextNotificationBorrow(const ContextNotificationBorrow&) = delete;
  ContextNotificationBorrow& operator=(const ContextNotificationBorrow&) =
      delete;

  ~ContextNotificationBorrow() {
    if (!active_)
      return;
    try {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      const auto found = g_contexts.find(session_);
      if (found != g_contexts.end() &&
          found->second.context == context_ &&
          found->second.incarnation == incarnation_ &&
          found->second.active_notifications > 0) {
        --found->second.active_notifications;
        if (found->second.active_notifications == 0)
          g_context_lifetime_cv.notify_all();
      }
    } catch (...) {
    }
  }

  explicit operator bool() const noexcept { return active_; }
  FamoEngineContext* context() const noexcept { return context_; }

  // Route after every external lookup has completed. The binding may have
  // started retiring meanwhile; in that case the event is simply stale.
  bool ShouldDeliverOrQueue(PendingNotification notification) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    const auto found = g_contexts.find(session_);
    if (found == g_contexts.end() ||
        found->second.context != context_ ||
        found->second.incarnation != incarnation_ ||
        found->second.retiring) {
      return false;
    }
    if (found->second.published)
      return true;
    constexpr size_t kMaxCreateNotifications = 32;
    if (found->second.pending.size() >= kMaxCreateNotifications)
      found->second.pending.erase(found->second.pending.begin());
    found->second.pending.push_back(std::move(notification));
    return false;
  }

 private:
  RimeSessionId session_ = 0;
  FamoEngineContext* context_ = nullptr;
  uint64_t incarnation_ = 0;
  bool active_ = false;
};

bool BoundedCStringLength(const char* value,
                          size_t* out_length) noexcept;

bool ValidUtf8Bytes(const char* data, size_t length) noexcept {
  if (length != 0 && !data)
    return false;
  const auto* bytes =
      reinterpret_cast<const unsigned char*>(data);
  size_t index = 0;
  while (index < length) {
    const unsigned char first = bytes[index++];
    if (first <= 0x7f)
      continue;
    size_t trailing = 0;
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
    for (size_t offset = 1; offset < trailing; ++offset) {
      const unsigned char continuation = bytes[index++];
      if (continuation < 0x80 || continuation > 0xbf)
        return false;
    }
  }
  return true;
}

bool CopyNotificationString(const char* value,
                            std::string* target) {
  size_t length = 0;
  if (!target || !BoundedCStringLength(value, &length) ||
      !ValidUtf8Bytes(value, length)) {
    return false;
  }
  target->assign(value ? value : "", length);
  return true;
}

struct NotificationBorrowSignal {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
};

void RouteNotification(uintptr_t session_id,
                       const char* message_type,
                       const char* message_value,
                       NotificationBorrowSignal* signal = nullptr) noexcept {
  CallbackLifetime lifetime;
  if (!lifetime)
    return;
  if (!message_type || !message_value)
    return;
  try {
    std::string safe_type;
    std::string safe_value;
    if (!CopyNotificationString(message_type, &safe_type) ||
        !CopyNotificationString(message_value, &safe_value)) {
      return;
    }
    const RimeSessionId source_session =
        static_cast<RimeSessionId>(session_id);
    ContextNotificationBorrow borrow(source_session);
    if (signal) {
      std::unique_lock<std::mutex> lock(signal->mutex);
      signal->entered = true;
      signal->condition.notify_all();
      signal->condition.wait(lock, [&] { return signal->release; });
    }
    FamoEngineContext* context = nullptr;
    if (session_id != 0) {
      if (!borrow)
        return;  // Late/session-scoped event: never relabel it as engine-wide.
      context = borrow.context();
    }

    std::string safe_label;
    if (context && safe_type == "option") {
      const Bool state =
          safe_value.empty() || safe_value[0] != '!';
      const char* option_name =
          safe_value.c_str() + (state ? 0 : 1);
      if (RIME_API_AVAILABLE(g_rime, get_state_label)) {
        const char* resolved =
            g_rime->get_state_label(source_session, option_name, state);
        if (resolved &&
            !CopyNotificationString(resolved, &safe_label)) {
          return;
        }
      }
    }
    PendingNotification notification{
        std::move(safe_type), std::move(safe_value),
        std::move(safe_label)};
    if (session_id != 0 &&
        !borrow.ShouldDeliverOrQueue(notification))
      return;
    DeliverNotification(context, notification);
  } catch (...) {
    // Never unwind from a librime worker through its C callback boundary.
  }
}

void ReOnNotifyV2(void* /*context_object*/,
                  uintptr_t session_id,
                  const char* message_type,
                  const char* message_value) noexcept {
  RouteNotification(session_id, message_type, message_value);
}

void FreeStr(FamoUtf8String* v) {
  if (v && v->data && g_host.free) {
    g_host.free(const_cast<char*>(v->data));
    v->data = nullptr;
    v->length_bytes = 0;
  }
}

std::string AsStd(const FamoUtf8String* s) {
  if (s && s->data) return std::string(s->data, s->length_bytes);
  return std::string();
}

inline uint32_t NonNeg(int v) { return static_cast<uint32_t>(v < 0 ? 0 : v); }

bool BoundedCStringLength(const char* value, size_t* out_length) noexcept {
  if (!out_length)
    return false;
  *out_length = 0;
  if (!value)
    return true;
  for (size_t length = 0;
       length <= FAMO_ENGINE_V2_MAX_STRING_BYTES; ++length) {
    if (value[length] == '\0') {
      *out_length = length;
      return true;
    }
  }
  return false;
}

bool AssignBoundedString(const char* value, std::string* target) {
  size_t length = 0;
  if (!target || !BoundedCStringLength(value, &length))
    return false;
  target->assign(value ? value : "", length);
  return true;
}

struct PreviewLabels {
  size_t page_size = 0;
  std::string select_keys;
  std::vector<std::string> select_labels;

  std::string ForAbsoluteIndex(uint64_t absolute_index) const {
    size_t slot_count = page_size;
    if (slot_count == 0)
      slot_count = select_keys.size();
    if (slot_count == 0)
      return {};
    const size_t slot =
        static_cast<size_t>(absolute_index % slot_count);
    if (slot < select_labels.size() &&
        !select_labels[slot].empty()) {
      return select_labels[slot];
    }
    if (slot < select_keys.size())
      return std::string(1, select_keys[slot]);
    return {};
  }
};

PreviewLabels ReadPreviewLabels(RimeSessionId session) {
  PreviewLabels result;
  RIME_STRUCT(RimeContext, context);
  if (!g_rime->get_context(session, &context))
    return result;
  try {
    size_t key_count = 0;
    if (BoundedCStringLength(context.menu.select_keys, &key_count) &&
        key_count != 0) {
      result.select_keys.assign(context.menu.select_keys, key_count);
    }
    if (context.menu.page_size > 0)
      result.page_size = static_cast<size_t>(context.menu.page_size);
    else if (!result.select_keys.empty())
      result.page_size = result.select_keys.size();

    const int label_count =
        (std::max)(context.menu.num_candidates, 0);
    if (context.select_labels && label_count > 0) {
      result.select_labels.resize(static_cast<size_t>(label_count));
      for (int i = 0; i < label_count; ++i) {
        if (context.select_labels[i]) {
          (void)AssignBoundedString(
              context.select_labels[i],
              &result.select_labels[static_cast<size_t>(i)]);
        }
      }
    }
  } catch (...) {
    g_rime->free_context(&context);
    throw;
  }
  g_rime->free_context(&context);
  return result;
}

bool DuplicateCString(const char* value, FamoUtf8String* target) {
  if (!target)
    return false;
  *target = DupC(value);
  return !value || !*value || target->data != nullptr;
}

void ReleaseViewStorage(FamoCompositionView* view, uint32_t view_size) {
  if (!view)
    return;
  FreeStr(&view->preedit);
  FreeStr(&view->commit);
  if (view->candidates && g_host.free) {
    void* candidates = const_cast<FamoCandidate*>(view->candidates);
    const size_t stride = famo_view_abi::CandidateStride(view_size);
    for (uint32_t i = 0; i < view->candidate_count; ++i) {
      FamoCandidate* candidate =
          famo_view_abi::CandidateAt(candidates, i, stride);
      FreeStr(&candidate->text);
      FreeStr(&candidate->comment);
      if (famo_view_abi::HasV12Candidates(view_size))
        FreeStr(&candidate->label);
    }
    g_host.free(candidates);
    view->candidates = nullptr;
    view->candidate_count = 0;
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, commit_preview),
          sizeof(view->commit_preview))) {
    FreeStr(&view->commit_preview);
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_id),
          sizeof(view->schema_id))) {
    FreeStr(&view->schema_id);
  }
  if (famo_view_abi::HasField(
          view_size, offsetof(FamoCompositionView, schema_name),
          sizeof(view->schema_name))) {
    FreeStr(&view->schema_name);
  }
}

// v1.1: fold RimeStatus into the view (schema id/name + status_flags). No commit
// consume; safe to call from get_status and from the per-key fill alike.
bool FillStatus(RimeSessionId session, uint32_t view_size,
                FamoCompositionView* out) {
  const bool has_schema_id = famo_view_abi::HasField(
      view_size, offsetof(FamoCompositionView, schema_id),
      sizeof(out->schema_id));
  const bool has_schema_name = famo_view_abi::HasField(
      view_size, offsetof(FamoCompositionView, schema_name),
      sizeof(out->schema_name));
  const bool has_status_flags = famo_view_abi::HasField(
      view_size, offsetof(FamoCompositionView, status_flags),
      sizeof(out->status_flags));
  if (!has_schema_id && !has_schema_name && !has_status_flags) return true;

  RIME_STRUCT(RimeStatus, status);
  if (!g_rime->get_status(session, &status)) return true;
  bool copied = true;
  if (has_schema_id)
    copied = DuplicateCString(status.schema_id, &out->schema_id);
  if (copied && has_schema_name)
    copied = DuplicateCString(status.schema_name, &out->schema_name);
  if (has_status_flags) {
    uint32_t flags = 0;
    if (status.is_ascii_mode) flags |= FAMO_STATUS_ASCII_MODE;
    if (status.is_composing) flags |= FAMO_STATUS_COMPOSING;
    if (status.is_disabled) flags |= FAMO_STATUS_DISABLED;
    if (status.is_full_shape) flags |= FAMO_STATUS_FULL_SHAPE;
    if (status.is_ascii_punct) flags |= FAMO_STATUS_ASCII_PUNCT;
    const bool traditional =
        g_rime->get_option(session, "traditionalization") ||
        g_rime->get_option(session, "zh_trad");
    if (!traditional) flags |= FAMO_STATUS_SIMPLIFIED;
    out->status_flags = flags;
  }
  g_rime->free_status(&status);
  return copied;
}

// Read the session's current context (+ status) into a host-owned view.
// consume_commit: pull RimeCommit (true on the key hot path, false for a pure
// status read that must not swallow pending commit text).
int32_t FillFromSession(RimeSessionId session, FamoCompositionView* out,
                        uint32_t view_size, bool consume_commit) {
  FamoCompositionView result;
  famo_view_abi::BeginResult(&result, view_size);
  const auto fail = [&]() {
    ReleaseViewStorage(&result, view_size);
    return FAMO_ENGINE_E_RUNTIME;
  };

  if (consume_commit) {
    RIME_STRUCT(RimeCommit, commit);
    if (g_rime->get_commit(session, &commit)) {
      const bool copied =
          !commit.text || !*commit.text ||
          DuplicateCString(commit.text, &result.commit);
      g_rime->free_commit(&commit);
      if (!copied)
        return fail();
    }
  }

  RIME_STRUCT(RimeContext, ctx);
  if (g_rime->get_context(session, &ctx)) {
    bool copied = true;
    if (ctx.composition.preedit && *ctx.composition.preedit) {
      copied =
          DuplicateCString(ctx.composition.preedit, &result.preedit);
    }
    const bool has_preedit = result.preedit.length_bytes != 0;
    if (has_preedit && famo_view_abi::HasField(
            view_size, offsetof(FamoCompositionView, preedit_sel_start),
            sizeof(result.preedit_sel_start))) {
      result.preedit_sel_start = NonNeg(ctx.composition.sel_start);
    }
    if (has_preedit && famo_view_abi::HasField(
            view_size, offsetof(FamoCompositionView, preedit_sel_end),
            sizeof(result.preedit_sel_end))) {
      result.preedit_sel_end = NonNeg(ctx.composition.sel_end);
    }
    if (has_preedit && famo_view_abi::HasField(
            view_size, offsetof(FamoCompositionView, preedit_cursor_pos),
            sizeof(result.preedit_cursor_pos))) {
      result.preedit_cursor_pos = NonNeg(ctx.composition.cursor_pos);
    }
    if (copied && ctx.commit_text_preview && *ctx.commit_text_preview &&
        famo_view_abi::HasField(
            view_size, offsetof(FamoCompositionView, commit_preview),
            sizeof(result.commit_preview))) {
      copied = DuplicateCString(ctx.commit_text_preview,
                                &result.commit_preview);
    }
    const int n = ctx.menu.num_candidates;
    if (copied && n > 0) {
      const size_t stride = famo_view_abi::CandidateStride(view_size);
      if (!ctx.menu.candidates || !g_host.alloc ||
          static_cast<size_t>(n) >
              (std::numeric_limits<size_t>::max)() / stride) {
        copied = false;
      } else {
        const size_t bytes = stride * static_cast<size_t>(n);
        void* candidates = g_host.alloc(bytes);
        if (!candidates) {
          copied = false;
        } else {
          std::memset(candidates, 0, bytes);
          result.candidates =
              static_cast<const FamoCandidate*>(candidates);
          result.candidate_count = static_cast<uint32_t>(n);
        // Length of the page's select-key string, hoisted so the per-candidate
        // fallback below indexes it in-bounds (a misconfigured schema can define
        // fewer select_keys than candidates on the page).
          const size_t n_keys =
              ctx.menu.select_keys ? std::strlen(ctx.menu.select_keys) : 0;
          for (int i = 0; i < n && copied; ++i) {
            FamoCandidate* candidate = famo_view_abi::CandidateAt(
                candidates, static_cast<size_t>(i), stride);
            candidate->size = static_cast<uint32_t>(stride);
            const char* text = ctx.menu.candidates[i].text;
            const char* comment = ctx.menu.candidates[i].comment;
            if ((text && *text &&
                 !DuplicateCString(text, &candidate->text)) ||
                (comment && *comment &&
                 !DuplicateCString(comment, &candidate->comment))) {
              copied = false;
              break;
            }
            candidate->flags =
                (i == ctx.menu.highlighted_candidate_index)
                    ? FAMO_CANDIDATE_FLAG_DEFAULT
                    : 0u;
            // v1.2 label: per-candidate select_labels ->
            // menu.select_keys[i] -> (i+1)%10. These index the page.
            if (famo_view_abi::HasV12Candidates(view_size)) {
              if (ctx.select_labels && ctx.select_labels[i] &&
                  *ctx.select_labels[i]) {
                copied = DuplicateCString(ctx.select_labels[i],
                                           &candidate->label);
              } else if (static_cast<size_t>(i) < n_keys) {
                const char key[2] = {ctx.menu.select_keys[i], '\0'};
                copied = DuplicateCString(key, &candidate->label);
              } else {
                const char digit[2] = {
                    static_cast<char>('0' + (i + 1) % 10), '\0'};
                copied = DuplicateCString(digit, &candidate->label);
              }
            }
          }
        }
      }
    }
    const uint32_t highlighted =
        NonNeg(ctx.menu.highlighted_candidate_index);
    result.highlighted_index =
        result.candidate_count != 0 &&
                highlighted < result.candidate_count
            ? highlighted
            : 0;
    result.page_index =
        static_cast<uint32_t>(ctx.menu.page_no < 0 ? 0 : ctx.menu.page_no);
    result.page_size =
        static_cast<uint32_t>(ctx.menu.page_size < 0 ? 0
                                                     : ctx.menu.page_size);
    if (famo_view_abi::HasField(
            view_size, offsetof(FamoCompositionView, is_last_page),
            sizeof(result.is_last_page))) {
      result.is_last_page = ctx.menu.is_last_page ? 1u : 0u;  // v1.2
    }
    g_rime->free_context(&ctx);
    if (!copied)
      return fail();
  }

  if (!FillStatus(session, view_size, &result))
    return fail();

  uint32_t flags = 0;
  if (result.preedit.length_bytes) flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (result.commit.length_bytes) flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (result.candidate_count) flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  result.state_flags = flags;
  famo_view_abi::Publish(out, result, view_size);
  return FAMO_ENGINE_OK;
}

int32_t SnapshotFromSession(FamoEngineContext* engine_context,
                            bool consume_commit,
                            famo_action_v2::Snapshot* out) {
  if (!engine_context || !out)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const RimeSessionId session = engine_context->session;
  famo_action_v2::Snapshot snapshot;

  // Fetch a consumptive commit immediately after the business mutation while
  // the emergency result is already live. Keep the RIME allocation owned by
  // the context until all later snapshot work and result publication succeed.
  // RECOVER therefore only copies retained state and never repeats a mutation.
  if (consume_commit && !engine_context->has_pending_rime_commit) {
    RIME_STRUCT(RimeCommit, commit);
    if (g_rime->get_commit(session, &commit)) {
      engine_context->pending_rime_commit = commit;
      engine_context->has_pending_rime_commit = true;
    }
  }

  RIME_STRUCT(RimeContext, context);
  if (!g_rime->get_context(session, &context))
    return FAMO_ENGINE_E_RUNTIME;
  if (context.menu.num_candidates > 0 && !context.menu.candidates) {
    g_rime->free_context(&context);
    return FAMO_ENGINE_E_RUNTIME;
  }
  try {
    const bool copied_preedit =
        context.composition.preedit &&
        AssignBoundedString(context.composition.preedit,
                            &snapshot.preedit);
    if (copied_preedit && !snapshot.preedit.empty()) {
      snapshot.preedit_sel_start =
          NonNeg(context.composition.sel_start);
      snapshot.preedit_sel_end = NonNeg(context.composition.sel_end);
      snapshot.preedit_cursor_pos =
          NonNeg(context.composition.cursor_pos);
    } else {
      // Oversized/unavailable preedit is optional UI. Drop it atomically with
      // every offset so the final result remains self-consistent and
      // recoverable instead of publishing an empty string with stale offsets.
      snapshot.preedit.clear();
      snapshot.preedit_sel_start = 0;
      snapshot.preedit_sel_end = 0;
      snapshot.preedit_cursor_pos = 0;
    }
    if (context.commit_text_preview)
      (void)AssignBoundedString(context.commit_text_preview,
                                &snapshot.commit_preview);
    const int candidate_count = (std::min)(
        context.menu.num_candidates,
        static_cast<int>(FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES));
    size_t select_key_count = 0;
    if (!BoundedCStringLength(context.menu.select_keys, &select_key_count))
      select_key_count = 0;
    if (candidate_count > 0)
      snapshot.candidates.reserve(static_cast<size_t>(candidate_count));
    for (int i = 0; i < candidate_count; ++i) {
      famo_action_v2::CandidateSnapshot candidate;
      if ((context.menu.candidates[i].text &&
           !AssignBoundedString(context.menu.candidates[i].text,
                                &candidate.text)) ||
          (context.menu.candidates[i].comment &&
           !AssignBoundedString(context.menu.candidates[i].comment,
                                &candidate.comment))) {
        snapshot.candidates.clear();
        break;
      }
      if (context.select_labels && context.select_labels[i]) {
        if (!AssignBoundedString(context.select_labels[i],
                                 &candidate.label)) {
          snapshot.candidates.clear();
          break;
        }
      } else if (static_cast<size_t>(i) < select_key_count) {
        candidate.label.assign(1, context.menu.select_keys[i]);
      } else {
        candidate.label.assign(
            1, static_cast<char>('0' + ((i + 1) % 10)));
      }
      candidate.flags =
          i == context.menu.highlighted_candidate_index
              ? FAMO_CANDIDATE_FLAG_DEFAULT
              : 0u;
      snapshot.candidates.push_back(std::move(candidate));
    }
    snapshot.highlighted_index =
        NonNeg(context.menu.highlighted_candidate_index);
    if (snapshot.highlighted_index >= snapshot.candidates.size())
      snapshot.highlighted_index = 0;
    snapshot.page_index = NonNeg(context.menu.page_no);
    snapshot.page_size = NonNeg(context.menu.page_size);
    snapshot.is_last_page = context.menu.is_last_page ? 1u : 0u;
  } catch (...) {
    g_rime->free_context(&context);
    return FAMO_ENGINE_E_RUNTIME;
  }
  g_rime->free_context(&context);

  RIME_STRUCT(RimeStatus, status);
  if (!g_rime->get_status(session, &status))
    return FAMO_ENGINE_E_RUNTIME;
  try {
    if (status.schema_id)
      (void)AssignBoundedString(status.schema_id, &snapshot.schema_id);
    if (status.schema_name)
      (void)AssignBoundedString(status.schema_name, &snapshot.schema_name);
  } catch (...) {
    g_rime->free_status(&status);
    return FAMO_ENGINE_E_RUNTIME;
  }
  if (status.is_ascii_mode)
    snapshot.status_flags |= FAMO_STATUS_ASCII_MODE;
  if (status.is_composing)
    snapshot.status_flags |= FAMO_STATUS_COMPOSING;
  if (status.is_disabled)
    snapshot.status_flags |= FAMO_STATUS_DISABLED;
  if (status.is_full_shape)
    snapshot.status_flags |= FAMO_STATUS_FULL_SHAPE;
  if (status.is_ascii_punct)
    snapshot.status_flags |= FAMO_STATUS_ASCII_PUNCT;
  const bool traditional =
      g_rime->get_option(session, "traditionalization") ||
      g_rime->get_option(session, "zh_trad");
  if (!traditional)
    snapshot.status_flags |= FAMO_STATUS_SIMPLIFIED;
  g_rime->free_status(&status);

  if (consume_commit && engine_context->has_pending_rime_commit) {
    try {
      if (engine_context->pending_rime_commit.text &&
          !AssignBoundedString(engine_context->pending_rime_commit.text,
                               &snapshot.commit)) {
        return FAMO_ENGINE_E_RUNTIME;
      }
    } catch (...) {
      return FAMO_ENGINE_E_RUNTIME;
    }
  }
  if (!snapshot.preedit.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (!snapshot.commit.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_COMMIT;
  if (!snapshot.candidates.empty()) {
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
    snapshot.page_size =
        static_cast<uint32_t>(snapshot.candidates.size());
  }
  if (!famo_action_v2::TrimOptionalToResultBudget(&snapshot))
    return FAMO_ENGINE_E_RUNTIME;
  *out = std::move(snapshot);
  return FAMO_ENGINE_OK;
}

int32_t PeekSnapshot(RimeSessionId session, uint32_t index, uint32_t count,
                     famo_action_v2::Snapshot* out) {
  if (!out || count > FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES ||
      index > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  famo_action_v2::Snapshot snapshot;
  if (count == 0 ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_from_index) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_next) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_end)) {
    *out = std::move(snapshot);
    return FAMO_ENGINE_OK;
  }

  PreviewLabels labels;
  try {
    labels = ReadPreviewLabels(session);
  } catch (...) {
    // Candidate text remains usable when schema labels are unavailable.
    labels = {};
  }
  RimeCandidateListIterator iterator{};
  if (!g_rime->candidate_list_from_index(session, &iterator,
                                          static_cast<int>(index))) {
    *out = std::move(snapshot);
    return FAMO_ENGINE_OK;
  }
  try {
    while (snapshot.candidates.size() < count &&
           g_rime->candidate_list_next(&iterator)) {
      famo_action_v2::CandidateSnapshot candidate;
      if ((iterator.candidate.text &&
           !AssignBoundedString(iterator.candidate.text,
                                &candidate.text)) ||
          (iterator.candidate.comment &&
           !AssignBoundedString(iterator.candidate.comment,
                                &candidate.comment))) {
        snapshot.candidates.clear();
        break;
      }
      candidate.label = labels.ForAbsoluteIndex(
          static_cast<uint64_t>(index) +
          snapshot.candidates.size());
      snapshot.candidates.push_back(std::move(candidate));
    }
  } catch (...) {
    g_rime->candidate_list_end(&iterator);
    return FAMO_ENGINE_E_RUNTIME;
  }
  g_rime->candidate_list_end(&iterator);
  if (!snapshot.candidates.empty())
    snapshot.state_flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  if (!famo_action_v2::TrimOptionalToResultBudget(&snapshot))
    return FAMO_ENGINE_E_RUNTIME;
  *out = std::move(snapshot);
  return FAMO_ENGINE_OK;
}

}  // namespace

namespace {

int32_t FAMO_ENGINE_CALL ReGetInfo(FamoEngineInfo* out_info) {
  if (!out_info || out_info->size < FAMO_ENGINE_INFO_REQUIRED_SIZE)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  out_info->size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  out_info->abi_version = FAMO_ENGINE_ABI_VERSION;
  // Conservative/honest: this build reliably supports deploy + userdb sync.
  // Lua/OpenCC depend on the librime build and are left unadvertised for MVP.
  out_info->capabilities = FAMO_ENGINE_CAP_SCHEMA_DEPLOY | FAMO_ENGINE_CAP_USERDB_SYNC;
  out_info->engine_name = Static("FamoRimeEngine");
  out_info->engine_version = Static("1.0.0");
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReGetInfoV2(FamoEngineInfo* out_info) {
  const int32_t result = ReGetInfo(out_info);
  if (result == FAMO_ENGINE_OK)
    out_info->abi_version = FAMO_ENGINE_ABI_V2;
  return result;
}

int32_t ReInitializeForAbi(const FamoEngineHostApi* host,
                           const FamoUtf8String* data_root,
                           uint32_t expected_abi_version) {
  if (!host ||
      host->size < static_cast<uint32_t>(FAMO_ENGINE_HOST_API_REQUIRED_SIZE) ||
      host->abi_version != expected_abi_version || !host->alloc ||
      !host->free) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  g_host = *host;
  g_rime = rime_get_api();
  if (!g_rime) {
    std::memset(&g_host, 0, sizeof(g_host));
    return FAMO_ENGINE_E_RUNTIME;
  }

  const std::string root = AsStd(data_root);

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = root.c_str();
  traits.user_data_dir = root.c_str();
  traits.distribution_name = "Famo";
  traits.distribution_code_name = "FamoRimeEngine";
  traits.distribution_version = "1.0.0";
  traits.app_name = "rime.famo";
  traits.min_log_level = 2;  // ERROR
  traits.log_dir = "";       // stderr only

  g_rime->setup(&traits);  // copies traits internally
  {
    std::lock_guard<std::mutex> registration_lock(
        g_notification_registration_mutex);
    bool install_handler = false;
    {
      std::lock_guard<std::mutex> lock(g_notification_mutex);
      install_handler = g_notification_handler != nullptr;
    }
    if (install_handler) {
      {
        std::lock_guard<std::mutex> lifetime_lock(
            g_callback_lifetime_mutex);
        g_callback_gate_closed = false;
      }
      // Never hold g_notification_mutex across this external call: librime is
      // allowed to report an initialization event synchronously.
      g_rime->set_notification_handler(&ReOnNotifyV2, nullptr);
    }
  }
  // Mark the initialize attempt before entering librime so the no-unwind
  // adapter finalizes even if an exceptional failure interrupts the call.
  g_rime_initialized = true;
  g_rime->initialize(&traits);  // pointers valid for the call duration
  g_data_root = root;
  g_initialized_abi_version = expected_abi_version;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReInitialize(const FamoEngineHostApi* host,
                                      const FamoUtf8String* data_root) {
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  return ReInitializeForAbi(host, data_root, FAMO_ENGINE_ABI_VERSION);
}

int32_t FAMO_ENGINE_CALL ReInitializeV2(const FamoEngineHostApi* host,
                                        const FamoUtf8String* data_root) {
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  const int32_t result =
      ReInitializeForAbi(host, data_root, FAMO_ENGINE_ABI_V2);
  if (result != FAMO_ENGINE_OK)
    return result;
  if (!g_v2_results.Start()) {
    ResetRimeState();
    return FAMO_ENGINE_E_RUNTIME;
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReShutdown(void) {
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  ResetRimeState();
  return FAMO_ENGINE_OK;
}

bool RotateEmptyRimeEpoch() noexcept {
  // ResetRimeState waits for every active callback. A host notification that
  // re-enters create_context must never try to rotate the epoch from inside
  // the callback it would then wait for.
  if (g_callback_depth != 0)
    return false;
  bool reset_started = false;
  try {
    if (!g_v2_results.Empty())
      return false;
    {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      if (!g_contexts.empty())
        return false;
    }
    if (TestSwitch("FAMO_TEST_RIME_NOTIFICATION_DURING_ROTATION")) {
      // Model a librime worker that is already inside the host callback while
      // the creator owns g_context_creation_mutex and is about to drain
      // callbacks. Its re-entrant create must fail before touching that mutex.
      std::thread notifier(
          [] { RouteNotification(0, "deploy", "start"); });
      notifier.join();
    }

    const FamoEngineHostApi host = g_host;
    const std::string root = g_data_root;
    const uint32_t abi_version = g_initialized_abi_version;
    FamoEngineNotificationHandlerV2 handler = nullptr;
    void* user_data = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_notification_mutex);
      handler = g_notification_handler;
      user_data = g_notification_user_data;
    }
    if (TestSwitch("FAMO_TEST_RIME_ROTATION_PREFLIGHT_FAILURE"))
      throw std::bad_alloc();

    reset_started = true;
    ResetRimeState();
    if (handler) {
      std::lock_guard<std::mutex> lock(g_notification_mutex);
      g_notification_handler = handler;
      g_notification_user_data = user_data;
      g_notification_ever_enabled = true;
    }
    const FamoUtf8String root_view = Borrowed(root);
    const int32_t result =
        abi_version == FAMO_ENGINE_ABI_V2
            ? ReInitializeV2(&host, &root_view)
            : ReInitialize(&host, &root_view);
    if (result != FAMO_ENGINE_OK) {
      ResetRimeState();
      return false;
    }
    return true;
  } catch (...) {
    if (reset_started) {
      // A partial reinitialization must not leave librime, the host allocator,
      // or notification registration in an indeterminate mixed epoch.
      ResetRimeState();
    }
    // A preflight copy/allocation failure occurs before the old epoch is
    // touched, so it remains runnable for a later bounded retry.
    return false;
  }
}

bool RetireContextBindingAndWait(FamoEngineContext* context) noexcept {
  if (!context)
    return false;
  try {
    const RimeSessionId session = context->session;
    std::unique_lock<std::mutex> lock(g_context_mutex);
    auto found = g_contexts.find(session);
    if (found == g_contexts.end() ||
        found->second.context != context ||
        found->second.incarnation != context->incarnation) {
      return true;
    }
    found->second.retiring = true;
    g_context_lifetime_cv.wait(lock, [&] {
      const auto current = g_contexts.find(session);
      return current == g_contexts.end() ||
             current->second.context != context ||
             current->second.incarnation != context->incarnation ||
             current->second.active_notifications == 0;
    });
    found = g_contexts.find(session);
    if (found != g_contexts.end() &&
        found->second.context == context &&
        found->second.incarnation == context->incarnation) {
      g_contexts.erase(found);
    }
    return true;
  } catch (...) {
    return false;
  }
}

int32_t DestroyRimeContext(FamoEngineContext* context) {
  if (!context)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  // RetireContextBindingAndWait includes the current callback in
  // active_notifications. A callback must not wait for itself, or acquire the
  // creation mutex while another thread owns it and drains that callback.
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  std::lock_guard<std::mutex> creation_lock(g_context_creation_mutex);
  if (!RetireContextBindingAndWait(context))
    return FAMO_ENGINE_E_RUNTIME;
  const RimeSessionId session = context->session;
  ReleasePendingCommit(context);
  if (!NotificationEverEnabled() && g_rime &&
      g_rime->destroy_session(session)) {
    try {
      std::lock_guard<std::mutex> context_lock(g_context_mutex);
      g_seen_session_ids.erase(session);
    } catch (...) {
      // The session itself is already gone. A stale seen id only fails closed
      // until this engine epoch is unloaded.
    }
  }
  // If notifications were ever exposed, do not destroy the librime session
  // here. Its callback id has no incarnation, so recycling it could relabel a
  // late notification as a new context. ResetRimeState destroys every bounded
  // quarantined session once notifications are disabled and drained.
  delete context;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReCreateContext(const FamoUtf8String* schema_id,
                                         FamoEngineContext** out_context) {
  if (!out_context) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_context = nullptr;
  // A callback-reentrant create must never wait on the creation mutex: another
  // thread may own it while rotating and be waiting for this callback to exit.
  // The same guard also prevents a synchronous initialize notification from
  // recursively locking the non-recursive creation mutex.
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  if (!g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string schema = AsStd(schema_id);
  if (!schema.empty()) {
    RimeConfig config{};
    if (!g_rime->schema_open(schema.c_str(), &config))
      return FAMO_ENGINE_E_SCHEMA;
    const char* configured_id =
        g_rime->config_get_cstring(&config, "schema/schema_id");
    const bool valid = configured_id && schema == configured_id;
    const Bool closed = g_rime->config_close(&config);
    if (!valid)
      return FAMO_ENGINE_E_SCHEMA;
    if (!closed)
      return FAMO_ENGINE_E_RUNTIME;
  }
  RimeSessionId session = 0;
  {
    // Serialize only allocation + quarantine registration. Holding
    // g_context_mutex across create_session would deadlock if librime notifies
    // synchronously, while holding this mutex through publication could
    // deadlock a re-entrant host notification handler.
    std::lock_guard<std::mutex> creation_lock(g_context_creation_mutex);
    const size_t limit = SeenSessionLimit();
    bool rotate_epoch = false;
    {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      if (g_session_registry_poisoned ||
          g_seen_session_ids.size() >= limit) {
        if (!g_contexts.empty())
          return FAMO_ENGINE_E_RUNTIME;
        rotate_epoch = true;
      }
    }
    if (rotate_epoch) {
      if (!RotateEmptyRimeEpoch())
        return FAMO_ENGINE_E_RUNTIME;
    }
    session = g_rime->create_session();
    if (!session)
      return FAMO_ENGINE_E_RUNTIME;

    bool reserved = false;
    bool duplicate = false;
    try {
      std::lock_guard<std::mutex> lock(g_context_mutex);
      if (g_session_registry_poisoned ||
          g_seen_session_ids.size() >= limit) {
        g_session_registry_poisoned = true;
      } else {
        const auto inserted = g_seen_session_ids.insert(session);
        reserved = inserted.second;
        duplicate = !inserted.second;
        if (duplicate)
          g_session_registry_poisoned = true;
      }
    } catch (...) {
      try {
        std::lock_guard<std::mutex> lock(g_context_mutex);
        g_session_registry_poisoned = true;
      } catch (...) {
      }
    }
    if (!reserved) {
      // A duplicate id may name an existing quarantined session; destroying
      // it would corrupt that incarnation. For a unique-but-unregistrable id,
      // poison future creates before destroying it so it can never be rebound.
      if (!duplicate)
        g_rime->destroy_session(session);
      return FAMO_ENGINE_E_RUNTIME;
    }
  }
  auto* ctx =
      TestSwitch("FAMO_TEST_RIME_CONTEXT_ALLOCATION_FAILURE")
          ? nullptr
          : new (std::nothrow) FamoEngineContext();
  if (!ctx) {
    ReleaseUnpublishedSession(session);
    return FAMO_ENGINE_E_RUNTIME;
  }
  ctx->session = session;
  uint64_t incarnation = 0;
  bool inserted = false;
  try {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    if (g_session_registry_poisoned ||
        g_seen_session_ids.find(session) == g_seen_session_ids.end() ||
        g_contexts.find(session) != g_contexts.end() ||
        g_next_context_incarnation == 0 ||
        g_next_context_incarnation == UINT64_MAX) {
      inserted = false;
    } else {
      incarnation = g_next_context_incarnation++;
      ContextBinding binding;
      binding.context = ctx;
      binding.incarnation = incarnation;
      inserted = g_contexts.emplace(session, std::move(binding)).second;
    }
  } catch (...) {
    delete ctx;
    ReleaseUnpublishedSession(session);
    return FAMO_ENGINE_E_RUNTIME;
  }
  if (!inserted) {
    delete ctx;
    ReleaseUnpublishedSession(session);
    return FAMO_ENGINE_E_RUNTIME;
  }
  ctx->incarnation = incarnation;

  // Deterministic concurrency seam: model a librime worker notification that
  // already borrowed the unpublished context when a later create step fails.
  // Cleanup must mark the binding retiring and wait for that borrow.
  if (TestSwitch("FAMO_TEST_CREATE_FAILURE_AFTER_NOTIFICATION")) {
    NotificationBorrowSignal signal;
    std::thread notifier;
    std::thread releaser;
    try {
      notifier = std::thread([&] {
        RouteNotification(static_cast<uintptr_t>(session), "option",
                          "ascii_mode", &signal);
      });
      {
        std::unique_lock<std::mutex> lock(signal.mutex);
        signal.condition.wait(lock, [&] { return signal.entered; });
      }
      releaser = std::thread([&] {
        ::Sleep(150);
        {
          std::lock_guard<std::mutex> lock(signal.mutex);
          signal.release = true;
        }
        signal.condition.notify_all();
      });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(signal.mutex);
        signal.release = true;
      }
      signal.condition.notify_all();
      if (notifier.joinable())
        notifier.join();
      if (releaser.joinable())
        releaser.join();
      (void)DestroyRimeContext(ctx);
      return FAMO_ENGINE_E_RUNTIME;
    }
    (void)DestroyRimeContext(ctx);
    notifier.join();
    releaser.join();
    return FAMO_ENGINE_E_RUNTIME;
  }

  // Map the opaque context before select_schema/set_option can synchronously
  // notify, so the host always receives the exact source context and label.
  if (!schema.empty() &&
      !g_rime->select_schema(session, schema.c_str())) {
    (void)DestroyRimeContext(ctx);
    return FAMO_ENGINE_E_RUNTIME;
  }
  try {
    // Publish only after every fallible create step has succeeded. Flush in
    // batches outside g_context_mutex; callbacks that arrive during a batch
    // still see published=false and join the next batch, preserving order
    // without invoking the external host while holding the registry lock.
    bool published = false;
    while (!published) {
      std::vector<PendingNotification> pending;
      {
        std::lock_guard<std::mutex> lock(g_context_mutex);
        const auto found = g_contexts.find(session);
        if (found == g_contexts.end() ||
            found->second.context != ctx ||
            found->second.incarnation != ctx->incarnation ||
            found->second.retiring) {
          break;
        }
        if (found->second.pending.empty()) {
          found->second.published = true;
          published = true;
        } else {
          pending.swap(found->second.pending);
        }
      }
      for (const auto& notification : pending)
        DeliverNotification(ctx, notification);
    }
    if (!published) {
      // Defensive fail-safe: if the binding changed unexpectedly, never leave
      // a map entry borrowing the context that is about to be destroyed.
      (void)DestroyRimeContext(ctx);
      return FAMO_ENGINE_E_RUNTIME;
    }
  } catch (...) {
    (void)DestroyRimeContext(ctx);
    return FAMO_ENGINE_E_RUNTIME;
  }
  *out_context = ctx;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReDestroyContext(FamoEngineContext* context) {
  return DestroyRimeContext(context);
}

int32_t FAMO_ENGINE_CALL ReProcessKey(FamoEngineContext* context, const FamoKeyEvent* key,
                                      FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !key || !g_rime ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  int keycode = 0, mask = 0;
  bool handled = false;
  if (famo_rime_keys::FamoKeyToRimeV1(*key, &keycode, &mask))
    handled = g_rime->process_key(context->session, keycode, mask);
  const int32_t rc =
      FillFromSession(context->session, out_view, view_size,
                      /*consume_commit=*/true);
  if (rc == FAMO_ENGINE_OK && handled)
    out_view->state_flags |= FAMO_COMPOSITION_HANDLED;
  return rc;
}

int32_t FAMO_ENGINE_CALL ReSelectCandidate(FamoEngineContext* context, uint32_t index,
                                           FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !g_rime ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  const bool handled = g_rime->select_candidate_on_current_page(
      context->session, static_cast<size_t>(index));
  // consume_commit=false, legacy-faithful: the reroute's SelectCandidateOnCurrentPage does
  // NOT _Respond (mirrors stock weasel — the IPC On-handler passes no eat). The commit the
  // selection produces is delivered by the FOLLOWING simulated VK_SELECT key
  // (CandidateList.cpp _SelectCandidateOnCurrentPage) via ProcessKeyEvent→_Respond→get_commit.
  // Consuming it here strands it in a view that SelectCandidateOnCurrentPage discards →
  // mouse-clicking a candidate drops the committed text under abi (byte-parity harness caught this).
  const int32_t rc =
      FillFromSession(context->session, out_view, view_size,
                      /*consume_commit=*/false);
  if (rc == FAMO_ENGINE_OK && handled)
    out_view->state_flags |= FAMO_COMPOSITION_HANDLED;
  return rc;
}

int32_t FAMO_ENGINE_CALL ReSetOption(FamoEngineContext* context, const FamoUtf8String* name,
                                     int32_t value) {
  if (!context || !name || !name->data || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (context->recovery_required)
    return FAMO_ENGINE_E_RECOVERY_REQUIRED;
  const std::string opt = AsStd(name);
  g_rime->set_option(context->session, opt.c_str(), value ? True : False);
  if (value && (opt == "traditionalization" || opt == "zh_trad")) {
    // Load OpenCC off the 50 ms TSF key path without disturbing active input.
    const char* input = g_rime->get_input(context->session);
    if (!input || !*input) {
      g_rime->process_key(context->session, 'a', 0);
      g_rime->clear_composition(context->session);
    }
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReDeploySchema(const FamoUtf8String* /*schema_id*/,
                                        FamoUtf8String* /*out_error_message*/) {
  if (!g_rime) return FAMO_ENGINE_E_RUNTIME;
  {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    for (const auto& entry : g_contexts) {
      if (entry.second.context &&
          entry.second.context->recovery_required) {
        return FAMO_ENGINE_E_RECOVERY_REQUIRED;
      }
    }
  }
  g_rime->deployer_initialize(nullptr);
  return g_rime->deploy() ? FAMO_ENGINE_OK : FAMO_ENGINE_E_RUNTIME;
}

int32_t FAMO_ENGINE_CALL ReFreeView(FamoCompositionView* view) {
  uint32_t view_size = 0;
  if (!famo_view_abi::Negotiate(view, &view_size))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  ReleaseViewStorage(view, view_size);
  famo_view_abi::ClearPreservingSize(view, view_size);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReGetStatus(FamoEngineContext* context,
                                     FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !g_rime ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  return FillFromSession(context->session, out_view, view_size,
                         /*consume_commit=*/false);
}

int32_t FAMO_ENGINE_CALL ReGetOption(FamoEngineContext* context,
                                     const FamoUtf8String* name, int32_t* out_value) {
  if (!context || !name || !name->data || !out_value || !g_rime)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const std::string opt = AsStd(name);
  *out_value = g_rime->get_option(context->session, opt.c_str()) ? 1 : 0;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReSetPropertyV2(
    FamoEngineContext* context,
    const FamoUtf8String* name,
    const FamoUtf8String* value) {
  if (!context || !name || !value || !g_rime)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  if (context->recovery_required)
    return FAMO_ENGINE_E_RECOVERY_REQUIRED;
  const std::string property_name = AsStd(name);
  const std::string property_value = AsStd(value);
  g_rime->set_property(context->session, property_name.c_str(),
                       property_value.c_str());
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReSetNotificationHandlerV2(
    FamoEngineNotificationHandlerV2 handler,
    void* user_data) {
  if (!handler && user_data)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  // DisableNotifications waits for g_active_callbacks and handler replacement
  // shares the creation/registration locks with destroy/rotation. Neither
  // operation may run synchronously from the callback being drained.
  if (g_callback_depth != 0)
    return FAMO_ENGINE_E_RUNTIME;
  if (!handler) {
    DisableNotifications();
    return FAMO_ENGINE_OK;
  }
  {
    // This is the false->true epoch transition read by create/destroy cleanup.
    // Keep the same creation->notification lock order as context destruction so
    // an ID cannot be destroyed/recycled while a handler is being enabled.
    std::lock_guard<std::mutex> creation_lock(g_context_creation_mutex);
    PauseNotificationEnableForTest();
    std::lock_guard<std::mutex> lock(g_notification_mutex);
    g_notification_handler = handler;
    g_notification_user_data = user_data;
    g_notification_ever_enabled = true;
  }
  if (!g_rime)
    return FAMO_ENGINE_OK;  // Host registers before initialize.
  try {
    std::lock_guard<std::mutex> registration_lock(
        g_notification_registration_mutex);
    {
      std::lock_guard<std::mutex> lock(g_callback_lifetime_mutex);
      g_callback_gate_closed = false;
    }
    if (g_rime)
      g_rime->set_notification_handler(&ReOnNotifyV2, nullptr);
  } catch (...) {
    DisableNotifications();
    throw;
  }
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReCommitComposition(FamoEngineContext* context) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->commit_composition(context->session);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReClearComposition(FamoEngineContext* context) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->clear_composition(context->session);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReHighlightCandidate(FamoEngineContext* context, uint32_t index) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->highlight_candidate_on_current_page(context->session, static_cast<size_t>(index));
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReChangePage(FamoEngineContext* context, int32_t backward) {
  if (!context || !g_rime) return FAMO_ENGINE_E_INVALID_ARGUMENT;
  g_rime->change_page(context->session, backward ? True : False);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL RePeekCandidates(FamoEngineContext* context,
                                          uint32_t index, uint32_t count,
                                          FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !g_rime || count > 64 ||
      index > static_cast<uint32_t>((std::numeric_limits<int>::max)()) ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  FamoCompositionView result;
  famo_view_abi::BeginResult(&result, view_size);
  if (count == 0 || !RIME_API_AVAILABLE(g_rime, candidate_list_from_index) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_next) ||
      !RIME_API_AVAILABLE(g_rime, candidate_list_end)) {
    famo_view_abi::Publish(out_view, result, view_size);
    return FAMO_ENGINE_OK;
  }

  PreviewLabels labels;
  try {
    labels = ReadPreviewLabels(context->session);
  } catch (...) {
    labels = {};
  }
  RimeCandidateListIterator iterator{};
  if (!g_rime->candidate_list_from_index(context->session, &iterator,
                                          static_cast<int>(index))) {
    famo_view_abi::Publish(out_view, result, view_size);
    return FAMO_ENGINE_OK;
  }
  const size_t stride = famo_view_abi::CandidateStride(view_size);
  if (!g_host.alloc) {
    g_rime->candidate_list_end(&iterator);
    return FAMO_ENGINE_E_RUNTIME;
  }
  void* candidates =
      g_host.alloc(stride * static_cast<size_t>(count));
  if (!candidates) {
    g_rime->candidate_list_end(&iterator);
    return FAMO_ENGINE_E_RUNTIME;
  }
  std::memset(candidates, 0, stride * static_cast<size_t>(count));
  result.candidates = static_cast<const FamoCandidate*>(candidates);

  uint32_t size = 0;
  while (size < count && g_rime->candidate_list_next(&iterator)) {
    FamoCandidate* candidate =
        famo_view_abi::CandidateAt(candidates, size, stride);
    candidate->size = static_cast<uint32_t>(stride);
    result.candidate_count = size + 1;
    const char* text = iterator.candidate.text;
    const char* comment = iterator.candidate.comment;
    if ((text && *text &&
         !DuplicateCString(text, &candidate->text)) ||
        (comment && *comment &&
         !DuplicateCString(comment, &candidate->comment))) {
      g_rime->candidate_list_end(&iterator);
      ReleaseViewStorage(&result, view_size);
      return FAMO_ENGINE_E_RUNTIME;
    }
    if (famo_view_abi::HasV12Candidates(view_size)) {
      const std::string label = labels.ForAbsoluteIndex(
          static_cast<uint64_t>(index) + size);
      if (!label.empty() &&
          !DuplicateCString(label.c_str(), &candidate->label)) {
        g_rime->candidate_list_end(&iterator);
        ReleaseViewStorage(&result, view_size);
        return FAMO_ENGINE_E_RUNTIME;
      }
    }
    ++size;
  }
  g_rime->candidate_list_end(&iterator);
  if (size == 0) {
    ReleaseViewStorage(&result, view_size);
    famo_view_abi::Publish(out_view, result, view_size);
    return FAMO_ENGINE_OK;
  }
  result.candidate_count = size;
  famo_view_abi::Publish(out_view, result, view_size);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReSelectCandidateAbsolute(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) {
  uint32_t view_size = 0;
  if (!context || !g_rime ||
      !famo_view_abi::Negotiate(out_view, &view_size)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  const bool handled =
      g_rime->select_candidate(context->session, static_cast<size_t>(index));
  const int32_t rc =
      FillFromSession(context->session, out_view, view_size,
                      /*consume_commit=*/false);
  if (rc == FAMO_ENGINE_OK && handled)
    out_view->state_flags |= FAMO_COMPOSITION_HANDLED;
  return rc;
}

void MarkRecovery(FamoEngineContext* context,
                  const FamoEngineActionRequestV2& request,
                  bool handled,
                  bool consume_commit,
                  famo_action_v2::Snapshot* completed_snapshot) noexcept {
  context->recovery_action = request.action;
  context->recovery_handled = handled;
  context->recovery_consume_commit = consume_commit;
  context->recovery_required = true;
  if (completed_snapshot) {
    context->pending_snapshot = std::move(*completed_snapshot);
    context->has_pending_snapshot = true;
    ReleasePendingCommit(context);
  }
}

void ClearRecovery(FamoEngineContext* context) noexcept {
  context->recovery_required = false;
  context->recovery_action = 0;
  context->recovery_handled = false;
  context->recovery_consume_commit = false;
  context->has_pending_snapshot = false;
  context->pending_snapshot = famo_action_v2::Snapshot{};
  ReleasePendingCommit(context);
}

int32_t RecoverActionV2(
    FamoEngineContext* context,
    const FamoEngineActionRequestV2& recovery_request,
    FamoEngineActionResultV2** out_result) {
  const uint32_t original_action =
      static_cast<uint32_t>(recovery_request.value);
  if (!context->recovery_required ||
      original_action != context->recovery_action) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }

  if (!context->has_pending_snapshot) {
    famo_action_v2::Snapshot snapshot;
    const int32_t snapshot_result =
        SnapshotFromSession(context, context->recovery_consume_commit,
                            &snapshot);
    if (snapshot_result != FAMO_ENGINE_OK)
      return snapshot_result;
    context->pending_snapshot = std::move(snapshot);
    context->has_pending_snapshot = true;
    ReleasePendingCommit(context);
  }

  FamoEngineActionRequestV2 original_request = recovery_request;
  original_request.action = original_action;
  original_request.value = 0;
  const int32_t publish_result = g_v2_results.Publish(
      g_host, original_request, context->recovery_handled,
      context->pending_snapshot, out_result);
  if (publish_result != FAMO_ENGINE_OK)
    return publish_result;
  ClearRecovery(context);
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReExecuteActionV2(
    FamoEngineContext* context, const FamoEngineActionRequestV2* request,
    FamoEngineActionResultV2** out_result) {
  if (!out_result)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  *out_result = nullptr;
  if (!context || !g_rime)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const int32_t validation = famo_action_v2::ValidateRequest(request);
  if (validation != FAMO_ENGINE_OK)
    return validation;

  if (request->action == FAMO_ENGINE_ACTION_RECOVER)
    return RecoverActionV2(context, *request, out_result);

  if (context->recovery_required &&
      famo_action_v2::IsBusinessAction(request->action)) {
    return FAMO_ENGINE_E_RECOVERY_REQUIRED;
  }

  if (request->action == FAMO_ENGINE_ACTION_STATUS) {
    famo_action_v2::Snapshot status;
    const int32_t snapshot_result =
        SnapshotFromSession(context, false, &status);
    if (snapshot_result != FAMO_ENGINE_OK)
      return snapshot_result;
    return g_v2_results.Publish(g_host, *request, false, status, out_result);
  }
  if (request->action == FAMO_ENGINE_ACTION_PEEK_CANDIDATES) {
    famo_action_v2::Snapshot peek;
    const int32_t peek_result =
        PeekSnapshot(context->session, request->index, request->count, &peek);
    if (peek_result != FAMO_ENGINE_OK)
      return peek_result;
    return g_v2_results.Publish(g_host, *request, false, peek, out_result);
  }

  FamoEngineActionResultV2* emergency = nullptr;
  const int32_t emergency_result =
      g_v2_results.PrepareEmergency(g_host, *request, &emergency);
  if (emergency_result != FAMO_ENGINE_OK)
    return emergency_result;

  bool handled = false;
  bool consume_commit = false;
  switch (request->action) {
    case FAMO_ENGINE_ACTION_PROCESS_KEY: {
      if (request->key.size < FAMO_KEY_EVENT_REQUIRED_SIZE)
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      int keycode = 0;
      int mask = 0;
      if (famo_rime_keys::FamoKeyToRimeV2(request->key, &keycode, &mask)) {
        handled =
            g_rime->process_key(context->session, keycode, mask) != False;
      }
      consume_commit = true;
      break;
    }
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE:
      handled = g_rime->select_candidate_on_current_page(
                    context->session, static_cast<size_t>(request->index)) !=
                False;
      consume_commit = true;
      break;
    case FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE:
      handled =
          g_rime->select_candidate(context->session,
                                   static_cast<size_t>(request->index)) !=
          False;
      consume_commit = true;
      break;
    case FAMO_ENGINE_ACTION_COMMIT_COMPOSITION:
      handled =
          g_rime->commit_composition(context->session) != False;
      consume_commit = true;
      break;
    case FAMO_ENGINE_ACTION_CLEAR_COMPOSITION:
      g_rime->clear_composition(context->session);
      handled = true;
      break;
    case FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE:
      handled = g_rime->highlight_candidate_on_current_page(
                    context->session, static_cast<size_t>(request->index)) !=
                False;
      break;
    case FAMO_ENGINE_ACTION_CHANGE_PAGE:
      if (request->value != 0 && request->value != 1)
        return FAMO_ENGINE_E_INVALID_ARGUMENT;
      handled =
          g_rime->change_page(context->session,
                              request->value ? True : False) != False;
      break;
    default:
      (void)g_v2_results.Free(g_host, emergency);
      return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  famo_action_v2::Snapshot snapshot;
  const int32_t snapshot_result =
      SnapshotFromSession(context, consume_commit, &snapshot);
  if (snapshot_result != FAMO_ENGINE_OK) {
    MarkRecovery(context, *request, handled, consume_commit, nullptr);
    emergency->handled = handled ? 1u : 0u;
    *out_result = emergency;
    return FAMO_ENGINE_OK;
  }

  FamoEngineActionResultV2* completed = nullptr;
  const int32_t publish_result =
      g_v2_results.Publish(g_host, *request, handled, snapshot, &completed);
  if (publish_result != FAMO_ENGINE_OK) {
    MarkRecovery(context, *request, handled, consume_commit, &snapshot);
    emergency->handled = handled ? 1u : 0u;
    *out_result = emergency;
    return FAMO_ENGINE_OK;
  }

  ReleasePendingCommit(context);
  (void)g_v2_results.Free(g_host, emergency);
  *out_result = completed;
  return FAMO_ENGINE_OK;
}

int32_t FAMO_ENGINE_CALL ReFreeResultV2(
    FamoEngineActionResultV2* result) {
  return g_v2_results.Free(g_host, result);
}

template <typename Callback>
int32_t ReLegacyNoUnwind(Callback callback) noexcept {
  try {
    return callback();
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReGetInfoSafe(
    FamoEngineInfo* out_info) noexcept {
  return ReLegacyNoUnwind([&] { return ReGetInfo(out_info); });
}

int32_t FAMO_ENGINE_CALL ReInitializeSafe(
    const FamoEngineHostApi* host,
    const FamoUtf8String* data_root) noexcept {
  if (!famo_action_v2::ValidInputString(data_root))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const int32_t result = ReLegacyNoUnwind(
      [&] { return ReInitialize(host, data_root); });
  // ReInitialize rejects callback re-entry before touching engine state.
  // ResetRimeState would wait for the callback currently executing this
  // wrapper and therefore deadlock the callback thread against itself.
  if (result != FAMO_ENGINE_OK && g_callback_depth == 0)
    ResetRimeState();
  return result;
}

int32_t FAMO_ENGINE_CALL ReShutdownSafe() noexcept {
  const int32_t result = ReLegacyNoUnwind([] { return ReShutdown(); });
  // Preserve the same fail-fast callback boundary as ReShutdown. Cleanup from
  // inside that callback would synchronously wait for itself to leave.
  if (result != FAMO_ENGINE_OK && g_callback_depth == 0)
    ResetRimeState();
  return result;
}

int32_t FAMO_ENGINE_CALL ReCreateContextSafe(
    const FamoUtf8String* schema_id,
    FamoEngineContext** out_context) noexcept {
  if (out_context)
    *out_context = nullptr;
  if (!famo_action_v2::ValidInputString(schema_id, true))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return ReLegacyNoUnwind(
      [&] { return ReCreateContext(schema_id, out_context); });
}

int32_t FAMO_ENGINE_CALL ReDestroyContextSafe(
    FamoEngineContext* context) noexcept {
  return ReLegacyNoUnwind([&] { return ReDestroyContext(context); });
}

int32_t FAMO_ENGINE_CALL ReProcessKeySafe(
    FamoEngineContext* context, const FamoKeyEvent* key,
    FamoCompositionView* out_view) noexcept {
  if (!key || key->size < FAMO_KEY_EVENT_REQUIRED_SIZE)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return ReLegacyNoUnwind(
      [&] { return ReProcessKey(context, key, out_view); });
}

int32_t FAMO_ENGINE_CALL ReSelectCandidateSafe(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReSelectCandidate(context, index, out_view); });
}

int32_t FAMO_ENGINE_CALL ReSetOptionSafe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t value) noexcept {
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return ReLegacyNoUnwind(
      [&] { return ReSetOption(context, name, value); });
}

int32_t FAMO_ENGINE_CALL ReDeploySchemaSafe(
    const FamoUtf8String* schema_id,
    FamoUtf8String* out_error_message) noexcept {
  if (!famo_action_v2::ValidInputString(schema_id))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return ReLegacyNoUnwind(
      [&] { return ReDeploySchema(schema_id, out_error_message); });
}

int32_t FAMO_ENGINE_CALL ReFreeViewSafe(
    FamoCompositionView* view) noexcept {
  return ReLegacyNoUnwind([&] { return ReFreeView(view); });
}

int32_t FAMO_ENGINE_CALL ReGetStatusSafe(
    FamoEngineContext* context, FamoCompositionView* out_view) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReGetStatus(context, out_view); });
}

int32_t FAMO_ENGINE_CALL ReGetOptionSafe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t* out_value) noexcept {
  if (out_value)
    *out_value = 0;
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  return ReLegacyNoUnwind(
      [&] { return ReGetOption(context, name, out_value); });
}

int32_t FAMO_ENGINE_CALL ReCommitCompositionSafe(
    FamoEngineContext* context) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReCommitComposition(context); });
}

int32_t FAMO_ENGINE_CALL ReClearCompositionSafe(
    FamoEngineContext* context) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReClearComposition(context); });
}

int32_t FAMO_ENGINE_CALL ReHighlightCandidateSafe(
    FamoEngineContext* context, uint32_t index) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReHighlightCandidate(context, index); });
}

int32_t FAMO_ENGINE_CALL ReChangePageSafe(
    FamoEngineContext* context, int32_t backward) noexcept {
  return ReLegacyNoUnwind(
      [&] { return ReChangePage(context, backward); });
}

int32_t FAMO_ENGINE_CALL RePeekCandidatesSafe(
    FamoEngineContext* context, uint32_t index, uint32_t count,
    FamoCompositionView* out_view) noexcept {
  return ReLegacyNoUnwind(
      [&] { return RePeekCandidates(context, index, count, out_view); });
}

int32_t FAMO_ENGINE_CALL ReSelectCandidateAbsoluteSafe(
    FamoEngineContext* context, uint32_t index,
    FamoCompositionView* out_view) noexcept {
  return ReLegacyNoUnwind([&] {
    return ReSelectCandidateAbsolute(context, index, out_view);
  });
}

int32_t FAMO_ENGINE_CALL ReGetInfoV2Safe(
    FamoEngineInfo* out_info) noexcept {
  try {
    return ReGetInfoV2(out_info);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReInitializeV2Safe(
    const FamoEngineHostApi* host,
    const FamoUtf8String* data_root) noexcept {
  if (!famo_action_v2::ValidInputString(data_root))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return ReInitializeV2(host, data_root);
  } catch (...) {
    ResetRimeState();
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReShutdownV2Safe() noexcept {
  try {
    return ReShutdown();
  } catch (...) {
    ResetRimeState();
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReCreateContextV2Safe(
    const FamoUtf8String* schema_id,
    FamoEngineContext** out_context) noexcept {
  if (out_context)
    *out_context = nullptr;
  if (!famo_action_v2::ValidInputString(schema_id, true))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return ReCreateContext(schema_id, out_context);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReDestroyContextV2Safe(
    FamoEngineContext* context) noexcept {
  try {
    return ReDestroyContext(context);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReExecuteActionV2Safe(
    FamoEngineContext* context, const FamoEngineActionRequestV2* request,
    FamoEngineActionResultV2** out_result) noexcept {
  if (out_result)
    *out_result = nullptr;
  try {
    return ReExecuteActionV2(context, request, out_result);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReSetOptionV2Safe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t value) noexcept {
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return ReSetOption(context, name, value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReGetOptionV2Safe(
    FamoEngineContext* context, const FamoUtf8String* name,
    int32_t* out_value) noexcept {
  if (out_value)
    *out_value = 0;
  if (!famo_action_v2::ValidInputString(name))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return ReGetOption(context, name, out_value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReDeploySchemaV2Safe(
    const FamoUtf8String* schema_id) noexcept {
  if (!famo_action_v2::ValidInputString(schema_id))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  try {
    return ReDeploySchema(schema_id, nullptr);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReFreeResultV2Safe(
    FamoEngineActionResultV2* result) noexcept {
  try {
    return ReFreeResultV2(result);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReSetPropertyV2Safe(
    FamoEngineContext* context,
    const FamoUtf8String* name,
    const FamoUtf8String* value) noexcept {
  if (!famo_action_v2::ValidInputString(name) ||
      !famo_action_v2::ValidInputString(value)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }
  try {
    return ReSetPropertyV2(context, name, value);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

int32_t FAMO_ENGINE_CALL ReSetNotificationHandlerV2Safe(
    FamoEngineNotificationHandlerV2 handler,
    void* user_data) noexcept {
  try {
    return ReSetNotificationHandlerV2(handler, user_data);
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

}  // namespace

extern "C" FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL
FamoCreateEngineApi(uint32_t requested_abi_version, FamoEngineApi* out_api) {
  try {
  if (requested_abi_version != FAMO_ENGINE_ABI_VERSION)
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  if (!out_api)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const uint32_t caller_size = out_api->size;
  if (caller_size < static_cast<uint32_t>(offsetof(FamoEngineApi, get_status)))
    return FAMO_ENGINE_E_INVALID_ARGUMENT;

  FamoEngineApi api{};
  api.size = caller_size < static_cast<uint32_t>(sizeof(api))
                 ? caller_size
                 : static_cast<uint32_t>(sizeof(api));
  api.abi_version = FAMO_ENGINE_ABI_VERSION;
  api.get_info = &ReGetInfoSafe;
  api.initialize = &ReInitializeSafe;
  api.shutdown = &ReShutdownSafe;
  api.create_context = &ReCreateContextSafe;
  api.destroy_context = &ReDestroyContextSafe;
  api.process_key = &ReProcessKeySafe;
  api.select_candidate = &ReSelectCandidateSafe;
  api.set_option = &ReSetOptionSafe;
  api.deploy_schema = &ReDeploySchemaSafe;
  api.free_view = &ReFreeViewSafe;
  api.get_status = &ReGetStatusSafe;
  api.get_option = &ReGetOptionSafe;
  api.commit_composition = &ReCommitCompositionSafe;
  api.clear_composition = &ReClearCompositionSafe;
  api.highlight_candidate = &ReHighlightCandidateSafe;
  api.change_page = &ReChangePageSafe;
  api.peek_candidates = &RePeekCandidatesSafe;
  api.select_candidate_absolute = &ReSelectCandidateAbsoluteSafe;
  std::memcpy(out_api, &api, api.size);
  return FAMO_ENGINE_OK;
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}

extern "C" FAMO_ENGINE_EXPORT int32_t FAMO_ENGINE_CALL
FamoCreateEngineApiV2(uint32_t requested_abi_version,
                      FamoEngineApiV2* out_api) {
  try {
  if (requested_abi_version != FAMO_ENGINE_ABI_V2)
    return FAMO_ENGINE_E_UNSUPPORTED_ABI;
  if (!out_api)
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  const uint32_t caller_size = out_api->struct_size;
  if (caller_size <
      static_cast<uint32_t>(FAMO_ENGINE_API_V2_REQUIRED_SIZE)) {
    return FAMO_ENGINE_E_INVALID_ARGUMENT;
  }

  FamoEngineApiV2 api{};
  api.struct_size =
      caller_size < static_cast<uint32_t>(sizeof(api))
          ? caller_size
          : static_cast<uint32_t>(sizeof(api));
  api.abi_version = FAMO_ENGINE_ABI_V2;
  api.get_info = &ReGetInfoV2Safe;
  api.initialize = &ReInitializeV2Safe;
  api.shutdown = &ReShutdownV2Safe;
  api.create_context = &ReCreateContextV2Safe;
  api.destroy_context = &ReDestroyContextV2Safe;
  api.execute_action = &ReExecuteActionV2Safe;
  api.set_option = &ReSetOptionV2Safe;
  api.get_option = &ReGetOptionV2Safe;
  api.deploy_schema = &ReDeploySchemaV2Safe;
  api.free_result = &ReFreeResultV2Safe;
  api.set_property = &ReSetPropertyV2Safe;
  api.set_notification_handler = &ReSetNotificationHandlerV2Safe;
  std::memcpy(out_api, &api, api.struct_size);
  return FAMO_ENGINE_OK;
  } catch (...) {
    return FAMO_ENGINE_E_RUNTIME;
  }
}
