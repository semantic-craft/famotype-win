#include <cstdio>

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
  bad[14] = 2;
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

  source.payload.assign(kMaxFrameSize - kHeaderSize + 1, 0);
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
                         false};
  CHECK(EncodeUiState(ui_state, &payload, &error));
  UiState decoded_ui_state;
  CHECK(DecodeUiState(payload, &decoded_ui_state, &error));
  CHECK(decoded_ui_state == ui_state);
  payload.back() = 0x80;
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
  composition.state_flags = 5;
  composition.preedit_sel_end = 2;
  composition.preedit_cursor_pos = 2;
  composition.status_flags = 2;
  composition.is_last_page = 1;
  CHECK(EncodeComposition(composition, &payload, &error));
  Composition decoded;
  CHECK(DecodeComposition(payload, &decoded, &error));
  CHECK(decoded == composition);
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
  composition.candidates.resize(kMaxCandidateCount + 1);
  CHECK(!EncodeComposition(composition, &payload, &error));

  KeyEvent key{static_cast<uint32_t>('N'), 49, 1, 1, 1234};
  CHECK(EncodeKeyEvent(key, &payload));
  KeyEvent decoded_key;
  CHECK(DecodeKeyEvent(payload, &decoded_key, &error));
  CHECK(decoded_key == key);

  ControlResult control{42, ControlState::Running, ControlError::None, false,
                        RuntimeReadiness::Maintenance, 7};
  CHECK(EncodeControlResult(control, &payload, &error));
  ControlResult decoded_control;
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
  source.command = Command::ControlShutdown;
  source.payload.clear();
  CHECK(EncodeFrame(source, &bytes, &error));
  CHECK(DecodeFrame(bytes, &parsed, &error));
  CHECK(parsed.command == Command::ControlShutdown);

  std::printf("protocol_selfcheck: OK\n");
  return 0;
}
