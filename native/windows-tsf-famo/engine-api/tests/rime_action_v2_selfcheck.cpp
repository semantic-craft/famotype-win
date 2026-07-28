// Real-librime public-interface canary for Famo engine ABI v2.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "../engines/rime/keymap.h"
#include "../famo_engine_api.h"
#include "../host/famo_engine_host.h"

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #condition,          \
                   __FILE__, __LINE__);                                       \
      return 1;                                                               \
    }                                                                         \
  } while (0)

namespace {

FamoUtf8String String(const std::string& value) {
  return {static_cast<uint32_t>(sizeof(FamoUtf8String)), value.data(),
          static_cast<uint32_t>(value.size())};
}

FamoUtf8String String(const char* value) {
  return {static_cast<uint32_t>(sizeof(FamoUtf8String)), value,
          static_cast<uint32_t>(std::strlen(value))};
}

FamoEngineActionRequestV2 Request(uint32_t action) {
  FamoEngineActionRequestV2 request{};
  request.struct_size = static_cast<uint32_t>(sizeof(request));
  request.action = action;
  request.view_layout_version = FAMO_COMPOSITION_LAYOUT_V2;
  request.candidate_layout_version = FAMO_CANDIDATE_LAYOUT_V2;
  request.candidate_stride =
      static_cast<uint32_t>(FAMO_CANDIDATE_V2_STRIDE);
  return request;
}

bool Equals(const FamoUtf8String& value, const char* expected) {
  const size_t length = std::strlen(expected);
  return value.data && value.length_bytes == length &&
         std::memcmp(value.data, expected, length) == 0;
}

bool Write(const std::filesystem::path& path, const char* contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  return output.good();
}

bool WriteFixture(const std::filesystem::path& root) {
  return Write(root / "default.yaml",
               "config_version: \"0.40\"\n"
               "schema_list:\n"
               "  - schema: famo_action_v2\n") &&
         Write(root / "famo_action_v2.schema.yaml",
               "schema:\n"
               "  schema_id: famo_action_v2\n"
               "  name: Famo Action V2\n"
               "  version: \"1.0\"\n"
               "switches:\n"
               "  - name: ascii_mode\n"
               "    reset: 0\n"
               "    states: [Chinese, ASCII]\n"
               "menu:\n"
               "  page_size: 2\n"
               "  alternative_select_keys: \"jk\"\n"
               "engine:\n"
               "  processors:\n"
               "    - speller\n"
               "    - selector\n"
               "    - navigator\n"
               "    - express_editor\n"
               "  segmentors:\n"
               "    - abc_segmentor\n"
               "    - fallback_segmentor\n"
               "  translators:\n"
               "    - table_translator\n"
               "speller:\n"
               "  alphabet: abcdefghijklmnopqrstuvwxyz\n"
               "translator:\n"
               "  dictionary: famo_action_v2\n"
               "  enable_completion: false\n"
               "  enable_sentence: false\n"
               "  enable_user_dict: false\n") &&
         Write(root / "famo_action_v2.dict.yaml",
               "---\n"
               "name: famo_action_v2\n"
               "version: \"1.0\"\n"
               "sort: by_weight\n"
               "columns:\n"
               "  - text\n"
               "  - code\n"
               "  - weight\n"
               "...\n"
               "\xE4\xBD\xA0\tni\t100\n"
               "\xE5\xB0\xBC\tni\t90\n"
               "\xE6\xB3\xA5\tni\t80\n");
}

struct NotificationCapture {
  std::mutex mutex;
  std::condition_variable condition;
  FamoEngineContext* context = nullptr;
  std::string label;
  uint32_t ascii_option_count = 0;
  bool block = false;
  bool entered = false;
  bool release = false;
  std::atomic_bool copy_failed{false};
};

void FAMO_ENGINE_CALL CaptureNotification(
    void* user_data,
    FamoEngineContext* context,
    const FamoUtf8String* type,
    const FamoUtf8String* value,
    const FamoUtf8String* label) noexcept {
  auto* capture = static_cast<NotificationCapture*>(user_data);
  if (!capture || !type || !value || !label)
    return;
  try {
    const std::string type_text(type->data ? type->data : "",
                                type->length_bytes);
    const std::string value_text(value->data ? value->data : "",
                                 value->length_bytes);
    if (type_text != "option" ||
        (value_text != "ascii_mode" && value_text != "!ascii_mode")) {
      return;
    }
    std::unique_lock<std::mutex> lock(capture->mutex);
    capture->context = context;
    capture->label.assign(label->data ? label->data : "",
                          label->length_bytes);
    ++capture->ascii_option_count;
    if (capture->block) {
      capture->entered = true;
      capture->condition.notify_all();
      capture->condition.wait(lock, [&] { return capture->release; });
    }
  } catch (...) {
    capture->copy_failed.store(true);
  }
}

struct ThrowingNotificationCapture {
  std::atomic_uint32_t calls{0};
};

void FAMO_ENGINE_CALL ThrowingNotification(
    void* user_data,
    FamoEngineContext* /*context*/,
    const FamoUtf8String* /*type*/,
    const FamoUtf8String* /*value*/,
    const FamoUtf8String* /*label*/) {
  auto* capture = static_cast<ThrowingNotificationCapture*>(user_data);
  if (capture)
    ++capture->calls;
  throw std::runtime_error("notification handler failure");
}

struct ReentrantNotificationCapture {
  FamoEngineHost* host = nullptr;
  FamoUtf8String schema{};
  std::atomic_bool armed{false};
  std::atomic_bool attempted{false};
  std::atomic<int32_t> result{FAMO_ENGINE_OK};
  FamoEngineContext* context = nullptr;
};

void FAMO_ENGINE_CALL ReentrantNotification(
    void* user_data,
    FamoEngineContext* context,
    const FamoUtf8String* /*type*/,
    const FamoUtf8String* /*value*/,
    const FamoUtf8String* /*label*/) noexcept {
  auto* capture =
      static_cast<ReentrantNotificationCapture*>(user_data);
  if (!capture || context || !capture->host ||
      !capture->armed.load() || capture->attempted.exchange(true)) {
    return;
  }
  FamoEngineContext* nested =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  capture->result.store(
      capture->host->v2_api().create_context(
          &capture->schema, &nested));
  capture->context = nested;
}

void FAMO_ENGINE_CALL ReplacementNotification(
    void* /*user_data*/,
    FamoEngineContext* /*context*/,
    const FamoUtf8String* /*type*/,
    const FamoUtf8String* /*value*/,
    const FamoUtf8String* /*label*/) noexcept {}

struct ReentrantLifecycleCapture {
  const FamoEngineApiV2* v2 = nullptr;
  const FamoEngineApi* legacy = nullptr;
  const FamoEngineHostApi* host = nullptr;
  FamoUtf8String root{};
  std::atomic_uint32_t calls{0};
  std::atomic_bool attempted{false};
  std::atomic<int32_t> destroy_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> shutdown_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> unregister_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> replace_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> initialize_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> legacy_initialize_result{FAMO_ENGINE_OK};
  std::atomic<int32_t> legacy_shutdown_result{FAMO_ENGINE_OK};
};

void FAMO_ENGINE_CALL ReentrantLifecycleNotification(
    void* user_data,
    FamoEngineContext* context,
    const FamoUtf8String* /*type*/,
    const FamoUtf8String* /*value*/,
    const FamoUtf8String* /*label*/) noexcept {
  auto* capture =
      static_cast<ReentrantLifecycleCapture*>(user_data);
  if (!capture || !context || !capture->v2 || !capture->legacy ||
      !capture->host) {
    return;
  }
  ++capture->calls;
  if (capture->attempted.exchange(true))
    return;
  capture->destroy_result.store(
      capture->v2->destroy_context(context));
  capture->shutdown_result.store(capture->v2->shutdown());
  capture->unregister_result.store(
      capture->v2->set_notification_handler(nullptr, nullptr));
  capture->replace_result.store(
      capture->v2->set_notification_handler(
          &ReplacementNotification, capture));
  capture->initialize_result.store(
      capture->v2->initialize(capture->host, &capture->root));
  // ABI v1 has no notification registration entry point, but its safe
  // wrappers are reachable from the same DLL. Re-enter them through a v1
  // function table while this v2 callback owns the callback lifetime token.
  capture->legacy_initialize_result.store(
      capture->legacy->initialize(capture->host, &capture->root));
  capture->legacy_shutdown_result.store(
      capture->legacy->shutdown());
}

bool WaitForEnvironment(const char* name, const char* expected,
                        DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  char value[32]{};
  do {
    const DWORD length = ::GetEnvironmentVariableA(
        name, value, static_cast<DWORD>(std::size(value)));
    if (length < std::size(value) && length == std::strlen(expected) &&
        std::memcmp(value, expected, length) == 0) {
      return true;
    }
    ::Sleep(1);
  } while (::GetTickCount64() < deadline);
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  {
    // ABI-v1 Weasel hosts already pass expand_ibus_modifier(mask), not the
    // public compact convenience constants. Preserve that historical wire
    // behavior byte-for-byte, including RELEASE.
    FamoKeyEvent legacy_expanded{};
    legacy_expanded.virtual_key = 'x';
    legacy_expanded.modifiers =
        FAMO_KEY_V2_MOD_CAPS_LOCK | FAMO_KEY_V2_MOD_CONTROL |
        FAMO_KEY_V2_MOD_ALT | FAMO_KEY_V2_MOD_SUPER |
        FAMO_KEY_V2_MOD_RELEASE;
    legacy_expanded.is_key_down = 0;
    int keycode = 0;
    int mask = 0;
    CHECK(famo_rime_keys::FamoKeyToRimeV1(
        legacy_expanded, &keycode, &mask));
    CHECK(keycode == 'x');
    CHECK(static_cast<uint32_t>(mask) ==
          legacy_expanded.modifiers);

    // is_key_down was advisory in v1 and must not manufacture a RELEASE bit.
    FamoKeyEvent legacy_advisory = legacy_expanded;
    legacy_advisory.modifiers = FAMO_KEY_V2_MOD_CONTROL;
    legacy_advisory.is_key_down = 0;
    CHECK(famo_rime_keys::FamoKeyToRimeV1(
        legacy_advisory, &keycode, &mask));
    CHECK(static_cast<uint32_t>(mask) == legacy_advisory.modifiers);

    FamoKeyEvent expanded = legacy_expanded;
    expanded.modifiers =
        FAMO_KEY_V2_MOD_CONTROL | FAMO_KEY_V2_MOD_CAPS_LOCK |
        FAMO_KEY_V2_MOD_RELEASE;
    CHECK(famo_rime_keys::FamoKeyToRimeV2(expanded, &keycode, &mask));
    CHECK(static_cast<uint32_t>(mask) == expanded.modifiers);
  }
  const fs::path root =
      fs::temp_directory_path() /
      ("famo-action-v2-" + std::to_string(::GetCurrentProcessId()));
  std::error_code error;
  fs::remove_all(root, error);
  fs::create_directories(root, error);
  CHECK(!error);
  CHECK(WriteFixture(root));

  const std::string root_utf8 = root.string();
  FamoUtf8String schema = String("famo_action_v2");
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", "1") != 0);
  {
    // Production does not register a notification handler. Retired librime
    // sessions therefore have no callback route to quarantine and must not
    // accumulate across ordinary focus/session churn.
    FamoEngineHost no_notification_host;
    CHECK(no_notification_host.LoadV2(
              L"FamoRimeEngine.dll", root_utf8.c_str()) ==
          FAMO_ENGINE_OK);
    CHECK(no_notification_host.v2_api().deploy_schema(&schema) ==
          FAMO_ENGINE_OK);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_CONTEXT_ALLOCATION_FAILURE", "1") != 0);
    FamoEngineContext* failed_allocation =
        reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
    CHECK(no_notification_host.v2_api().create_context(
              &schema, &failed_allocation) == FAMO_ENGINE_E_RUNTIME);
    CHECK(failed_allocation == nullptr);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_CONTEXT_ALLOCATION_FAILURE", nullptr) != 0);
    for (size_t iteration = 0; iteration < 1000; ++iteration) {
      FamoEngineContext* churned = nullptr;
      CHECK(no_notification_host.v2_api().create_context(
                &schema, &churned) == FAMO_ENGINE_OK);
      CHECK(churned != nullptr);
      CHECK(no_notification_host.v2_api().destroy_context(churned) ==
            FAMO_ENGINE_OK);
    }

    FamoEngineContext* enable_race_context = nullptr;
    CHECK(no_notification_host.v2_api().create_context(
              &schema, &enable_race_context) == FAMO_ENGINE_OK);
    CHECK(enable_race_context != nullptr);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_PAUSE", "1") != 0);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_ENTERED", nullptr) != 0);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_RELEASE", nullptr) != 0);
    std::atomic<int32_t> enable_result{FAMO_ENGINE_E_RUNTIME};
    std::thread enabler([&] {
      enable_result.store(
          no_notification_host.v2_api().set_notification_handler(
              &CaptureNotification, nullptr));
    });
    const bool enable_entered = WaitForEnvironment(
        "FAMO_TEST_RIME_NOTIFICATION_ENABLE_ENTERED", "1", 2000);
    if (!enable_entered) {
      enabler.join();
      CHECK(enable_entered);
    }
    std::atomic_bool destroy_returned{false};
    std::atomic<int32_t> destroy_result{FAMO_ENGINE_E_RUNTIME};
    std::thread destroyer([&] {
      destroy_result.store(no_notification_host.v2_api().destroy_context(
          enable_race_context));
      destroy_returned.store(true);
    });
    ::Sleep(30);
    const bool destroy_blocked = !destroy_returned.load();
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_RELEASE", "1") != 0);
    enabler.join();
    destroyer.join();
    CHECK(destroy_blocked);
    CHECK(enable_result.load() == FAMO_ENGINE_OK);
    CHECK(destroy_result.load() == FAMO_ENGINE_OK);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_PAUSE", nullptr) != 0);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_ENTERED", nullptr) != 0);
    CHECK(::SetEnvironmentVariableA(
              "FAMO_TEST_RIME_NOTIFICATION_ENABLE_RELEASE", nullptr) != 0);
    no_notification_host.Unload();
  }
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", nullptr) != 0);

  NotificationCapture notification;
  FamoEngineHost host;
  CHECK(host.LoadV2(L"FamoRimeEngine.dll", root_utf8.c_str(),
                    &CaptureNotification, &notification) ==
        FAMO_ENGINE_OK);
  CHECK(host.V2Runnable());

  CHECK(host.v2_api().deploy_schema(&schema) ==
        FAMO_ENGINE_OK);
  FamoEngineContext* context = nullptr;
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  CHECK(context != nullptr);

  // A failed create must not erase and destroy its session while a concurrent
  // librime notification still owns that binding for get_state_label.
  CHECK(_putenv_s("FAMO_TEST_CREATE_FAILURE_AFTER_NOTIFICATION", "1") == 0);
  std::atomic_bool failed_create_returned{false};
  std::atomic<int32_t> failed_create_result{FAMO_ENGINE_OK};
  FamoEngineContext* failed_context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  std::thread failed_creator([&] {
    failed_create_result.store(
        host.v2_api().create_context(&schema, &failed_context));
    failed_create_returned.store(true);
  });
  ::Sleep(30);
  CHECK(!failed_create_returned.load());
  failed_creator.join();
  CHECK(failed_create_returned.load());
  CHECK(failed_create_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(failed_context == nullptr);
  CHECK(_putenv_s("FAMO_TEST_CREATE_FAILURE_AFTER_NOTIFICATION", "") == 0);
  FamoEngineContext* after_failed_create = nullptr;
  CHECK(host.v2_api().create_context(&schema, &after_failed_create) ==
        FAMO_ENGINE_OK);
  CHECK(after_failed_create != nullptr);
  CHECK(host.v2_api().destroy_context(after_failed_create) == FAMO_ENGINE_OK);

  FamoUtf8String property_name = String("client_app");
  FamoUtf8String property_value = String("rime-canary.exe");
  CHECK(host.SetProperty(context, &property_name, &property_value) ==
        FAMO_ENGINE_OK);
  FamoUtf8String ascii_mode = String("ascii_mode");
  CHECK(host.SetOption(context, &ascii_mode, 1) == FAMO_ENGINE_OK);
  {
    std::lock_guard<std::mutex> lock(notification.mutex);
    CHECK(!notification.copy_failed.load());
    CHECK(notification.ascii_option_count >= 1);
    CHECK(notification.context == context);
    CHECK(notification.label == "ASCII");
  }
  CHECK(host.SetOption(context, &ascii_mode, 0) == FAMO_ENGINE_OK);

  // Every lifecycle operation that can drain notifications or acquire the
  // context-creation locks must fail fast when re-entered by a notification.
  // The legacy v1 no-unwind adapters must preserve that result instead of
  // invoking ResetRimeState and waiting for the current callback to exit.
  using CreateLegacyFn =
      int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApi*);
  HMODULE rime_module = ::GetModuleHandleW(L"FamoRimeEngine.dll");
  CHECK(rime_module != nullptr);
  const auto create_legacy = reinterpret_cast<CreateLegacyFn>(
      ::GetProcAddress(rime_module, "FamoCreateEngineApi"));
  CHECK(create_legacy != nullptr);
  FamoEngineApi legacy_api{};
  legacy_api.size = static_cast<uint32_t>(sizeof(legacy_api));
  CHECK(create_legacy(FAMO_ENGINE_ABI_VERSION, &legacy_api) ==
        FAMO_ENGINE_OK);
  ReentrantLifecycleCapture lifecycle;
  lifecycle.v2 = &host.v2_api();
  lifecycle.legacy = &legacy_api;
  lifecycle.host = &host.host_api();
  lifecycle.root = String(root_utf8);
  CHECK(host.v2_api().set_notification_handler(
            &ReentrantLifecycleNotification, &lifecycle) ==
        FAMO_ENGINE_OK);
  CHECK(host.SetOption(context, &ascii_mode, 1) == FAMO_ENGINE_OK);
  CHECK(lifecycle.attempted.load());
  CHECK(lifecycle.destroy_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.shutdown_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.unregister_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.replace_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.initialize_result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.legacy_initialize_result.load() ==
        FAMO_ENGINE_E_RUNTIME);
  CHECK(lifecycle.legacy_shutdown_result.load() ==
        FAMO_ENGINE_E_RUNTIME);
  CHECK(host.SetOption(context, &ascii_mode, 0) == FAMO_ENGINE_OK);
  CHECK(lifecycle.calls.load() >= 2);
  CHECK(host.v2_api().set_notification_handler(
            &CaptureNotification, &notification) == FAMO_ENGINE_OK);

  // A host notification target is outside the engine's trust boundary. Its
  // exception must not unwind through librime/create_context, double-delete a
  // context, or leave a stale session binding behind.
  ThrowingNotificationCapture throwing_notification;
  CHECK(host.v2_api().set_notification_handler(
            &ThrowingNotification, &throwing_notification) ==
        FAMO_ENGINE_OK);
  FamoEngineContext* throwing_context = nullptr;
  CHECK(host.v2_api().create_context(&schema, &throwing_context) ==
        FAMO_ENGINE_OK);
  CHECK(throwing_context != nullptr);
  CHECK(host.SetOption(throwing_context, &ascii_mode, 1) ==
        FAMO_ENGINE_OK);
  CHECK(throwing_notification.calls.load() >= 1);
  CHECK(host.v2_api().destroy_context(throwing_context) == FAMO_ENGINE_OK);
  throwing_context = nullptr;
  CHECK(host.v2_api().set_notification_handler(
            &CaptureNotification, &notification) == FAMO_ENGINE_OK);
  CHECK(host.SetOption(context, &ascii_mode, 1) == FAMO_ENGINE_OK);
  CHECK(host.SetOption(context, &ascii_mode, 0) == FAMO_ENGINE_OK);

  FamoEngineActionRequestV2 request = Request(FAMO_ENGINE_ACTION_STATUS);
  FamoEngineActionResultV2* result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->view.layout_version == FAMO_COMPOSITION_LAYOUT_V2);
  CHECK(result->view.candidate_layout_version == FAMO_CANDIDATE_LAYOUT_V2);
  CHECK(result->view.candidate_stride == FAMO_CANDIDATE_V2_STRIDE);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  const uint32_t keys[] = {'n', 'i'};
  for (const uint32_t key : keys) {
    request.key.virtual_key = key;
    result = nullptr;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(result->handled == 1);
    if (key == 'i') {
      CHECK(result->view.candidate_count >= 2);
      const auto* bytes =
          reinterpret_cast<const unsigned char*>(result->view.candidates);
      const auto* first =
          reinterpret_cast<const FamoCandidateV2*>(bytes);
      const auto* second = reinterpret_cast<const FamoCandidateV2*>(
          bytes + result->view.candidate_stride);
      CHECK(first->struct_size == FAMO_CANDIDATE_V2_STRIDE);
      CHECK(second->struct_size == FAMO_CANDIDATE_V2_STRIDE);
      CHECK(Equals(first->text, "\xE4\xBD\xA0"));
      CHECK(Equals(second->text, "\xE5\xB0\xBC"));
      CHECK(Equals(result->view.commit_preview, "\xE4\xBD\xA0"));
    }
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }

  request = Request(FAMO_ENGINE_ACTION_PEEK_CANDIDATES);
  request.index = 1;
  request.count = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 0);
  CHECK(result->view.candidate_count == 1);
  CHECK(Equals(result->view.candidates->text, "\xE5\xB0\xBC"));
  CHECK(Equals(result->view.candidates->label, "k"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request.index = 2;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.candidate_count == 1);
  CHECK(Equals(result->view.candidates->text, "\xE6\xB3\xA5"));
  CHECK(Equals(result->view.candidates->label, "j"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.preedit.length_bytes != 0);  // peek did not mutate
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.commit, "\xE4\xBD\xA0"));
  CHECK(result->view.preedit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 0x7b;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.commit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  for (const uint32_t key : keys) {
    request.key.virtual_key = key;
    result = nullptr;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }
  request = Request(FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  request.index = 1;
  result = nullptr;
  FamoEngineHost::SetAllocationFailureCountdownForTesting(1);
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  CHECK(result->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  CHECK(result->handled == 1);
  CHECK(result->result_flags == FAMO_ENGINE_RESULT_RESYNC_REQUIRED);
  CHECK(result->view.commit.length_bytes == 0);
  CHECK(host.SetOption(context, &ascii_mode, 1) ==
        FAMO_ENGINE_E_RECOVERY_REQUIRED);
  CHECK(host.SetProperty(context, &property_name, &property_value) ==
        FAMO_ENGINE_E_RECOVERY_REQUIRED);
  CHECK(host.v2_api().deploy_schema(&schema) ==
        FAMO_ENGINE_E_RECOVERY_REQUIRED);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_RECOVER);
  request.value = FAMO_ENGINE_ACTION_SELECT_CANDIDATE;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(result->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  CHECK(result->result_flags == 0);
  CHECK(Equals(result->view.commit, "\xE5\xB0\xBC"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.commit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  // A callback that entered before disable but has not reached its host target
  // still owns g_rime/context lifetime. Disable must wait for the complete
  // entry/exit guard, not merely for callbacks already inside Deliver.
  {
    std::lock_guard<std::mutex> lock(notification.mutex);
    notification.block = true;
    notification.entered = false;
    notification.release = false;
  }
  std::atomic<int32_t> set_option_result{FAMO_ENGINE_E_RUNTIME};
  std::thread notifier([&] {
    set_option_result.store(host.SetOption(context, &ascii_mode, 1));
  });
  {
    std::unique_lock<std::mutex> lock(notification.mutex);
    CHECK(notification.condition.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return notification.entered; }));
  }
  std::atomic_bool disable_returned{false};
  std::atomic<int32_t> disable_result{FAMO_ENGINE_E_RUNTIME};
  std::thread disabler([&] {
    disable_result.store(
        host.v2_api().set_notification_handler(nullptr, nullptr));
    disable_returned.store(true);
  });
  ::Sleep(20);
  CHECK(!disable_returned.load());
  {
    std::lock_guard<std::mutex> lock(notification.mutex);
    notification.release = true;
  }
  notification.condition.notify_all();
  notifier.join();
  disabler.join();
  CHECK(set_option_result.load() == FAMO_ENGINE_OK);
  CHECK(disable_result.load() == FAMO_ENGINE_OK);
  CHECK(disable_returned.load());

  CHECK(host.v2_api().destroy_context(context) == FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", "1") != 0);
  host.Unload();
  char attempted_sessions[32]{};
  char destroyed_sessions[32]{};
  char failed_sessions[32]{};
  char notifications_drained[4]{};
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", attempted_sessions,
            static_cast<DWORD>(std::size(attempted_sessions))) == 1);
  CHECK(std::strcmp(attempted_sessions, "4") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", destroyed_sessions,
            static_cast<DWORD>(std::size(destroyed_sessions))) == 1);
  CHECK(std::strcmp(destroyed_sessions, "4") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", failed_sessions,
            static_cast<DWORD>(std::size(failed_sessions))) == 1);
  CHECK(std::strcmp(failed_sessions, "0") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATIONS_DRAINED", notifications_drained,
            static_cast<DWORD>(std::size(notifications_drained))) == 1);
  CHECK(std::strcmp(notifications_drained, "1") == 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATIONS_DRAINED", nullptr) != 0);

  // Callback ids cannot be rebound safely within one librime epoch. When the
  // bounded quarantine fills and no context/result is live, the engine rotates
  // that epoch in-process and restores the notification target.
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", "1") != 0);
  CHECK(host.LoadV2(L"FamoRimeEngine.dll", root_utf8.c_str(),
                    &CaptureNotification, &notification) ==
        FAMO_ENGINE_OK);
  CHECK(host.v2_api().deploy_schema(&schema) == FAMO_ENGINE_OK);
  FamoEngineContext* capped_context = nullptr;
  CHECK(host.v2_api().create_context(&schema, &capped_context) ==
        FAMO_ENGINE_OK);
  CHECK(capped_context != nullptr);
  CHECK(host.v2_api().destroy_context(capped_context) ==
        FAMO_ENGINE_OK);
  capped_context = nullptr;
  for (size_t iteration = 0; iteration < 3; ++iteration) {
    FamoEngineContext* rotated_context = nullptr;
    CHECK(host.v2_api().create_context(&schema, &rotated_context) ==
          FAMO_ENGINE_OK);
    CHECK(rotated_context != nullptr);
    request = Request(FAMO_ENGINE_ACTION_STATUS);
    result = nullptr;
    CHECK(host.v2_api().execute_action(rotated_context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(result != nullptr);
    CHECK(FamoEngineHost::ValidateResultV2(
        result, FAMO_ENGINE_ACTION_STATUS,
        FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES,
        FAMO_ENGINE_V2_MAX_STRING_BYTES));
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
    CHECK(host.SetOption(rotated_context, &ascii_mode, 1) ==
          FAMO_ENGINE_OK);
    {
      std::lock_guard<std::mutex> lock(notification.mutex);
      CHECK(notification.context == rotated_context);
    }
    CHECK(host.v2_api().destroy_context(rotated_context) ==
          FAMO_ENGINE_OK);
  }
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", "1") != 0);
  host.Unload();
  std::memset(attempted_sessions, 0, sizeof(attempted_sessions));
  std::memset(destroyed_sessions, 0, sizeof(destroyed_sessions));
  std::memset(failed_sessions, 0, sizeof(failed_sessions));
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", attempted_sessions,
            static_cast<DWORD>(std::size(attempted_sessions))) == 1);
  CHECK(std::strcmp(attempted_sessions, "1") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", destroyed_sessions,
            static_cast<DWORD>(std::size(destroyed_sessions))) == 1);
  // Each full quarantine rotated before the next create. Only the final
  // epoch's one retired session reaches this shutdown.
  CHECK(std::strcmp(destroyed_sessions, "1") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", failed_sessions,
            static_cast<DWORD>(std::size(failed_sessions))) == 1);
  CHECK(std::strcmp(failed_sessions, "0") == 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATIONS_DRAINED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", nullptr) != 0);

  // An engine-wide host callback may re-enter create_context. If the bounded
  // quarantine is full, that nested call must fail without trying to wait for
  // its own callback; the outer non-callback create then rotates normally.
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", "1") != 0);
  FamoEngineHost reentrant_host;
  ReentrantNotificationCapture reentrant;
  reentrant.host = &reentrant_host;
  reentrant.schema = schema;
  CHECK(reentrant_host.LoadV2(
            L"FamoRimeEngine.dll", root_utf8.c_str(),
            &ReentrantNotification, &reentrant) == FAMO_ENGINE_OK);
  CHECK(reentrant_host.v2_api().deploy_schema(&schema) ==
        FAMO_ENGINE_OK);
  FamoEngineContext* retired_context = nullptr;
  CHECK(reentrant_host.v2_api().create_context(
            &schema, &retired_context) == FAMO_ENGINE_OK);
  CHECK(reentrant_host.v2_api().destroy_context(retired_context) ==
        FAMO_ENGINE_OK);
  reentrant.armed.store(true);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_ROTATION_PREFLIGHT_FAILURE", "1") != 0);
  FamoEngineContext* failed_rotation =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(reentrant_host.v2_api().create_context(
            &schema, &failed_rotation) == FAMO_ENGINE_E_RUNTIME);
  CHECK(failed_rotation == nullptr);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_ROTATION_PREFLIGHT_FAILURE", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATION_DURING_ROTATION", "1") != 0);
  FamoEngineContext* outer_context = nullptr;
  CHECK(reentrant_host.v2_api().create_context(
            &schema, &outer_context) == FAMO_ENGINE_OK);
  CHECK(reentrant.attempted.load());
  CHECK(reentrant.result.load() == FAMO_ENGINE_E_RUNTIME);
  CHECK(reentrant.context == nullptr);
  CHECK(outer_context != nullptr);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATION_DURING_ROTATION", nullptr) !=
        0);
  CHECK(reentrant_host.v2_api().destroy_context(outer_context) ==
        FAMO_ENGINE_OK);
  reentrant_host.Unload();
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MAX_SEEN_SESSION_IDS", nullptr) != 0);

  // Shutdown diagnostics count attempts, successes, and failures separately.
  // A failed individual destroy is still recovered by librime finalize.
  CHECK(host.LoadV2(L"FamoRimeEngine.dll", root_utf8.c_str(),
                    &CaptureNotification, &notification) ==
        FAMO_ENGINE_OK);
  CHECK(host.v2_api().deploy_schema(&schema) == FAMO_ENGINE_OK);
  FamoEngineContext* failed_destroy_context = nullptr;
  CHECK(host.v2_api().create_context(&schema, &failed_destroy_context) ==
        FAMO_ENGINE_OK);
  CHECK(failed_destroy_context != nullptr);
  CHECK(host.v2_api().destroy_context(failed_destroy_context) ==
        FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", "1") != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILURE", "1") != 0);
  host.Unload();
  std::memset(attempted_sessions, 0, sizeof(attempted_sessions));
  std::memset(destroyed_sessions, 0, sizeof(destroyed_sessions));
  std::memset(failed_sessions, 0, sizeof(failed_sessions));
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", attempted_sessions,
            static_cast<DWORD>(std::size(attempted_sessions))) == 1);
  CHECK(std::strcmp(attempted_sessions, "1") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", destroyed_sessions,
            static_cast<DWORD>(std::size(destroyed_sessions))) == 1);
  CHECK(std::strcmp(destroyed_sessions, "0") == 0);
  CHECK(::GetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", failed_sessions,
            static_cast<DWORD>(std::size(failed_sessions))) == 1);
  CHECK(std::strcmp(failed_sessions, "1") == 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_AUDIT", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILURE", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_ATTEMPTED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROYED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_QUARANTINE_DESTROY_FAILED", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_RIME_NOTIFICATIONS_DRAINED", nullptr) != 0);

  fs::remove_all(root, error);
  CHECK(!error);
  std::printf("rime_action_v2_selfcheck: OK\n");
  return 0;
}
