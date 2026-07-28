#include <cstdio>
#include <type_traits>

#include "famo_install_state.h"
#include "famo_runtime_protocol.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

int main() {
  CHECK(InstallTargetAllowed(L"Ready", L"C:\\Program Files\\Famo\\v1\\",
                             L"c:\\program files\\famo\\v1"));
  CHECK(InstallTargetAllowed(L"Activating", L"C:\\Famo\\v2", L"C:\\Famo\\v2",
                             true));
  CHECK(!InstallTargetAllowed(L"Activating", L"C:\\Famo\\v2", L"C:\\Famo\\v2"));
  CHECK(!InstallTargetAllowed(L"PendingReboot", L"C:\\Famo\\v2", L"C:\\Famo\\v2",
                              true));
  CHECK(!InstallTargetAllowed(L"Ready", L"C:\\Famo\\v1", L"C:\\Famo\\v2"));

  std::string error;
  std::vector<uint8_t> payload;
  CHECK(EncodeOpenSession("test", &payload, &error));

  Frame source;
  source.command = Command::OpenSession;
  source.correlation = {1, 2, 3, 4, 5, 6};
  source.payload = payload;
  std::vector<uint8_t> bytes;
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(bytes.size() == kHeaderSize + payload.size());

  Frame parsed;
  CHECK(DecodeFrame(bytes, &parsed, &error));
  CHECK(parsed.command == source.command);
  CHECK(parsed.correlation == source.correlation);
  CHECK(kProtocolVersion == 2);
  auto v1 = bytes;
  v1[4] = 1;
  v1[5] = 0;
  CHECK(!DecodeFrame(v1, &parsed, &error));
  std::string schema;
  CHECK(DecodeOpenSession(parsed.payload, &schema, &error));
  CHECK(schema == "test");

  for (size_t i = 0; i < bytes.size(); ++i)
    CHECK(!DecodeFrame(std::span(bytes).first(i), &parsed, &error));

  auto bad = bytes;
  bad[0] ^= 0xff;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[4] = 99;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[6] = 0;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[8] = 1;
  bad[9] = 0;
  bad[10] = 1;
  bad[11] = 0;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[12] = 0xff;
  bad[13] = 0xff;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[14] = 0x80;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[16] = 0xff;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad[72] = 1;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad.back() ^= 0x01;
  CHECK(!DecodeFrame(bad, &parsed, &error));
  bad = bytes;
  bad.push_back(0);
  CHECK(!DecodeFrame(bad, &parsed, &error));

  CHECK(kMaxCompositionWireOverhead == 1344);
  CHECK(kMaxFramePayloadSize == 8389952);
  CHECK(kMaxFrameSize == 8390028);
  source.payload.assign(kMaxFramePayloadSize, 0);
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(bytes.size() == kMaxFrameSize);
  source.payload.push_back(0);
  CHECK(!EncodeFrame(source, &bytes, &error));
  source.payload.clear();
  CHECK(EncodeOpenSession(std::string(kMaxStringBytes, 'a'), &payload, &error));
  CHECK(!EncodeOpenSession(std::string(kMaxStringBytes + 1, 'a'), &payload,
                           &error));

  CHECK(EncodePageDirection(true, &payload));
  bool backward = false;
  CHECK(DecodePageDirection(payload, &backward, &error));
  CHECK(backward);
  payload.push_back(0);
  CHECK(!DecodePageDirection(payload, &backward, &error));
  payload = {2, 0, 0, 0};
  CHECK(!DecodePageDirection(payload, &backward, &error));

  const UiState ui_state{{10, 20, 11, 40},
                         {-1920, 0, 0, 1080},
                         144,
                         true,
                         true,
                         false,
                         {0x1122334455667788ull, 0x8877665544332211ull}};
  CHECK(SelectionCapabilityMatches(ui_state.selection_capability,
                                   ui_state.selection_capability));
  CHECK(!SelectionCapabilityMatches(ui_state.selection_capability, {}));
  CHECK(!SelectionCapabilityMatches(
      ui_state.selection_capability,
      {ui_state.selection_capability.low ^ 1,
       ui_state.selection_capability.high}));
  CHECK(EncodeUiState(ui_state, &payload, &error));
  UiState decoded_ui_state;
  CHECK(DecodeUiState(payload, &decoded_ui_state, &error));
  CHECK(decoded_ui_state == ui_state);
  payload[39] = 0x80;
  CHECK(!DecodeUiState(payload, &decoded_ui_state, &error));
  UiState invalid_ui_state = ui_state;
  invalid_ui_state.caret.right = invalid_ui_state.caret.left - 1;
  CHECK(!EncodeUiState(invalid_ui_state, &payload, &error));

  std::vector<uint8_t> invalid_utf8 = {2, 0, 0, 0, 0xc0, 0xaf};
  source.payload = invalid_utf8;
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(DecodeFrame(bytes, &parsed, &error));
  CHECK(!DecodeOpenSession(parsed.payload, &schema, &error));
  invalid_utf8 = {1, 0, 0, 0, 'a', 0};
  CHECK(!DecodeOpenSession(invalid_utf8, &schema, &error));

  Composition composition;
  composition.handled = true;
  composition.preedit = "ni";
  composition.commit_preview = "\xe4\xbd\xa0";
  composition.schema_id = "test";
  composition.schema_name = "Test";
  composition.candidates.push_back({"\xe4\xbd\xa0", "candidate", "1", 10, 1});
  composition.highlighted_index = 0;
  composition.page_size = 5;
  // HAS_PREEDIT | HAS_CANDIDATES | HANDLED.
  composition.state_flags = 13;
  composition.preedit_sel_end = 2;
  composition.preedit_cursor_pos = 2;
  composition.status_flags = 2;
  composition.is_last_page = 1;
  CHECK(EncodeComposition(composition, &payload, &error));
  Composition decoded;
  CHECK(DecodeComposition(payload, &decoded, &error));
  CHECK(decoded == composition);
  const auto valid_composition_payload = payload;
  const auto write_u32 = [](std::vector<uint8_t> *target, size_t offset,
                            uint32_t value) {
    (*target)[offset] = static_cast<uint8_t>(value);
    (*target)[offset + 1] = static_cast<uint8_t>(value >> 8);
    (*target)[offset + 2] = static_cast<uint8_t>(value >> 16);
    (*target)[offset + 3] = static_cast<uint8_t>(value >> 24);
  };
  const auto valid_crc_rejects_composition =
      [&](std::vector<uint8_t> invalid) {
        Frame malformed = source;
        malformed.payload = std::move(invalid);
        std::vector<uint8_t> framed;
        Frame decoded_frame;
        std::string local_error;
        Composition rejected;
        return EncodeFrame(malformed, &framed, &local_error) &&
               DecodeFrame(framed, &decoded_frame, &local_error) &&
               !DecodeComposition(decoded_frame.payload, &rejected,
                                  &local_error);
      };

  constexpr size_t kCompositionSuffixBytes = 9 * sizeof(uint32_t);
  const size_t suffix =
      valid_composition_payload.size() - kCompositionSuffixBytes;
  auto semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 3 * 4, 13 | (1u << 16));
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 3 * 4, 5); // handled bit mismatch
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix, 1); // one candidate, index one
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 4 * 4, 2);
  write_u32(&semantic_attack, suffix + 5 * 4, 1);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 7 * 4, 1u << 20);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 8 * 4, 2);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = valid_composition_payload;
  write_u32(&semantic_attack, suffix + 2 * 4, 0); // candidates need page_size
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));

  Composition empty_page = composition;
  empty_page.candidates.clear();
  empty_page.highlighted_index = 0;
  empty_page.page_index = 0;
  empty_page.page_size = 0;
  empty_page.state_flags &= ~4u;
  std::vector<uint8_t> empty_page_payload;
  CHECK(EncodeComposition(empty_page, &empty_page_payload, &error));
  const size_t empty_suffix =
      empty_page_payload.size() - kCompositionSuffixBytes;
  semantic_attack = empty_page_payload;
  write_u32(&semantic_attack, empty_suffix + 1 * 4, 1);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  semantic_attack = empty_page_payload;
  write_u32(&semantic_attack, empty_suffix + 2 * 4, 1);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));

  auto hostile_composition = payload;
  size_t candidate_count_offset = 4;
  for (int field = 0; field < 5; ++field) {
    const uint32_t size =
        hostile_composition[candidate_count_offset] |
        (hostile_composition[candidate_count_offset + 1] << 8) |
        (hostile_composition[candidate_count_offset + 2] << 16) |
        (hostile_composition[candidate_count_offset + 3] << 24);
    candidate_count_offset += 4 + size;
  }
  hostile_composition[candidate_count_offset] =
      static_cast<uint8_t>(kMaxCandidateCount + 1);
  hostile_composition[candidate_count_offset + 1] = 0;
  hostile_composition[candidate_count_offset + 2] = 0;
  hostile_composition[candidate_count_offset + 3] = 0;
  CHECK(!DecodeComposition(hostile_composition, &decoded, &error));
  semantic_attack = valid_composition_payload;
  size_t candidate_field = candidate_count_offset + 4;
  for (int field = 0; field < 3; ++field) {
    const uint32_t size =
        semantic_attack[candidate_field] |
        (semantic_attack[candidate_field + 1] << 8) |
        (semantic_attack[candidate_field + 2] << 16) |
        (semantic_attack[candidate_field + 3] << 24);
    candidate_field += 4 + size;
  }
  candidate_field += 4; // quality
  write_u32(&semantic_attack, candidate_field, 2);
  CHECK(valid_crc_rejects_composition(std::move(semantic_attack)));
  composition.candidates.resize(kMaxCandidateCount + 1);
  CHECK(!EncodeComposition(composition, &payload, &error));

  KeyEvent key{static_cast<uint32_t>('N'), 49, 1, 1, 1234};
  CHECK(EncodeKeyEvent(key, &payload));
  KeyEvent decoded_key;
  CHECK(DecodeKeyEvent(payload, &decoded_key, &error));
  CHECK(decoded_key == key);

  CHECK(EncodeAbsoluteCandidateSelection(17, 99, &payload));
  uint32_t absolute_index = 0;
  uint64_t composition_sequence = 0;
  CHECK(DecodeAbsoluteCandidateSelection(payload, &absolute_index,
                                         &composition_sequence, &error));
  CHECK(absolute_index == 17 && composition_sequence == 99);
  CHECK(!EncodeAbsoluteCandidateSelection(17, 0, &payload));

  const DeliveryReference delivery{
      Command::ProcessKey, Correlation{11, 12, 13, 14, 15, 16}};
  CHECK(EncodeDeliveryReference(delivery, &payload));
  DeliveryReference decoded_delivery;
  CHECK(DecodeDeliveryReference(payload, &decoded_delivery, &error));
  CHECK(decoded_delivery == delivery);

  ControlResult control{42, ControlState::Running, ControlError::None, false,
                        RuntimeReadiness::Maintenance, 7};
  CHECK(EncodeControlResult(control, &payload, &error));
  ControlResult decoded_control;
  CHECK(DecodeControlResult(payload, &decoded_control, &error));
  CHECK(decoded_control == control);
  control.state = ControlState::Failed;
  control.error = ControlError::UserDictionaryRollback;
  control.retryable = false;
  CHECK(EncodeControlResult(control, &payload, &error));
  CHECK(DecodeControlResult(payload, &decoded_control, &error));
  CHECK(decoded_control == control);
  payload.push_back(0);
  CHECK(!DecodeControlResult(payload, &decoded_control, &error));

  CHECK(EncodeControlOperationId(42, &payload));
  uint64_t operation_id = 0;
  CHECK(DecodeControlOperationId(payload, &operation_id, &error));
  CHECK(operation_id == 42);
  CHECK(!EncodeControlOperationId(0, &payload));

  CHECK(IsControlOperation(Command::ControlDeploy));
  CHECK(IsControlOperation(Command::ControlResetUserDictionary));
  CHECK(!IsControlOperation(Command::ControlStatus));
  source.command = Command::ControlResetUserDictionary;
  source.payload.clear();
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(DecodeFrame(bytes, &parsed, &error));
  CHECK(parsed.command == Command::ControlResetUserDictionary);
  source.command = Command::SelectCandidateAbsolute;
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(DecodeFrame(bytes, &parsed, &error));
  CHECK(parsed.command == Command::SelectCandidateAbsolute);

  static_assert(noexcept(EncodeFrame(source, &bytes, &error)));
  static_assert(std::is_nothrow_invocable_r_v<
                bool, decltype(&DecodeFrame), std::span<const uint8_t>, Frame *,
                std::string *>);
  static_assert(noexcept(EncodeComposition(composition, &payload, &error)));
  static_assert(std::is_nothrow_invocable_r_v<
                bool, decltype(&DecodeComposition), std::span<const uint8_t>,
                Composition *, std::string *>);
  static_assert(noexcept(EncodeControlResult(control, &payload, &error)));
  static_assert(std::is_nothrow_invocable_r_v<
                bool, decltype(&DecodeControlResult),
                std::span<const uint8_t>, ControlResult *, std::string *>);
  const std::vector<uint8_t> encode_sentinel{0xaa, 0x55};
  std::vector<uint8_t> unchanged_bytes = encode_sentinel;
  Composition unchanged_composition;
  unchanged_composition.schema_id = "sentinel";
  Frame unchanged_frame;
  unchanged_frame.command = Command::ClearComposition;
  unchanged_frame.payload = {0x42};
  CHECK(_putenv_s("FAMO_TEST_PROTOCOL_ALLOCATION_FAILURE", "1") == 0);
  CHECK(!EncodeFrame(source, &unchanged_bytes, &error));
  CHECK(unchanged_bytes == encode_sentinel);
  CHECK(!DecodeFrame(bytes, &unchanged_frame, &error));
  CHECK(unchanged_frame.command == Command::ClearComposition &&
        unchanged_frame.payload == std::vector<uint8_t>{0x42});
  CHECK(!DecodeComposition(valid_composition_payload, &unchanged_composition,
                           &error));
  CHECK(unchanged_composition.schema_id == "sentinel");
  CHECK(_putenv_s("FAMO_TEST_PROTOCOL_ALLOCATION_FAILURE", "") == 0);

  std::printf("protocol_selfcheck: OK\n");
  return 0;
}
