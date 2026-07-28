#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace famo::runtime {

constexpr uint32_t kProtocolMagic = 0x4f4d4146; // "FAMO" little-endian.
constexpr uint16_t kProtocolVersion = 2;
constexpr uint16_t kHeaderSize = 76;
constexpr uint32_t kMaxCandidateCount = 64;
constexpr uint32_t kMaxStringBytes = 1024 * 1024;
// ABI v2 permits one engine-owned result allocation up to 8 MiB. The wire
// budget adds every composition-only field and string-length prefix so even a
// legal allocation at that boundary remains encodable without dropping a
// handled/commit result.
constexpr uint32_t kMaxEngineResultBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaxCompositionWireOverhead =
    4 + (5 * 4) + 4 + (kMaxCandidateCount * ((3 * 4) + (2 * 4))) + (9 * 4);
constexpr uint32_t kMaxFramePayloadSize =
    kMaxEngineResultBytes + kMaxCompositionWireOverhead;
constexpr uint32_t kMaxFrameSize = kHeaderSize + kMaxFramePayloadSize;
constexpr uint16_t kFlagResponse = 1;
// On a prepared business request, confirms that the sole completed delivery
// for the same logical session has already been applied by the TSF host.
constexpr uint16_t kFlagAcknowledgePrevious = 1u << 1;
// Host-only behavior bits carried in Composition::state_flags. Engine ABI bits
// occupy the low range; these high bits are applied by the clean-room TSF host.
constexpr uint32_t kHostInlinePreedit = 1u << 24;
constexpr uint32_t kHostCandidatePreview = 1u << 25;
constexpr uint32_t kHostAutoPair = 1u << 26;
constexpr uint32_t kHostCjkEnglishSpacing = 1u << 27;
constexpr uint32_t kHostCjkNumberSpacing = 1u << 28;
constexpr uint32_t kHostPreviewPages = 1u << 29;
constexpr uint32_t kHostPreviewRowsTwo = 1u << 30;
constexpr uint32_t kHostRimeVertical = 1u << 31;

enum class Command : uint16_t {
  Hello = 1,
  OpenSession = 2,
  ProcessKey = 3,
  SelectCandidate = 4,
  CloseSession = 5,
  CommitComposition = 6,
  ClearComposition = 7,
  HighlightCandidate = 8,
  ChangePage = 9,
  UpdateUiState = 10,
  ControlReloadStyle = 11,
  ControlReloadOptions = 12,
  ControlSelectSchema = 13,
  ControlDeploy = 14,
  ControlStatus = 15,
  ControlShutdown = 16,
  ControlResetUserDictionary = 17,
  SelectCandidateAbsolute = 18,
  ExecutePrepared = 19,
  ClaimResult = 20,
  AckResult = 21,
  // Terminal, authenticated teardown for a TSF activation that can no longer
  // apply an outstanding result. It never represents a successful host apply.
  AbandonConnection = 22,
  // Terminal, authenticated teardown for one exact delivery and logical
  // session. The payload is DeliveryReference and the frame correlation is
  // the owning connection identity. Sibling sessions in that epoch survive.
  AbandonSession = 23,
};

enum class Status : uint32_t {
  Ok = 0,
  InvalidFrame = 1,
  StaleRequest = 2,
  EngineError = 3,
  Unavailable = 4,
  Timeout = 5,
  WrongPeer = 6,
  Prepared = 7,
  // The business mutation has happened, but the engine still owes the exact
  // final snapshot. Claiming this delivery may only issue RECOVER.
  RecoveryPending = 8,
  // The runtime can no longer produce the authoritative final snapshot for
  // this delivery. Its authenticated logical session and exact delivery must
  // be abandoned; sibling sessions survive, and the business action must never
  // be replayed or treated as unhandled.
  DeliveryFailed = 9,
};

enum class ControlState : uint32_t {
  Pending = 1,
  Running = 2,
  Succeeded = 3,
  Failed = 4,
};

enum class ControlError : uint32_t {
  None = 0,
  InvalidOperation = 1,
  QueueFull = 2,
  Config = 3,
  Engine = 4,
  Runtime = 5,
  UserDictionaryEnumeration = 6,
  UserDictionaryRollback = 7,
};

enum class RuntimeReadiness : uint32_t {
  Starting = 0,
  Ready = 1,
  Maintenance = 2,
  Unavailable = 3,
  Stopping = 4,
};

struct Correlation {
  uint64_t client_id = 0;
  uint64_t activation_generation = 0;
  uint64_t connection_generation = 0;
  // Within one client connection epoch, each new logical session uses a
  // strictly increasing ID. Retrying an already-open session is idempotent;
  // an older ID is a stale replay even after its bounded tombstone expires.
  uint64_t session_id = 0;
  uint64_t session_generation = 0;
  uint64_t sequence = 0;

  bool operator==(const Correlation &) const = default;
};

struct DeliveryReference {
  Command command = Command::Hello;
  Correlation correlation;

  bool operator==(const DeliveryReference &) const = default;
};

struct Frame {
  Command command = Command::Hello;
  uint16_t flags = 0;
  Status status = Status::Ok;
  Correlation correlation;
  std::vector<uint8_t> payload;
};

struct KeyEvent {
  uint32_t virtual_key = 0;
  uint32_t scan_code = 0;
  uint32_t modifiers = 0;
  uint32_t is_key_down = 0;
  uint64_t timestamp_ms = 0;

  bool operator==(const KeyEvent &) const = default;
};

struct UiRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;

  bool operator==(const UiRect &) const = default;
};

struct SelectionCapability {
  uint64_t low = 0;
  uint64_t high = 0;

  bool operator==(const SelectionCapability &) const = default;
  explicit operator bool() const { return low != 0 || high != 0; }
};

bool SelectionCapabilityMatches(const SelectionCapability &left,
                                const SelectionCapability &right) noexcept;

struct UiState {
  UiRect caret;
  UiRect work_area;
  uint32_t dpi = 96;
  bool layout_available = false;
  bool focused = false;
  bool show_allowed = false;
  SelectionCapability selection_capability;

  bool operator==(const UiState &) const = default;
};

struct Candidate {
  std::string text;
  std::string comment;
  std::string label;
  uint32_t quality = 0;
  uint32_t flags = 0;

  bool operator==(const Candidate &) const = default;
};

struct Composition {
  bool handled = false;
  std::string preedit;
  std::string commit;
  std::string commit_preview;
  std::string schema_id;
  std::string schema_name;
  std::vector<Candidate> candidates;
  // UI-only next-page candidates. They stay inside FamoRuntime snapshots and
  // are deliberately not serialized back across the TSF request pipe.
  std::vector<Candidate> preview_candidates;
  uint32_t highlighted_index = 0;
  uint32_t page_index = 0;
  uint32_t page_size = 0;
  uint32_t state_flags = 0;
  uint32_t preedit_sel_start = 0;
  uint32_t preedit_sel_end = 0;
  uint32_t preedit_cursor_pos = 0;
  uint32_t status_flags = 0;
  uint32_t is_last_page = 0;

  bool operator==(const Composition &) const = default;
};

struct ControlResult {
  uint64_t operation_id = 0;
  ControlState state = ControlState::Pending;
  ControlError error = ControlError::None;
  bool retryable = false;
  RuntimeReadiness readiness = RuntimeReadiness::Unavailable;
  uint64_t engine_generation = 0;

  bool operator==(const ControlResult &) const = default;
};

inline constexpr wchar_t kPreviewSelectionWindowClass[] =
    L"FamoTsfPreviewSelectionWindow";
constexpr uint64_t kPreviewSelectionCopyDataId = 0x314c45534f4d4146ull;

struct PreviewSelectionRequest {
  Correlation correlation;
  uint64_t composition_sequence = 0;
  uint32_t absolute_index = 0;
  uint32_t reserved = 0;
  SelectionCapability selection_capability;
};

uint32_t Crc32(std::span<const uint8_t> bytes);
bool IsValidUtf8(std::string_view text);
bool PeekFrameSize(std::span<const uint8_t> header, uint32_t *size,
                   std::string *error) noexcept;
bool EncodeFrame(const Frame &frame, std::vector<uint8_t> *bytes,
                 std::string *error) noexcept;
bool DecodeFrame(std::span<const uint8_t> bytes, Frame *frame,
                 std::string *error) noexcept;

bool EncodeOpenSession(std::string_view schema, std::vector<uint8_t> *payload,
                       std::string *error) noexcept;
bool DecodeOpenSession(std::span<const uint8_t> payload, std::string *schema,
                       std::string *error) noexcept;
bool EncodeKeyEvent(const KeyEvent &key,
                    std::vector<uint8_t> *payload) noexcept;
bool DecodeKeyEvent(std::span<const uint8_t> payload, KeyEvent *key,
                    std::string *error) noexcept;
bool EncodeCandidateIndex(uint32_t index,
                          std::vector<uint8_t> *payload) noexcept;
bool DecodeCandidateIndex(std::span<const uint8_t> payload, uint32_t *index,
                          std::string *error) noexcept;
bool EncodeAbsoluteCandidateSelection(uint32_t index,
                                      uint64_t composition_sequence,
                                      std::vector<uint8_t> *payload) noexcept;
bool DecodeAbsoluteCandidateSelection(std::span<const uint8_t> payload,
                                      uint32_t *index,
                                      uint64_t *composition_sequence,
                                      std::string *error) noexcept;
bool EncodeDeliveryReference(const DeliveryReference &reference,
                             std::vector<uint8_t> *payload) noexcept;
bool DecodeDeliveryReference(std::span<const uint8_t> payload,
                             DeliveryReference *reference,
                             std::string *error) noexcept;
bool EncodePageDirection(bool backward,
                         std::vector<uint8_t> *payload) noexcept;
bool DecodePageDirection(std::span<const uint8_t> payload, bool *backward,
                         std::string *error) noexcept;
bool EncodeUiState(const UiState &state, std::vector<uint8_t> *payload,
                   std::string *error) noexcept;
bool DecodeUiState(std::span<const uint8_t> payload, UiState *state,
                   std::string *error) noexcept;
bool EncodeComposition(const Composition &composition,
                       std::vector<uint8_t> *payload,
                       std::string *error) noexcept;
bool DecodeComposition(std::span<const uint8_t> payload,
                       Composition *composition, std::string *error) noexcept;
bool EncodeControlOperationId(uint64_t operation_id,
                              std::vector<uint8_t> *payload) noexcept;
bool DecodeControlOperationId(std::span<const uint8_t> payload,
                              uint64_t *operation_id,
                              std::string *error) noexcept;
bool EncodeControlResult(const ControlResult &result,
                         std::vector<uint8_t> *payload,
                         std::string *error) noexcept;
bool DecodeControlResult(std::span<const uint8_t> payload,
                         ControlResult *result, std::string *error) noexcept;

bool IsControlOperation(Command command);
bool IsDeliveryTracked(Command command);

} // namespace famo::runtime
