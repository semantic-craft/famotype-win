// Public-DLL canary for the Famo engine ABI v2 action interface.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../engines/action_v2_result.h"
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

static_assert(FAMO_KEY_MOD_SHIFT == 0x00000001u);
static_assert(FAMO_KEY_MOD_CONTROL == 0x00000002u);
static_assert(FAMO_KEY_MOD_ALT == 0x00000004u);
static_assert(FAMO_KEY_MOD_SUPER == 0x00000008u);
static_assert(FAMO_KEY_MOD_CAPS_LOCK == 0x00000010u);
static_assert(FAMO_KEY_V2_MOD_SHIFT == 0x00000001u);
static_assert(FAMO_KEY_V2_MOD_CAPS_LOCK == 0x00000002u);
static_assert(FAMO_KEY_V2_MOD_CONTROL == 0x00000004u);
static_assert(FAMO_KEY_V2_MOD_ALT == 0x00000008u);
static_assert(FAMO_KEY_V2_MOD_SUPER == 0x04000000u);
static_assert(FAMO_KEY_V2_MOD_RELEASE == 0x40000000u);
static_assert(FAMO_ENGINE_ACTION_REQUEST_V2_REQUIRED_SIZE <=
              sizeof(FamoEngineActionRequestV2));

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

struct NotificationCapture {
  FamoEngineContext* context = nullptr;
  std::string type;
  std::string value;
  std::string label;
  uint32_t count = 0;
  bool copy_failed = false;
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
    capture->context = context;
    capture->type.assign(type->data ? type->data : "", type->length_bytes);
    capture->value.assign(value->data ? value->data : "",
                          value->length_bytes);
    capture->label.assign(label->data ? label->data : "",
                          label->length_bytes);
    ++capture->count;
  } catch (...) {
    capture->copy_failed = true;
  }
}

}  // namespace

int main() {
  famo_action_v2::Snapshot shed_candidates;
  shed_candidates.candidates.resize(1);
  shed_candidates.candidates[0].text.assign(
      FAMO_ENGINE_V2_MAX_STRING_BYTES + 1u, 'x');
  shed_candidates.highlighted_index = 1;
  shed_candidates.page_index = 3;
  shed_candidates.page_size = 9;
  shed_candidates.is_last_page = 0;
  shed_candidates.state_flags |= FAMO_COMPOSITION_HAS_CANDIDATES;
  CHECK(famo_action_v2::TrimOptionalToResultBudget(&shed_candidates));
  CHECK(shed_candidates.candidates.empty());
  CHECK(shed_candidates.highlighted_index == 0);
  CHECK(shed_candidates.page_index == 0);
  CHECK(shed_candidates.page_size == 0);
  CHECK(shed_candidates.is_last_page == 1);
  CHECK((shed_candidates.state_flags &
         FAMO_COMPOSITION_HAS_CANDIDATES) == 0);

  using CreateV2 = int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApiV2*);
  HMODULE module = ::LoadLibraryW(L"FamoTestEngine.dll");
  CHECK(module != nullptr);
  const auto create_v2 = reinterpret_cast<CreateV2>(
      ::GetProcAddress(module, "FamoCreateEngineApiV2"));
  CHECK(create_v2 != nullptr);
  struct GuardedApi {
    FamoEngineApiV2 api{};
    std::array<unsigned char, 32> guard{};
  } guarded_api;
  guarded_api.guard.fill(0xA7);
  guarded_api.api.struct_size =
      static_cast<uint32_t>(FAMO_ENGINE_API_V2_REQUIRED_SIZE - 1);
  CHECK(create_v2(FAMO_ENGINE_ABI_V2, &guarded_api.api) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  for (const unsigned char byte : guarded_api.guard)
    CHECK(byte == 0xA7);
  guarded_api.api.struct_size =
      static_cast<uint32_t>(FAMO_ENGINE_API_V2_REQUIRED_SIZE);
  CHECK(create_v2(FAMO_ENGINE_ABI_V2, &guarded_api.api) == FAMO_ENGINE_OK);
  CHECK(guarded_api.api.struct_size == FAMO_ENGINE_API_V2_REQUIRED_SIZE);
  for (const unsigned char byte : guarded_api.guard)
    CHECK(byte == 0xA7);
  ::FreeLibrary(module);

  NotificationCapture notification;
  FamoEngineHost host;
  CHECK(host.LoadV2(L"FamoTestEngine.dll", "", &CaptureNotification,
                    &notification) == FAMO_ENGINE_OK);
  CHECK(host.V2Runnable());

  FamoEngineActionResultV2 unregistered_result{};
  unregistered_result.struct_size =
      FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE;
  CHECK(!FamoEngineHost::ValidateResultV2(
      &unregistered_result, FAMO_ENGINE_ACTION_STATUS, 0, 32));

  auto* semantic_result = static_cast<FamoEngineActionResultV2*>(
      host.host_api().alloc(sizeof(FamoEngineActionResultV2)));
  CHECK(semantic_result != nullptr);
  std::memset(semantic_result, 0, sizeof(*semantic_result));
  semantic_result->struct_size = FAMO_ENGINE_ACTION_RESULT_V2_REQUIRED_SIZE;
  semantic_result->action = FAMO_ENGINE_ACTION_CLEAR_COMPOSITION;
  semantic_result->handled = 1;
  semantic_result->view.struct_size =
      FAMO_COMPOSITION_VIEW_V2_REQUIRED_SIZE;
  semantic_result->view.layout_version = FAMO_COMPOSITION_LAYOUT_V2;
  semantic_result->view.candidate_layout_version =
      FAMO_CANDIDATE_LAYOUT_V2;
  semantic_result->view.candidate_stride = FAMO_CANDIDATE_V2_STRIDE;
  semantic_result->view.preedit.size = FAMO_UTF8_STRING_REQUIRED_SIZE;
  semantic_result->view.commit.size = FAMO_UTF8_STRING_REQUIRED_SIZE;
  semantic_result->view.commit_preview.size =
      FAMO_UTF8_STRING_REQUIRED_SIZE;
  semantic_result->view.schema_id.size = FAMO_UTF8_STRING_REQUIRED_SIZE;
  semantic_result->view.schema_name.size = FAMO_UTF8_STRING_REQUIRED_SIZE;
  CHECK(FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_CLEAR_COMPOSITION, 0, 32));
  semantic_result->view.page_size = 1;
  CHECK(!FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_CLEAR_COMPOSITION, 0, 32));
  semantic_result->view.page_size = 0;
  semantic_result->result_flags = FAMO_ENGINE_RESULT_RESYNC_REQUIRED;
  CHECK(FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_CLEAR_COMPOSITION, 0, 32));
  semantic_result->view.page_index = 1;
  CHECK(!FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_CLEAR_COMPOSITION, 0, 32));
  semantic_result->view.page_index = 0;
  semantic_result->result_flags = 0;
  semantic_result->view.commit = String("x");
  semantic_result->view.state_flags = FAMO_COMPOSITION_HAS_COMMIT;
  CHECK(!FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_CLEAR_COMPOSITION, 0, 32));
  semantic_result->action = FAMO_ENGINE_ACTION_PROCESS_KEY;
  semantic_result->handled = 0;
  CHECK(!FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_PROCESS_KEY, 0, 32));
  semantic_result->handled = 1;
  CHECK(!FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_PROCESS_KEY, 0, 32));
  // The content pointer above is outside the registered result allocation.
  // Once removed, the otherwise-valid PROCESS_KEY result is accepted.
  semantic_result->view.commit = {};
  semantic_result->view.commit.size = FAMO_UTF8_STRING_REQUIRED_SIZE;
  semantic_result->view.state_flags = 0;
  CHECK(FamoEngineHost::ValidateResultV2(
      semantic_result, FAMO_ENGINE_ACTION_PROCESS_KEY, 0, 32));
  host.host_api().free(semantic_result);

  struct GuardedInfo {
    FamoEngineInfo info{};
    std::array<unsigned char, 32> guard{};
  } guarded_info;
  guarded_info.guard.fill(0xD3);
  guarded_info.info.size =
      static_cast<uint32_t>(FAMO_ENGINE_INFO_REQUIRED_SIZE - 1);
  CHECK(host.v2_api().get_info(&guarded_info.info) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  for (const unsigned char byte : guarded_info.guard)
    CHECK(byte == 0xD3);
  guarded_info.info.size = static_cast<uint32_t>(sizeof(FamoEngineInfo));
  CHECK(host.v2_api().get_info(&guarded_info.info) == FAMO_ENGINE_OK);
  CHECK(guarded_info.info.abi_version == FAMO_ENGINE_ABI_V2);
  for (const unsigned char byte : guarded_info.guard)
    CHECK(byte == 0xD3);

  FamoEngineContext* context = nullptr;
  FamoUtf8String malformed{FAMO_UTF8_STRING_REQUIRED_SIZE - 1, "test", 4};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&malformed, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  const char embedded_data[] = {'t', '\0', 'x'};
  FamoUtf8String embedded{sizeof(FamoUtf8String), embedded_data, 3};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&embedded, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  const char overlong_data[] = {static_cast<char>(0xc0),
                                static_cast<char>(0xaf)};
  FamoUtf8String overlong{sizeof(FamoUtf8String), overlong_data, 2};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&overlong, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  const char truncated_data[] = {static_cast<char>(0xe2),
                                 static_cast<char>(0x82)};
  FamoUtf8String truncated{sizeof(FamoUtf8String), truncated_data, 2};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&truncated, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  FamoUtf8String huge_declared{
      sizeof(FamoUtf8String), "x",
      FAMO_ENGINE_V2_MAX_STRING_BYTES + 1u};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&huge_declared, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  FamoUtf8String null_nonempty{
      sizeof(FamoUtf8String), nullptr, 1};
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&null_nonempty, &context) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(context == nullptr);
  FamoUtf8String fail_after_notify = String("notify_then_fail");
  context =
      reinterpret_cast<FamoEngineContext*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().create_context(&fail_after_notify, &context) ==
        FAMO_ENGINE_E_RUNTIME);
  CHECK(context == nullptr);
  CHECK(notification.count == 0);
  FamoUtf8String reuse_after_fail =
      String("reuse_after_failed_create");
  CHECK(host.v2_api().create_context(&reuse_after_fail, &context) ==
        FAMO_ENGINE_OK);
  CHECK(context != nullptr);
  CHECK(notification.count == 1);
  CHECK(notification.context == context);
  CHECK(notification.value == "create_reuse_fresh");
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);
  notification = {};

  FamoUtf8String schema = String("test");
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  CHECK(context != nullptr);
  CHECK(host.v2_api().set_option(context, &overlong, 1) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(host.v2_api().set_option(context, &huge_declared, 1) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  int32_t option_value = 99;
  CHECK(host.v2_api().get_option(context, &truncated, &option_value) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(option_value == 0);
  CHECK(host.v2_api().deploy_schema(&overlong) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  FamoUtf8String property_name = String("client_app");
  FamoUtf8String property_value = String("Code.exe");
  CHECK(host.SetProperty(context, &overlong, &property_value) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(host.SetProperty(context, &huge_declared, &property_value) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(host.SetProperty(context, &property_name, &truncated) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(host.SetProperty(context, &property_name, &property_value) ==
        FAMO_ENGINE_OK);
  FamoUtf8String notify_client_app = String("notify_client_app");
  CHECK(host.SetOption(context, &notify_client_app, 1) == FAMO_ENGINE_OK);
  CHECK(!notification.copy_failed);
  CHECK(notification.count == 1);
  CHECK(notification.context == context);
  CHECK(notification.type == "option");
  CHECK(notification.value == "notify_client_app");
  CHECK(notification.label == "Code.exe");

  // The host callback boundary rejects malformed UTF-8 and a session option
  // falsely presented as an engine-wide (null-context) event.
  FamoUtf8String notify_invalid = String("notify_invalid");
  CHECK(host.SetOption(context, &notify_invalid, 1) == FAMO_ENGINE_OK);
  FamoUtf8String notify_null_context = String("notify_null_context");
  CHECK(host.SetOption(context, &notify_null_context, 1) == FAMO_ENGINE_OK);
  CHECK(notification.count == 1);
  CHECK(host.v2_api().set_notification_handler(nullptr, &notification) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);

  FamoEngineActionRequestV2 request = Request(FAMO_ENGINE_ACTION_STATUS);
  request.view_layout_version = 999u;
  auto* result =
      reinterpret_cast<FamoEngineActionResultV2*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_UNSUPPORTED_ABI);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  request.struct_size = FAMO_ENGINE_ACTION_REQUEST_V2_REQUIRED_SIZE - 1;
  result =
      reinterpret_cast<FamoEngineActionResultV2*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  request.index = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  request.count = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_CHANGE_PAGE);
  request.key.virtual_key = 'x';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_PEEK_CANDIDATES);
  request.value = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = FAMO_KEY_EVENT_REQUIRED_SIZE;
  request.key.virtual_key = 'x';
  request.key.is_key_down = 2;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request.key.is_key_down = 1;
  request.key.modifiers = FAMO_KEY_V2_MOD_RELEASE;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request.key.is_key_down = 0;
  request.key.modifiers = 0;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request.key.modifiers = FAMO_KEY_V2_MOD_RELEASE;
  request.index = 1;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request.index = 0;
  request.key.virtual_key = 0xffe1;  // Shift_L
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr && result->handled == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(999u);
  result =
      reinterpret_cast<FamoEngineActionResultV2*>(static_cast<uintptr_t>(1));
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  request.view_layout_version = FAMO_COMPOSITION_LAYOUT_V2;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->struct_size == sizeof(FamoEngineActionResultV2));
  CHECK(result->action == FAMO_ENGINE_ACTION_STATUS);
  CHECK(result->view.struct_size == sizeof(FamoCompositionViewV2));
  CHECK(result->view.layout_version == FAMO_COMPOSITION_LAYOUT_V2);
  CHECK(result->view.candidate_layout_version == FAMO_CANDIDATE_LAYOUT_V2);
  CHECK(result->view.candidate_stride == FAMO_CANDIDATE_V2_STRIDE);

  // Release owns its metadata; it must not trust mutable public layout fields.
  result->struct_size = 0;
  result->view.candidate_count = UINT32_MAX;
  result->view.candidate_stride = 1;
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_E_INVALID_ARGUMENT);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'n';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->handled == 1);
  CHECK((result->view.state_flags & FAMO_COMPOSITION_HANDLED) == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request.key.virtual_key = 'i';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.preedit, "ni"));
  CHECK(result->view.candidate_count == 3);
  CHECK(FamoEngineHost::ValidateResultV2(
      result, FAMO_ENGINE_ACTION_PROCESS_KEY,
      FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES,
      FAMO_ENGINE_V2_MAX_STRING_BYTES));
  const uint32_t valid_page_size = result->view.page_size;
  result->view.page_size = 0;
  CHECK(!FamoEngineHost::ValidateResultV2(
      result, FAMO_ENGINE_ACTION_PROCESS_KEY,
      FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES,
      FAMO_ENGINE_V2_MAX_STRING_BYTES));
  result->view.page_size = valid_page_size;
  const auto* candidate_bytes =
      reinterpret_cast<const unsigned char*>(result->view.candidates);
  const auto* first =
      reinterpret_cast<const FamoCandidateV2*>(candidate_bytes);
  const auto* second = reinterpret_cast<const FamoCandidateV2*>(
      candidate_bytes + result->view.candidate_stride);
  CHECK(first->struct_size == FAMO_CANDIDATE_V2_STRIDE);
  CHECK(second->struct_size == FAMO_CANDIDATE_V2_STRIDE);
  CHECK(Equals(first->text, "\xE4\xBD\xA0"));
  CHECK(Equals(second->text, "\xE5\xB0\xBC"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request.key.virtual_key = 0x7b;  // F12
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->handled == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(Equals(result->view.commit_preview, "\xE4\xBD\xA0"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  CHECK(::SetEnvironmentVariableA("FAMO_TEST_FORMAT_COMMIT", "1") != 0);
  request = Request(FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.commit,
               "\xE3\x80\x8C\xE4\xBD\xA0\xE3\x80\x8D"));  // 「你」
  CHECK(!Equals(result->view.commit, "\xE4\xBD\xA0"));
  CHECK(result->view.preedit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA("FAMO_TEST_FORMAT_COMMIT", nullptr) != 0);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 0x7b;  // next key must not repeat the commit
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 0);
  CHECK(result->view.commit.length_bytes == 0);
  CHECK((result->view.state_flags & FAMO_COMPOSITION_HAS_COMMIT) == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  const uint32_t selection_keys[] = {'n', 'i'};
  for (const uint32_t key : selection_keys) {
    request.key.virtual_key = key;
    result = nullptr;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }
  request = Request(FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  request.index = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.commit, "\xE5\xB0\xBC"));  // 尼
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
  for (const uint32_t key : selection_keys) {
    request.key.virtual_key = key;
    result = nullptr;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }

  request = Request(FAMO_ENGINE_ACTION_HIGHLIGHT_CANDIDATE);
  request.index = 1;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(result->view.highlighted_index == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_CHANGE_PAGE);
  request.value = 0;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PEEK_CANDIDATES);
  request.index = 1;
  request.count = 2;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 0);
  CHECK(result->view.candidate_count == 2);
  candidate_bytes =
      reinterpret_cast<const unsigned char*>(result->view.candidates);
  first = reinterpret_cast<const FamoCandidateV2*>(candidate_bytes);
  second = reinterpret_cast<const FamoCandidateV2*>(
      candidate_bytes + result->view.candidate_stride);
  CHECK(Equals(first->text, "\xE5\xB0\xBC"));
  CHECK(Equals(second->text, "\xE6\xB3\xA5"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_STATUS);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(Equals(result->view.preedit, "ni"));  // peek is non-mutating
  CHECK(result->view.highlighted_index == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.commit, "\xE5\xB0\xBC"));  // 尼
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  for (const uint32_t key : selection_keys) {
    request.key.virtual_key = key;
    result = nullptr;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }

  request = Request(FAMO_ENGINE_ACTION_SELECT_CANDIDATE_ABSOLUTE);
  request.index = 2;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.commit, "\xE6\xB3\xA5"));  // 泥
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'x';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_CLEAR_COMPOSITION);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->handled == 1);
  CHECK(result->view.preedit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);

  request = Request(FAMO_ENGINE_ACTION_CHANGE_PAGE);
  request.value = 2;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);
  request = Request(FAMO_ENGINE_ACTION_PEEK_CANDIDATES);
  request.count = FAMO_ENGINE_V2_MAX_PEEK_CANDIDATES + 1;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);

  CHECK(host.v2_api().destroy_context(context) == FAMO_ENGINE_OK);

  // Emergency allocation is a strict precondition: failure means no action
  // dispatch and no state change.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  result = nullptr;
  FamoEngineHost::SetAllocationFailureCountdownForTesting(0);
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_RUNTIME);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  CHECK(result == nullptr);
  FamoUtf8String process_count_name = String("test_process_count");
  int32_t action_count = -1;
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 0);
  request = Request(FAMO_ENGINE_ACTION_STATUS);
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.preedit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // A post-mutation snapshot failure returns only the preallocated receipt.
  // The context rejects another mutation, then RECOVER publishes the original
  // PROCESS_KEY result without dispatching the key a second time.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_FAIL_POST_MUTATION_SNAPSHOT", "process") != 0);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result != nullptr);
  CHECK(result->action == FAMO_ENGINE_ACTION_PROCESS_KEY);
  CHECK(result->handled == 1);
  CHECK(result->result_flags == FAMO_ENGINE_RESULT_RESYNC_REQUIRED);
  CHECK(result->view.state_flags == 0);
  CHECK(result->view.preedit.length_bytes == 0);
  CHECK(FamoEngineHost::ValidateResultV2(
      result, FAMO_ENGINE_ACTION_PROCESS_KEY, 0, 32));
  action_count = -1;
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  FamoEngineActionResultV2* receipt = result;
  result = nullptr;
  request.key.virtual_key = 'b';
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_RECOVERY_REQUIRED);
  CHECK(result == nullptr);
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_FAIL_POST_MUTATION_SNAPSHOT", nullptr) != 0);
  CHECK(host.FreeResultV2(receipt) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_RECOVER);
  request.value = FAMO_ENGINE_ACTION_SELECT_CANDIDATE;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);
  request.value = FAMO_ENGINE_ACTION_PROCESS_KEY;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->action == FAMO_ENGINE_ACTION_PROCESS_KEY);
  CHECK(result->result_flags == 0);
  CHECK(result->handled == 1);
  CHECK(Equals(result->view.preedit, "a"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // Force the second host allocation (the final result) to fail after a
  // candidate selection. The emergency receipt survives, and recovery returns
  // the selected commit exactly once while select_count stays one.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  for (const uint32_t key : selection_keys) {
    request.key.virtual_key = key;
    CHECK(host.v2_api().execute_action(context, &request, &result) ==
          FAMO_ENGINE_OK);
    CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  }
  request = Request(FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  request.index = 1;
  FamoEngineHost::SetAllocationFailureCountdownForTesting(1);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  CHECK(result->result_flags == FAMO_ENGINE_RESULT_RESYNC_REQUIRED);
  CHECK(result->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  CHECK(result->handled == 1);
  CHECK(result->view.commit.length_bytes == 0);
  FamoUtf8String select_count_name = String("test_select_count");
  CHECK(host.GetOption(context, &select_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_RECOVER);
  request.value = FAMO_ENGINE_ACTION_SELECT_CANDIDATE;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->action == FAMO_ENGINE_ACTION_SELECT_CANDIDATE);
  CHECK(result->result_flags == 0);
  CHECK(Equals(result->view.commit, "\xE5\xB0\xBC"));  // 尼
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.GetOption(context, &select_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // COMMIT follows the same publish-failure path. A second RECOVER is rejected
  // and a later passive status cannot repeat the already-delivered commit.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'n';
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(1);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  CHECK(result->result_flags == FAMO_ENGINE_RESULT_RESYNC_REQUIRED);
  CHECK(result->handled == 1);
  FamoUtf8String commit_count_name = String("test_commit_count");
  CHECK(host.GetOption(context, &commit_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_RECOVER);
  request.value = FAMO_ENGINE_ACTION_COMMIT_COMPOSITION;
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->action == FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  CHECK(result->result_flags == 0);
  CHECK(Equals(result->view.commit, "n"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_INVALID_ARGUMENT);
  CHECK(result == nullptr);
  request = Request(FAMO_ENGINE_ACTION_STATUS);
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.commit.length_bytes == 0);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.GetOption(context, &commit_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // Ordinary page views use the ABI allocation budget, not PEEK's 64-item
  // control limit. A 65-candidate page validates at the product cap.
  CHECK(::SetEnvironmentVariableA("FAMO_TEST_CANDIDATE_COUNT", "65") != 0);
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(result->view.candidate_count == 65);
  CHECK(!FamoEngineHost::ValidateResultV2(
      result, FAMO_ENGINE_ACTION_PROCESS_KEY, 64,
      FAMO_ENGINE_V2_MAX_STRING_BYTES));
  CHECK(FamoEngineHost::ValidateResultV2(
      result, FAMO_ENGINE_ACTION_PROCESS_KEY,
      FAMO_ENGINE_V2_MAX_VIEW_CANDIDATES,
      FAMO_ENGINE_V2_MAX_STRING_BYTES));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA("FAMO_TEST_CANDIDATE_COUNT", nullptr) != 0);

  // A commit that violates the explicit ABI cap is rejected before mutation;
  // the composition and commit dispatch counter remain unchanged.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  const std::string oversized_commit =
      std::to_string(FAMO_ENGINE_V2_MAX_STRING_BYTES + 1u);
  CHECK(::SetEnvironmentVariableA("FAMO_TEST_COMMIT_BYTES",
                                  oversized_commit.c_str()) != 0);
  request = Request(FAMO_ENGINE_ACTION_COMMIT_COMPOSITION);
  result = nullptr;
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_E_RUNTIME);
  CHECK(result == nullptr);
  CHECK(host.GetOption(context, &commit_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 0);
  CHECK(::SetEnvironmentVariableA("FAMO_TEST_COMMIT_BYTES", nullptr) != 0);
  request = Request(FAMO_ENGINE_ACTION_STATUS);
  CHECK(host.v2_api().execute_action(context, &request, &result) ==
        FAMO_ENGINE_OK);
  CHECK(Equals(result->view.preedit, "a"));
  CHECK(host.FreeResultV2(result) == FAMO_ENGINE_OK);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // Consumer seam: one bounded RECOVER attempt can fail while retaining the
  // authoritative receipt outcome. A later control-only recovery succeeds and
  // the original PROCESS_KEY counter remains exactly one.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  FamoEngineActionResultLease lease;
  FamoEngineRecoveryOutcome recovery_outcome;
  FamoEngineHost::SetAllocationFailureCountdownForTesting(1);
  CHECK(host.ExecuteActionRecovering(context, &request, 1, &lease,
                                     &recovery_outcome) ==
        FAMO_ENGINE_E_RUNTIME);
  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  CHECK(!lease);
  CHECK(recovery_outcome.business_dispatched);
  CHECK(recovery_outcome.recovery_pending);
  CHECK(recovery_outcome.handled);
  CHECK(recovery_outcome.original_action ==
        FAMO_ENGINE_ACTION_PROCESS_KEY);
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  request = Request(FAMO_ENGINE_ACTION_RECOVER);
  request.value = FAMO_ENGINE_ACTION_PROCESS_KEY;
  CHECK(host.ExecuteAction(context, &request, &lease) == FAMO_ENGINE_OK);
  CHECK(lease);
  CHECK(lease->action == FAMO_ENGINE_ACTION_PROCESS_KEY);
  CHECK(lease->result_flags == 0);
  CHECK(lease->handled == 1);
  CHECK(Equals(lease->view.preedit, "a"));
  CHECK(lease.Reset() == FAMO_ENGINE_OK);
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  // A RESYNC receipt's UI view is untrusted and deliberately ignored. Once
  // its authenticated envelope says the business key was dispatched, a
  // malformed view must still lead to RECOVER rather than making the key look
  // replayable or leaving the context permanently recovery-required.
  CHECK(host.v2_api().create_context(&schema, &context) == FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_FAIL_POST_MUTATION_SNAPSHOT", "process") != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MALFORMED_RESYNC_RECEIPT", "1") != 0);
  request = Request(FAMO_ENGINE_ACTION_PROCESS_KEY);
  request.key.size = static_cast<uint32_t>(sizeof(request.key));
  request.key.is_key_down = 1;
  request.key.virtual_key = 'a';
  lease = {};
  recovery_outcome = {};
  CHECK(host.ExecuteActionRecovering(context, &request, 1, &lease,
                                     &recovery_outcome) ==
        FAMO_ENGINE_OK);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_FAIL_POST_MUTATION_SNAPSHOT", nullptr) != 0);
  CHECK(::SetEnvironmentVariableA(
            "FAMO_TEST_MALFORMED_RESYNC_RECEIPT", nullptr) != 0);
  CHECK(recovery_outcome.business_dispatched);
  CHECK(!recovery_outcome.recovery_pending);
  CHECK(recovery_outcome.handled);
  CHECK(lease);
  CHECK(lease->action == FAMO_ENGINE_ACTION_PROCESS_KEY);
  CHECK(lease->result_flags == 0);
  CHECK(Equals(lease->view.preedit, "a"));
  CHECK(host.GetOption(context, &process_count_name, &action_count) ==
        FAMO_ENGINE_OK);
  CHECK(action_count == 1);
  CHECK(lease.Reset() == FAMO_ENGINE_OK);
  CHECK(host.DestroyContext(context) == FAMO_ENGINE_OK);

  FamoEngineHost::SetAllocationFailureCountdownForTesting(-1);
  host.Unload();
  std::printf("action_v2_selfcheck: OK\n");
  return 0;
}
