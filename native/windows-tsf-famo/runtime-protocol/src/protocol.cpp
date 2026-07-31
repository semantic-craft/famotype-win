#include "famo_runtime_protocol.h"
#include "wire_io.h"

#include <algorithm>
#include <limits>

#include "../../engine-api/famo_engine_api.h"
#include "protocol_boundary.h"

namespace famo::runtime {
namespace {

bool KnownCommand(uint16_t value) {
  return value >= static_cast<uint16_t>(Command::Hello) &&
         value <= static_cast<uint16_t>(Command::AbandonSession);
}

bool KnownStatus(uint32_t value) {
  return value <= static_cast<uint32_t>(Status::DeliveryFailed);
}

bool Utf8Boundary(std::string_view text, uint32_t offset) {
  if (offset > text.size())
    return false;
  return offset == text.size() ||
         (static_cast<unsigned char>(text[offset]) & 0xc0u) != 0x80u;
}

bool ValidCompositionSemantics(const Composition &value,
                               std::string *error) {
  constexpr uint32_t kContentFlags =
      FAMO_COMPOSITION_HAS_PREEDIT | FAMO_COMPOSITION_HAS_COMMIT |
      FAMO_COMPOSITION_HAS_CANDIDATES | FAMO_COMPOSITION_HANDLED;
  constexpr uint32_t kHostFlags =
      kHostInlinePreedit | kHostCandidatePreview | kHostAutoPair |
      kHostCjkEnglishSpacing | kHostCjkNumberSpacing | kHostPreviewPages |
      kHostPreviewRowsTwo | kHostRimeVertical;
  constexpr uint32_t kStatusFlags =
      FAMO_STATUS_ASCII_MODE | FAMO_STATUS_COMPOSING | FAMO_STATUS_DISABLED |
      FAMO_STATUS_FULL_SHAPE | FAMO_STATUS_ASCII_PUNCT |
      FAMO_STATUS_SIMPLIFIED;
  uint32_t expected_content = 0;
  if (!value.preedit.empty())
    expected_content |= FAMO_COMPOSITION_HAS_PREEDIT;
  if (!value.commit.empty())
    expected_content |= FAMO_COMPOSITION_HAS_COMMIT;
  if (!value.candidates.empty())
    expected_content |= FAMO_COMPOSITION_HAS_CANDIDATES;
  if (value.handled)
    expected_content |= FAMO_COMPOSITION_HANDLED;
  bool valid =
      (value.state_flags & ~(kContentFlags | kHostFlags)) == 0 &&
      (value.state_flags & kContentFlags) == expected_content &&
      (value.status_flags & ~kStatusFlags) == 0 &&
      value.candidates.size() <= kMaxCandidateCount &&
      ((value.candidates.empty() && value.highlighted_index == 0) ||
       (!value.candidates.empty() &&
        value.highlighted_index < value.candidates.size())) &&
      ((value.candidates.empty() && value.page_index == 0 &&
        value.page_size == 0) ||
       (!value.candidates.empty() && value.page_size > 0)) &&
      (value.commit.empty() || value.handled) && value.is_last_page <= 1 &&
      value.preedit_sel_start <= value.preedit_sel_end &&
      Utf8Boundary(value.preedit, value.preedit_sel_start) &&
      Utf8Boundary(value.preedit, value.preedit_sel_end) &&
      Utf8Boundary(value.preedit, value.preedit_cursor_pos);
  if (valid) {
    for (const Candidate &candidate : value.candidates) {
      if ((candidate.flags & ~FAMO_CANDIDATE_FLAG_DEFAULT) != 0) {
        valid = false;
        break;
      }
    }
  }
  if (!valid && error)
    *error = "inconsistent composition semantics";
  return valid;
}

} // namespace

bool SelectionCapabilityMatches(
    const SelectionCapability &left,
    const SelectionCapability &right) noexcept {
  const uint64_t difference =
      (left.low ^ right.low) | (left.high ^ right.high);
  return difference == 0 && static_cast<bool>(left) &&
         static_cast<bool>(right);
}

uint32_t Crc32(std::span<const uint8_t> bytes) {
  uint32_t crc = 0xffffffffu;
  for (uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool PeekFrameSize(std::span<const uint8_t> header, uint32_t *size,
                   std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!size || header.size() < kHeaderSize) {
    if (error)
      *error = "truncated header";
    return false;
  }
  Reader reader(header);
  uint32_t magic = 0;
  uint32_t decoded_size = 0;
  uint16_t version = 0, header_size = 0;
  if (!reader.U32(&magic) || !reader.U16(&version) ||
      !reader.U16(&header_size) || !reader.U32(&decoded_size) ||
      magic != kProtocolMagic || version < kMinSupportedProtocolVersion ||
      version > kProtocolVersion ||
      header_size != kHeaderSize || decoded_size < kHeaderSize ||
      decoded_size > kMaxFrameSize) {
    if (error)
      *error = "invalid frame prefix";
    return false;
  }
  *size = decoded_size;
  return true;
  });
}

bool EncodeFrame(const Frame &frame, std::vector<uint8_t> *bytes,
                 std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!bytes || !KnownCommand(static_cast<uint16_t>(frame.command)) ||
      !KnownStatus(static_cast<uint32_t>(frame.status)) ||
      (frame.flags & ~(kFlagResponse | kFlagAcknowledgePrevious)) != 0 ||
      frame.wire_version < kMinSupportedProtocolVersion ||
      frame.wire_version > kProtocolVersion ||
      frame.payload.size() > kMaxFramePayloadSize) {
    if (error)
      *error = "invalid frame fields";
    return false;
  }
  Writer writer;
  writer.U32(kProtocolMagic);
  writer.U16(frame.wire_version);
  writer.U16(kHeaderSize);
  writer.U32(static_cast<uint32_t>(kHeaderSize + frame.payload.size()));
  writer.U16(static_cast<uint16_t>(frame.command));
  writer.U16(frame.flags);
  writer.U32(static_cast<uint32_t>(frame.status));
  writer.U64(frame.correlation.client_id);
  writer.U64(frame.correlation.activation_generation);
  writer.U64(frame.correlation.connection_generation);
  writer.U64(frame.correlation.session_id);
  writer.U64(frame.correlation.session_generation);
  writer.U64(frame.correlation.sequence);
  writer.U32(Crc32(frame.payload));
  writer.U32(0);
  writer.Bytes(frame.payload);
  *bytes = writer.Take();
  return true;
  });
}

bool DecodeFrame(std::span<const uint8_t> bytes, Frame *frame,
                 std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!frame || bytes.size() < kHeaderSize) {
    if (error)
      *error = "truncated frame";
    return false;
  }
  uint32_t total_size = 0;
  if (!PeekFrameSize(bytes.first(kHeaderSize), &total_size, error) ||
      total_size != bytes.size()) {
    if (error && error->empty())
      *error = "frame size mismatch";
    return false;
  }
  Reader reader(bytes.first(kHeaderSize));
  uint32_t magic = 0, ignored_size = 0, status = 0, crc = 0, reserved = 0;
  uint16_t version = 0, header_size = 0, command = 0, flags = 0;
  Correlation correlation;
  if (!reader.U32(&magic) || !reader.U16(&version) ||
      !reader.U16(&header_size) || !reader.U32(&ignored_size) ||
      !reader.U16(&command) || !reader.U16(&flags) || !reader.U32(&status) ||
      !reader.U64(&correlation.client_id) ||
      !reader.U64(&correlation.activation_generation) ||
      !reader.U64(&correlation.connection_generation) ||
      !reader.U64(&correlation.session_id) ||
      !reader.U64(&correlation.session_generation) ||
      !reader.U64(&correlation.sequence) || !reader.U32(&crc) ||
      !reader.U32(&reserved) || !reader.done()) {
    if (error)
      *error = "truncated fixed header";
    return false;
  }
  if (!KnownCommand(command) || !KnownStatus(status) ||
      (flags & ~(kFlagResponse | kFlagAcknowledgePrevious)) != 0 ||
      reserved != 0) {
    if (error)
      *error = "unknown command, status, flags, or reserved field";
    return false;
  }
  const auto payload = bytes.subspan(kHeaderSize);
  if (Crc32(payload) != crc) {
    if (error)
      *error = "payload CRC mismatch";
    return false;
  }
  Frame decoded;
  decoded.command = static_cast<Command>(command);
  decoded.flags = flags;
  decoded.status = static_cast<Status>(status);
  decoded.correlation = correlation;
  decoded.payload.assign(payload.begin(), payload.end());
  decoded.wire_version = version;
  *frame = std::move(decoded);
  return true;
  });
}

bool EncodeHelloRequest(const HelloRequest &hello,
                        std::vector<uint8_t> *payload,
                        std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
    if (!payload || hello.min_protocol_version == 0 ||
        hello.min_protocol_version > hello.max_protocol_version ||
        hello.bridge_abi == 0) {
      if (error)
        *error = "invalid Hello request";
      return false;
    }
    Writer writer;
    writer.U16(hello.min_protocol_version);
    writer.U16(hello.max_protocol_version);
    writer.U32(hello.bridge_abi);
    *payload = writer.Take();
    return true;
  });
}

bool DecodeHelloRequest(std::span<const uint8_t> payload, HelloRequest *hello,
                        std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
    Reader reader(payload);
    HelloRequest decoded;
    if (!hello || !reader.U16(&decoded.min_protocol_version) ||
        !reader.U16(&decoded.max_protocol_version) ||
        !reader.U32(&decoded.bridge_abi) ||
        decoded.min_protocol_version == 0 ||
        decoded.min_protocol_version > decoded.max_protocol_version ||
        decoded.bridge_abi == 0) {
      if (error)
        *error = "invalid Hello request";
      return false;
    }
    *hello = decoded;
    return true;
  });
}

bool EncodeHelloResponse(const HelloResponse &hello,
                         std::vector<uint8_t> *payload,
                         std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
    if (!payload || hello.min_protocol_version == 0 ||
        hello.min_protocol_version > hello.max_protocol_version ||
        hello.selected_protocol_version < hello.min_protocol_version ||
        hello.selected_protocol_version > hello.max_protocol_version) {
      if (error)
        *error = "invalid Hello response";
      return false;
    }
    Writer writer;
    writer.U16(hello.min_protocol_version);
    writer.U16(hello.max_protocol_version);
    writer.U16(hello.selected_protocol_version);
    writer.U16(0);
    *payload = writer.Take();
    return true;
  });
}

bool DecodeHelloResponse(std::span<const uint8_t> payload,
                         HelloResponse *hello,
                         std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
    Reader reader(payload);
    HelloResponse decoded;
    uint16_t reserved = 0;
    if (!hello || !reader.U16(&decoded.min_protocol_version) ||
        !reader.U16(&decoded.max_protocol_version) ||
        !reader.U16(&decoded.selected_protocol_version) ||
        !reader.U16(&reserved) || reserved != 0 ||
        decoded.min_protocol_version == 0 ||
        decoded.min_protocol_version > decoded.max_protocol_version ||
        decoded.selected_protocol_version < decoded.min_protocol_version ||
        decoded.selected_protocol_version > decoded.max_protocol_version) {
      if (error)
        *error = "invalid Hello response";
      return false;
    }
    *hello = decoded;
    return true;
  });
}

bool NegotiateHello(const Frame &request, NegotiatedHello *negotiated,
                    std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
    if (!negotiated || request.command != Command::Hello ||
        request.flags != 0 || request.status != Status::Ok) {
      if (error)
        *error = "invalid Hello frame";
      return false;
    }

    NegotiatedHello result;
    if (request.payload.empty()) {
      result.protocol_version = request.wire_version;
      result.legacy = true;
      *negotiated = std::move(result);
      return true;
    }

    HelloRequest hello;
    if (!DecodeHelloRequest(request.payload, &hello, error) ||
        request.wire_version < hello.min_protocol_version ||
        request.wire_version > hello.max_protocol_version) {
      if (error && error->empty())
        *error = "Hello frame version is outside the offered range";
      return false;
    }
    const uint16_t minimum =
        (std::max)(hello.min_protocol_version,
                   kMinSupportedProtocolVersion);
    const uint16_t maximum =
        (std::min)(hello.max_protocol_version, kProtocolVersion);
    if (minimum > maximum) {
      if (error)
        *error = "Hello protocol ranges do not overlap";
      return false;
    }

    result.protocol_version = maximum;
    result.bridge_abi = hello.bridge_abi;
    const HelloResponse response{kMinSupportedProtocolVersion,
                                 kProtocolVersion, maximum};
    if (!EncodeHelloResponse(response, &result.response_payload, error))
      return false;
    *negotiated = std::move(result);
    return true;
  });
}

bool EncodeOpenSession(std::string_view schema, std::vector<uint8_t> *payload,
                       std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!payload)
    return false;
  Writer writer;
  if (!writer.String(schema, error))
    return false;
  *payload = writer.Take();
  return true;
  });
}

bool DecodeOpenSession(std::span<const uint8_t> payload, std::string *schema,
                       std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!schema)
    return false;
  std::string decoded;
  Reader reader(payload);
  if (!reader.String(decoded, error) || !Finish(reader, error))
    return false;
  *schema = std::move(decoded);
  return true;
  });
}

bool EncodeKeyEvent(const KeyEvent &key,
                    std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  if (!payload)
    return false;
  Writer writer;
  writer.U32(key.virtual_key);
  writer.U32(key.scan_code);
  writer.U32(key.modifiers);
  writer.U32(key.is_key_down);
  writer.U64(key.timestamp_ms);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeKeyEvent(std::span<const uint8_t> payload, KeyEvent *key,
                    std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!key)
    return false;
  KeyEvent decoded;
  Reader reader(payload);
  if (!reader.U32(&decoded.virtual_key) || !reader.U32(&decoded.scan_code) ||
      !reader.U32(&decoded.modifiers) || !reader.U32(&decoded.is_key_down) ||
      !reader.U64(&decoded.timestamp_ms) || decoded.is_key_down > 1) {
    if (error)
      *error = "invalid key payload";
    return false;
  }
  if (!Finish(reader, error))
    return false;
  *key = decoded;
  return true;
  });
}

bool EncodeCandidateIndex(uint32_t index,
                          std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  if (!payload)
    return false;
  Writer writer;
  writer.U32(index);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeCandidateIndex(std::span<const uint8_t> payload, uint32_t *index,
                          std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!index)
    return false;
  Reader reader(payload);
  uint32_t decoded = 0;
  if (!reader.U32(&decoded)) {
    if (error)
      *error = "invalid candidate index payload";
    return false;
  }
  if (!Finish(reader, error))
    return false;
  *index = decoded;
  return true;
  });
}

bool EncodeAbsoluteCandidateSelection(uint32_t index,
                                      uint64_t composition_sequence,
                                      std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  if (!payload || composition_sequence == 0)
    return false;
  Writer writer;
  writer.U32(index);
  writer.U64(composition_sequence);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeAbsoluteCandidateSelection(std::span<const uint8_t> payload,
                                      uint32_t *index,
                                      uint64_t *composition_sequence,
                                      std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!index || !composition_sequence)
    return false;
  Reader reader(payload);
  uint32_t decoded_index = 0;
  uint64_t decoded_sequence = 0;
  if (!reader.U32(&decoded_index) || !reader.U64(&decoded_sequence) ||
      decoded_sequence == 0) {
    if (error)
      *error = "invalid absolute candidate selection payload";
    return false;
  }
  if (!Finish(reader, error))
    return false;
  *index = decoded_index;
  *composition_sequence = decoded_sequence;
  return true;
  });
}

bool IsDeliveryTracked(Command command) {
  switch (command) {
  case Command::ProcessKey:
  case Command::SelectCandidate:
  case Command::CommitComposition:
  case Command::ClearComposition:
  case Command::HighlightCandidate:
  case Command::ChangePage:
  case Command::SelectCandidateAbsolute:
    return true;
  default:
    return false;
  }
}

bool EncodeDeliveryReference(const DeliveryReference &reference,
                             std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  const Correlation &c = reference.correlation;
  if (!payload || !IsDeliveryTracked(reference.command) || c.client_id == 0 ||
      c.activation_generation == 0 || c.connection_generation == 0 ||
      c.session_id == 0 || c.session_generation == 0 || c.sequence == 0) {
    return false;
  }
  Writer writer;
  writer.U16(static_cast<uint16_t>(reference.command));
  writer.U16(0);
  writer.U64(c.client_id);
  writer.U64(c.activation_generation);
  writer.U64(c.connection_generation);
  writer.U64(c.session_id);
  writer.U64(c.session_generation);
  writer.U64(c.sequence);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeDeliveryReference(std::span<const uint8_t> payload,
                             DeliveryReference *reference,
                             std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!reference) {
    if (error)
      *error = "delivery reference target is required";
    return false;
  }
  Reader reader(payload);
  uint16_t command = 0;
  uint16_t reserved = 0;
  DeliveryReference decoded;
  Correlation &c = decoded.correlation;
  if (!reader.U16(&command) || !reader.U16(&reserved) ||
      !reader.U64(&c.client_id) ||
      !reader.U64(&c.activation_generation) ||
      !reader.U64(&c.connection_generation) || !reader.U64(&c.session_id) ||
      !reader.U64(&c.session_generation) || !reader.U64(&c.sequence) ||
      !reader.done() || reserved != 0) {
    if (error)
      *error = "invalid delivery reference payload";
    return false;
  }
  decoded.command = static_cast<Command>(command);
  if (!IsDeliveryTracked(decoded.command) || c.client_id == 0 ||
      c.activation_generation == 0 || c.connection_generation == 0 ||
      c.session_id == 0 || c.session_generation == 0 || c.sequence == 0) {
    if (error)
      *error = "invalid delivery reference fields";
    return false;
  }
  *reference = decoded;
  return true;
  });
}

bool EncodePageDirection(bool backward,
                         std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  if (!payload)
    return false;
  Writer writer;
  writer.U32(backward ? 1u : 0u);
  *payload = writer.Take();
  return true;
  });
}

bool DecodePageDirection(std::span<const uint8_t> payload, bool *backward,
                         std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!backward)
    return false;
  Reader reader(payload);
  uint32_t encoded = 0;
  if (!reader.U32(&encoded) || encoded > 1) {
    if (error)
      *error = "invalid page direction payload";
    return false;
  }
  if (!Finish(reader, error))
    return false;
  *backward = encoded != 0;
  return true;
  });
}

bool EncodeUiState(const UiState &state, std::vector<uint8_t> *payload,
                   std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  const auto valid_rect = [](const UiRect &rect) {
    return rect.right >= rect.left && rect.bottom >= rect.top;
  };
  if (!payload || !valid_rect(state.caret) || !valid_rect(state.work_area) ||
      state.dpi < 48 || state.dpi > 768 ||
      !state.selection_capability) {
    if (error)
      *error = "invalid UI state";
    return false;
  }
  Writer writer;
  const auto write_rect = [&writer](const UiRect &rect) {
    writer.U32(static_cast<uint32_t>(rect.left));
    writer.U32(static_cast<uint32_t>(rect.top));
    writer.U32(static_cast<uint32_t>(rect.right));
    writer.U32(static_cast<uint32_t>(rect.bottom));
  };
  write_rect(state.caret);
  write_rect(state.work_area);
  writer.U32(state.dpi);
  writer.U32((state.layout_available ? 1u : 0u) |
             (state.focused ? 2u : 0u) | (state.show_allowed ? 4u : 0u));
  writer.U64(state.selection_capability.low);
  writer.U64(state.selection_capability.high);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeUiState(std::span<const uint8_t> payload, UiState *state,
                   std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!state)
    return false;
  Reader reader(payload);
  uint32_t values[10]{};
  for (uint32_t &value : values) {
    if (!reader.U32(&value)) {
      if (error)
        *error = "truncated UI state";
      return false;
    }
  }
  const uint32_t flags = values[9];
  uint64_t capability_low = 0;
  uint64_t capability_high = 0;
  if (!reader.U64(&capability_low) || !reader.U64(&capability_high)) {
    if (error)
      *error = "truncated UI selection capability";
    return false;
  }
  UiState decoded{{static_cast<int32_t>(values[0]),
                   static_cast<int32_t>(values[1]),
                   static_cast<int32_t>(values[2]),
                   static_cast<int32_t>(values[3])},
                  {static_cast<int32_t>(values[4]),
                   static_cast<int32_t>(values[5]),
                   static_cast<int32_t>(values[6]),
                   static_cast<int32_t>(values[7])},
                  values[8],
                  (flags & 1u) != 0,
                  (flags & 2u) != 0,
                  (flags & 4u) != 0,
                  {capability_low, capability_high}};
  const auto valid_rect = [](const UiRect &rect) {
    return rect.right >= rect.left && rect.bottom >= rect.top;
  };
  if (!reader.done() || (flags & ~7u) != 0 || decoded.dpi < 48 ||
      decoded.dpi > 768 || !valid_rect(decoded.caret) ||
      !valid_rect(decoded.work_area) ||
      !decoded.selection_capability) {
    if (error)
      *error = "invalid UI state";
    return false;
  }
  *state = decoded;
  return true;
  });
}

bool EncodeComposition(const Composition &value, std::vector<uint8_t> *payload,
                       std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!payload || !ValidCompositionSemantics(value, error)) {
    if (error)
      if (error->empty())
        *error = "invalid composition";
    return false;
  }
  constexpr size_t kFixedBytes = 4 + 4 + (9 * 4);
  size_t encoded_size = kFixedBytes + (value.candidates.size() * 8);
  const auto account_string = [&](std::string_view text) {
    if (text.size() > kMaxStringBytes || !IsValidUtf8(text) ||
        encoded_size > kMaxFramePayloadSize ||
        text.size() + 4 > kMaxFramePayloadSize - encoded_size) {
      return false;
    }
    encoded_size += 4 + text.size();
    return true;
  };
  if (!account_string(value.preedit) || !account_string(value.commit) ||
      !account_string(value.commit_preview) ||
      !account_string(value.schema_id) || !account_string(value.schema_name)) {
    if (error)
      *error = "composition strings exceed wire budget";
    return false;
  }
  for (const auto &candidate : value.candidates) {
    if (!account_string(candidate.text) || !account_string(candidate.comment) ||
        !account_string(candidate.label)) {
      if (error)
        *error = "candidate strings exceed wire budget";
      return false;
    }
  }
  Writer writer;
  writer.U8(value.handled ? 1 : 0);
  writer.U8(0);
  writer.U16(0);
  if (!writer.String(value.preedit, error) ||
      !writer.String(value.commit, error) ||
      !writer.String(value.commit_preview, error) ||
      !writer.String(value.schema_id, error) ||
      !writer.String(value.schema_name, error))
    return false;
  writer.U32(static_cast<uint32_t>(value.candidates.size()));
  for (const auto &candidate : value.candidates) {
    if (!writer.String(candidate.text, error) ||
        !writer.String(candidate.comment, error) ||
        !writer.String(candidate.label, error))
      return false;
    writer.U32(candidate.quality);
    writer.U32(candidate.flags);
  }
  writer.U32(value.highlighted_index);
  writer.U32(value.page_index);
  writer.U32(value.page_size);
  writer.U32(value.state_flags);
  writer.U32(value.preedit_sel_start);
  writer.U32(value.preedit_sel_end);
  writer.U32(value.preedit_cursor_pos);
  writer.U32(value.status_flags);
  writer.U32(value.is_last_page);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeComposition(std::span<const uint8_t> payload, Composition *value,
                       std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!value)
    return false;
  Composition decoded;
  Reader reader(payload);
  uint8_t handled = 0, reserved8 = 0;
  uint16_t reserved16 = 0;
  uint32_t count = 0;
  if (!reader.U8(&handled) || !reader.U8(&reserved8) ||
      !reader.U16(&reserved16) || handled > 1 || reserved8 != 0 ||
      reserved16 != 0 || !reader.String(decoded.preedit, error) ||
      !reader.String(decoded.commit, error) ||
      !reader.String(decoded.commit_preview, error) ||
      !reader.String(decoded.schema_id, error) ||
      !reader.String(decoded.schema_name, error) || !reader.U32(&count) ||
      count > kMaxCandidateCount) {
    if (error && error->empty())
      *error = "invalid composition prefix";
    return false;
  }
  decoded.handled = handled != 0;
  decoded.candidates.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Candidate candidate;
    if (!reader.String(candidate.text, error) ||
        !reader.String(candidate.comment, error) ||
        !reader.String(candidate.label, error) ||
        !reader.U32(&candidate.quality) || !reader.U32(&candidate.flags))
      return false;
    decoded.candidates.push_back(std::move(candidate));
  }
  if (!reader.U32(&decoded.highlighted_index) ||
      !reader.U32(&decoded.page_index) || !reader.U32(&decoded.page_size) ||
      !reader.U32(&decoded.state_flags) ||
      !reader.U32(&decoded.preedit_sel_start) ||
      !reader.U32(&decoded.preedit_sel_end) ||
      !reader.U32(&decoded.preedit_cursor_pos) ||
      !reader.U32(&decoded.status_flags) ||
      !reader.U32(&decoded.is_last_page)) {
    if (error)
      *error = "truncated composition suffix";
    return false;
  }
  if (!Finish(reader, error) || !ValidCompositionSemantics(decoded, error))
    return false;
  *value = std::move(decoded);
  return true;
  });
}

} // namespace famo::runtime
