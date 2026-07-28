// Public-DLL ABI canary for a new engine serving a pre-v1.2 host.
//
// The legacy host owns only sizeof(LegacyCompositionView) bytes and indexes
// candidates with sizeof(LegacyCandidate).  Guard bytes and a tracked host
// allocator make any wider query/free access deterministic instead of relying
// on a crash.
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "../famo_engine_api.h"

namespace {

constexpr unsigned char kViewCanary = 0xD7;
constexpr unsigned char kAllocationCanary = 0xB9;
constexpr size_t kViewGuardBytes = 64;
constexpr size_t kAllocationGuardBytes = 192;
constexpr uint32_t kV10RequiredSpan = static_cast<uint32_t>(
    offsetof(FamoCompositionView, preedit_sel_start));
constexpr uint32_t kV12RequiredSpan = static_cast<uint32_t>(
    offsetof(FamoCompositionView, is_last_page) + sizeof(uint32_t));
constexpr uint32_t kV11FieldSpan =
    static_cast<uint32_t>(offsetof(FamoCompositionView, is_last_page));
constexpr uint32_t kLegacyCandidateStride =
    static_cast<uint32_t>(offsetof(FamoCandidate, label));
constexpr const char* kRimeFixtureSchema = "famo_abi_canary";

struct LegacyCandidate {
  uint32_t size;
  FamoUtf8String text;
  FamoUtf8String comment;
  uint32_t quality;
  uint32_t flags;
};

struct LegacyCompositionView {
  uint32_t size;
  FamoUtf8String preedit;
  FamoUtf8String commit;
  const LegacyCandidate* candidates;
  uint32_t candidate_count;
  uint32_t highlighted_index;
  uint32_t page_index;
  uint32_t page_size;
  uint32_t state_flags;
};

static_assert(sizeof(LegacyCandidate) == kLegacyCandidateStride,
              "legacy candidate stride must end where v1.2 label begins");
static_assert(sizeof(LegacyCompositionView) >= kV10RequiredSpan,
              "legacy view must span every v1.0 field");
static_assert(sizeof(LegacyCompositionView) < kV12RequiredSpan,
              "legacy view must not advertise the v1.2 candidate layout");
static_assert(kV11FieldSpan < kV12RequiredSpan,
              "v1.1 field span must select the legacy candidate stride");

struct Allocation {
  unsigned char* bytes;
  size_t payload_size;
};

std::unordered_map<void*, Allocation> g_allocations;
int g_invalid_frees = 0;
int g_damaged_allocation_guards = 0;
int g_failures = 0;
int64_t g_allocations_before_failure = -1;

void Expect(bool condition, const char* expression, int line) {
  if (condition) return;
  std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expression, __FILE__,
               line);
  ++g_failures;
}

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

void* FAMO_ENGINE_CALL TrackedAlloc(size_t bytes) {
  if (g_allocations_before_failure == 0)
    return nullptr;
  if (g_allocations_before_failure > 0)
    --g_allocations_before_failure;
  auto* allocation = static_cast<unsigned char*>(
      std::malloc(bytes + kAllocationGuardBytes));
  if (!allocation) return nullptr;
  std::memset(allocation + bytes, kAllocationCanary,
              kAllocationGuardBytes);
  g_allocations.emplace(allocation, Allocation{allocation, bytes});
  return allocation;
}

bool AllocationGuardIntact(const Allocation& allocation) {
  for (size_t i = 0; i < kAllocationGuardBytes; ++i) {
    if (allocation.bytes[allocation.payload_size + i] !=
        kAllocationCanary) {
      return false;
    }
  }
  return true;
}

void FAMO_ENGINE_CALL TrackedFree(void* pointer) {
  if (!pointer) return;
  const auto it = g_allocations.find(pointer);
  if (it == g_allocations.end()) {
    ++g_invalid_frees;
    return;
  }
  if (!AllocationGuardIntact(it->second))
    ++g_damaged_allocation_guards;
  std::free(it->second.bytes);
  g_allocations.erase(it);
}

void FAMO_ENGINE_CALL IgnoreLog(int32_t, const FamoUtf8String*,
                                const FamoUtf8String*) {}

size_t AllocationSize(const void* pointer) {
  const auto it = g_allocations.find(const_cast<void*>(pointer));
  return it == g_allocations.end() ? 0 : it->second.payload_size;
}

void CleanupOutstandingAllocations() {
  for (const auto& item : g_allocations)
    std::free(item.second.bytes);
  g_allocations.clear();
}

struct GuardedView {
  alignas(FamoCompositionView)
      std::array<unsigned char,
                 sizeof(FamoCompositionView) + kViewGuardBytes>
          bytes{};

  FamoCompositionView* Reset(uint32_t caller_size) {
    bytes.fill(kViewCanary);
    const size_t clear_size =
        caller_size < bytes.size() ? caller_size : bytes.size();
    std::memset(bytes.data(), 0, clear_size);
    std::memcpy(bytes.data(), &caller_size, sizeof(caller_size));
    return View();
  }

  FamoCompositionView* View() {
    return reinterpret_cast<FamoCompositionView*>(bytes.data());
  }

  bool UnchangedFrom(size_t offset) const {
    for (size_t i = offset; i < bytes.size(); ++i) {
      if (bytes[i] != kViewCanary) return false;
    }
    return true;
  }
};

using CreateFn = int32_t(FAMO_ENGINE_CALL*)(uint32_t, FamoEngineApi*);

FamoKeyEvent KeyDown(uint32_t key) {
  FamoKeyEvent event{};
  event.size = static_cast<uint32_t>(sizeof(event));
  event.virtual_key = key;
  event.is_key_down = 1;
  return event;
}

bool StringEquals(const FamoUtf8String& value, const char* expected) {
  const size_t length = std::strlen(expected);
  return value.size == sizeof(FamoUtf8String) && value.data &&
         value.length_bytes == length &&
         std::memcmp(value.data, expected, length) == 0;
}

void CheckSuccessfulQuery(FamoEngineApi& api, GuardedView& guarded,
                          uint32_t caller_size, int32_t result) {
  EXPECT(result == FAMO_ENGINE_OK);
  if (result != FAMO_ENGINE_OK) return;
  EXPECT(guarded.View()->size == caller_size);
  EXPECT(guarded.UnchangedFrom(caller_size));

  const int invalid_before = g_invalid_frees;
  const int damaged_before = g_damaged_allocation_guards;
  EXPECT(api.free_view(guarded.View()) == FAMO_ENGINE_OK);
  EXPECT(guarded.View()->size == caller_size);
  EXPECT(guarded.UnchangedFrom(caller_size));
  EXPECT(g_invalid_frees == invalid_before);
  EXPECT(g_damaged_allocation_guards == damaged_before);
  EXPECT(g_allocations.empty());
}

bool RecreateContext(FamoEngineApi& api, FamoEngineContext** context,
                     const char* schema_id) {
  if (*context) {
    EXPECT(api.destroy_context(*context) == FAMO_ENGINE_OK);
    *context = nullptr;
  }
  FamoUtf8String schema{
      sizeof(FamoUtf8String), schema_id,
      static_cast<uint32_t>(std::strlen(schema_id))};
  const int32_t result = api.create_context(&schema, context);
  EXPECT(result == FAMO_ENGINE_OK);
  EXPECT(*context != nullptr);
  return result == FAMO_ENGINE_OK && *context;
}

bool WriteTextFile(const std::filesystem::path& path, const char* contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  return output.good();
}

bool WriteRimeFixture(const std::filesystem::path& data_root) {
  const bool default_written = WriteTextFile(
      data_root / "default.yaml",
      "config_version: \"0.40\"\n"
      "schema_list:\n"
      "  - schema: famo_abi_canary\n");
  const bool schema_written = WriteTextFile(
      data_root / "famo_abi_canary.schema.yaml",
      "schema:\n"
      "  schema_id: famo_abi_canary\n"
      "  name: Famo ABI Canary\n"
      "  version: \"1.0\"\n"
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
      "  dictionary: famo_abi_canary\n"
      "  enable_completion: false\n"
      "  enable_sentence: false\n"
      "  enable_user_dict: false\n");
  const bool dictionary_written = WriteTextFile(
      data_root / "famo_abi_canary.dict.yaml",
      "---\n"
      "name: famo_abi_canary\n"
      "version: \"1.0\"\n"
      "sort: by_weight\n"
      "columns:\n"
      "  - text\n"
      "  - code\n"
      "  - weight\n"
      "...\n"
      "\xE4\xBD\xA0\tni\t100\n"
      "\xE5\xB0\xBC\tni\t90\n");
  return default_written && schema_written && dictionary_written;
}

void CheckTooSmallQuery(FamoEngineApi& api, FamoEngineContext* context,
                        bool deterministic_test_engine) {
  GuardedView guarded;
  const uint32_t too_small = kV10RequiredSpan - 1;
  FamoCompositionView* view = guarded.Reset(too_small);
  const auto before = guarded.bytes;
  const FamoKeyEvent key = KeyDown('x');
  const int32_t result = api.process_key(context, &key, view);
  EXPECT(result == FAMO_ENGINE_E_INVALID_ARGUMENT);
  EXPECT(guarded.bytes == before);
  if (result == FAMO_ENGINE_OK)
    (void)api.free_view(view);

  if (!deterministic_test_engine) return;
  guarded.Reset(static_cast<uint32_t>(sizeof(LegacyCompositionView)));
  const int32_t status_result = api.get_status(context, guarded.View());
  EXPECT(status_result == FAMO_ENGINE_OK);
  if (status_result == FAMO_ENGINE_OK) {
    EXPECT(guarded.View()->preedit.length_bytes == 0);
    (void)api.free_view(guarded.View());
  }
  EXPECT(g_allocations.empty());
}

void CheckTooSmallFree(FamoEngineApi& api) {
  GuardedView guarded;
  const uint32_t too_small = kV10RequiredSpan - 1;
  FamoCompositionView* view = guarded.Reset(too_small);
  const auto before = guarded.bytes;
  EXPECT(api.free_view(view) == FAMO_ENGINE_E_INVALID_ARGUMENT);
  EXPECT(guarded.bytes == before);
}

void CheckLegacyCandidateStride(FamoEngineApi& api,
                                FamoEngineContext* context,
                                uint32_t caller_size) {
  GuardedView guarded;
  FamoKeyEvent key = KeyDown('n');
  guarded.Reset(caller_size);
  CheckSuccessfulQuery(api, guarded, caller_size,
                       api.process_key(context, &key, guarded.View()));

  key = KeyDown('i');
  guarded.Reset(caller_size);
  const int32_t result = api.process_key(context, &key, guarded.View());
  EXPECT(result == FAMO_ENGINE_OK);
  if (result != FAMO_ENGINE_OK) return;

  EXPECT(guarded.View()->size == caller_size);
  EXPECT(guarded.UnchangedFrom(caller_size));
  EXPECT(guarded.View()->candidate_count >= 2);
  EXPECT(guarded.View()->candidates != nullptr);
  const size_t expected_bytes =
      static_cast<size_t>(guarded.View()->candidate_count) *
      kLegacyCandidateStride;
  const bool packed =
      AllocationSize(guarded.View()->candidates) == expected_bytes;
  EXPECT(packed);
  if (packed && guarded.View()->candidate_count >= 2) {
    const auto* candidates = reinterpret_cast<const LegacyCandidate*>(
        guarded.View()->candidates);
    EXPECT(candidates[0].size == kLegacyCandidateStride);
    EXPECT(candidates[1].size == kLegacyCandidateStride);
    EXPECT(StringEquals(candidates[0].text, "\xE4\xBD\xA0"));  // 你
    EXPECT(StringEquals(candidates[1].text, "\xE5\xB0\xBC"));  // 尼
  }

  const int invalid_before = g_invalid_frees;
  const int damaged_before = g_damaged_allocation_guards;
  EXPECT(api.free_view(guarded.View()) == FAMO_ENGINE_OK);
  EXPECT(guarded.View()->size == caller_size);
  EXPECT(guarded.UnchangedFrom(caller_size));
  EXPECT(g_invalid_frees == invalid_before);
  EXPECT(g_damaged_allocation_guards == damaged_before);
  EXPECT(g_allocations.empty());
}

void CheckOtherQuerySeams(FamoEngineApi& api, FamoEngineContext* context) {
  GuardedView guarded;
  const uint32_t caller_size =
      static_cast<uint32_t>(sizeof(LegacyCompositionView));
  guarded.Reset(caller_size);
  CheckSuccessfulQuery(api, guarded, caller_size,
                       api.get_status(context, guarded.View()));

  guarded.Reset(caller_size);
  CheckSuccessfulQuery(
      api, guarded, caller_size,
      api.peek_candidates(context, 0, 0, guarded.View()));

  guarded.Reset(caller_size);
  const int invalid_before = g_invalid_frees;
  const int damaged_before = g_damaged_allocation_guards;
  EXPECT(api.free_view(guarded.View()) == FAMO_ENGINE_OK);
  EXPECT(guarded.View()->size == caller_size);
  EXPECT(guarded.UnchangedFrom(caller_size));
  EXPECT(g_invalid_frees == invalid_before);
  EXPECT(g_damaged_allocation_guards == damaged_before);
  EXPECT(g_allocations.empty());
}

void CheckRimeAllocationFailures(FamoEngineApi& api,
                                 FamoEngineContext* context) {
  GuardedView guarded;
  const uint32_t caller_size =
      static_cast<uint32_t>(sizeof(FamoCompositionView));
  const auto expect_status_failure = [&](int64_t allocations) {
    FamoCompositionView* view = guarded.Reset(caller_size);
    const auto before = guarded.bytes;
    g_allocations_before_failure = allocations;
    const int32_t result = api.get_status(context, view);
    g_allocations_before_failure = -1;
    EXPECT(result == FAMO_ENGINE_E_RUNTIME);
    EXPECT(guarded.bytes == before);
    EXPECT(g_allocations.empty());
  };

  // Fail preedit, candidate-array, and first candidate-string allocation.
  // Every path must return an error without publishing a partial view and
  // release allocations made before the fault.
  expect_status_failure(0);
  expect_status_failure(2);
  expect_status_failure(3);

  // A commit query is consumptive. If its very first copy fails, the v1
  // adapter must still fail the call and leave no half-owned output.
  FamoKeyEvent commit = KeyDown(' ');
  FamoCompositionView* view = guarded.Reset(caller_size);
  const auto before = guarded.bytes;
  g_allocations_before_failure = 0;
  const int32_t result = api.process_key(context, &commit, view);
  g_allocations_before_failure = -1;
  EXPECT(result == FAMO_ENGINE_E_RUNTIME);
  EXPECT(guarded.bytes == before);
  EXPECT(g_allocations.empty());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr,
                 "usage: view_abi_canary <engine.dll> [--test-engine]\n");
    return 2;
  }
  const bool deterministic_test_engine =
      argc == 3 && std::strcmp(argv[2], "--test-engine") == 0;

  namespace fs = std::filesystem;
  const fs::path data_root =
      fs::temp_directory_path() /
      ("famo-view-abi-" + std::to_string(::GetCurrentProcessId()));
  std::error_code error;
  fs::create_directories(data_root, error);
  if (error || !WriteRimeFixture(data_root)) {
    std::fprintf(stderr, "failed to write Rime fixture\n");
    fs::remove_all(data_root, error);
    return 1;
  }

  const std::wstring dll_path = fs::path(argv[1]).wstring();
  HMODULE module = ::LoadLibraryW(dll_path.c_str());
  if (!module) {
    std::fprintf(stderr, "LoadLibraryW failed: %lu\n", ::GetLastError());
    fs::remove_all(data_root, error);
    return 1;
  }
  auto create = reinterpret_cast<CreateFn>(
      ::GetProcAddress(module, "FamoCreateEngineApi"));
  if (!create) {
    std::fprintf(stderr, "FamoCreateEngineApi export missing\n");
    ::FreeLibrary(module);
    fs::remove_all(data_root, error);
    return 1;
  }

  FamoEngineApi api{};
  api.size = static_cast<uint32_t>(sizeof(api));
  if (create(FAMO_ENGINE_ABI_VERSION, &api) != FAMO_ENGINE_OK) {
    std::fprintf(stderr, "FamoCreateEngineApi failed\n");
    ::FreeLibrary(module);
    fs::remove_all(data_root, error);
    return 1;
  }

  FamoEngineHostApi host{};
  host.size = static_cast<uint32_t>(sizeof(host));
  host.abi_version = FAMO_ENGINE_ABI_VERSION;
  host.alloc = &TrackedAlloc;
  host.free = &TrackedFree;
  host.log = &IgnoreLog;
  const std::string root = data_root.string();
  FamoUtf8String root_view{sizeof(FamoUtf8String), root.c_str(),
                           static_cast<uint32_t>(root.size())};
  if (api.initialize(&host, &root_view) != FAMO_ENGINE_OK) {
    std::fprintf(stderr, "engine initialize failed\n");
    ::FreeLibrary(module);
    fs::remove_all(data_root, error);
    return 1;
  }

  FamoEngineContext* malformed_context =
      reinterpret_cast<FamoEngineContext*>(
          static_cast<uintptr_t>(1));
  FamoUtf8String huge_declared{
      sizeof(FamoUtf8String), "x",
      FAMO_ENGINE_V2_MAX_STRING_BYTES + 1u};
  EXPECT(api.create_context(&huge_declared, &malformed_context) ==
         FAMO_ENGINE_E_INVALID_ARGUMENT);
  EXPECT(malformed_context == nullptr);
  const char invalid_utf8_bytes[] = {
      static_cast<char>(0xc0), static_cast<char>(0xaf)};
  FamoUtf8String invalid_utf8{
      sizeof(FamoUtf8String), invalid_utf8_bytes, 2};
  malformed_context =
      reinterpret_cast<FamoEngineContext*>(
          static_cast<uintptr_t>(1));
  EXPECT(api.create_context(&invalid_utf8, &malformed_context) ==
         FAMO_ENGINE_E_INVALID_ARGUMENT);
  EXPECT(malformed_context == nullptr);
  FamoUtf8String null_nonempty{
      sizeof(FamoUtf8String), nullptr, 1};
  malformed_context =
      reinterpret_cast<FamoEngineContext*>(
          static_cast<uintptr_t>(1));
  EXPECT(api.create_context(&null_nonempty, &malformed_context) ==
         FAMO_ENGINE_E_INVALID_ARGUMENT);
  EXPECT(malformed_context == nullptr);

  const char* context_schema =
      deterministic_test_engine ? "" : kRimeFixtureSchema;
  bool fixture_ready = true;
  if (!deterministic_test_engine) {
    FamoUtf8String schema{
        sizeof(FamoUtf8String), context_schema,
        static_cast<uint32_t>(std::strlen(context_schema))};
    FamoUtf8String deploy_error{sizeof(FamoUtf8String), nullptr, 0};
    const int32_t deploy_result =
        api.deploy_schema(&schema, &deploy_error);
    EXPECT(deploy_result == FAMO_ENGINE_OK);
    fixture_ready = deploy_result == FAMO_ENGINE_OK;
  }

  FamoEngineContext* context = nullptr;
  if (fixture_ready && RecreateContext(api, &context, context_schema)) {
    CheckTooSmallQuery(api, context, deterministic_test_engine);
    if (RecreateContext(api, &context, context_schema)) {
      CheckTooSmallFree(api);
      CheckLegacyCandidateStride(
          api, context,
          static_cast<uint32_t>(sizeof(LegacyCompositionView)));
      if (RecreateContext(api, &context, context_schema))
        CheckLegacyCandidateStride(api, context, kV11FieldSpan);
      CheckOtherQuerySeams(api, context);
      if (!deterministic_test_engine)
        CheckRimeAllocationFailures(api, context);
    }
  }

  if (context)
    EXPECT(api.destroy_context(context) == FAMO_ENGINE_OK);
  EXPECT(g_allocations.empty());
  EXPECT(api.shutdown() == FAMO_ENGINE_OK);
  ::FreeLibrary(module);
  fs::remove_all(data_root, error);

  if (!g_allocations.empty())
    CleanupOutstandingAllocations();
  if (g_failures != 0) {
    std::fprintf(stderr,
                 "view_abi_canary: FAIL (%d checks, invalid_frees=%d, "
                 "damaged_guards=%d)\n",
                 g_failures, g_invalid_frees,
                 g_damaged_allocation_guards);
    return 1;
  }
  std::printf("view_abi_canary: OK (%s)\n",
              deterministic_test_engine ? "FamoTestEngine"
                                        : "FamoRimeEngine");
  return 0;
}
