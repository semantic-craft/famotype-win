#include "famo_runtime_protocol.h"
#include "wire_io.h"

#include <algorithm>
#include <limits>

namespace famo::runtime {
namespace {

bool KnownCommand(uint16_t value) {
  return value >= static_cast<uint16_t>(Command::Hello) &&
         value <= static_cast<uint16_t>(Command::SelectCandidateAbsolute);
}

bool KnownStatus(uint32_t value) {
  return value <= static_cast<uint32_t>(Status::WrongPeer);
}

} // namespace

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
                   std::string *error) {
  if (!size || header.size() < kHeaderSize) {
    if (error)
      *error = "truncated header";
    return false;
  }
  Reader reader(header);
  uint32_t magic = 0;
  uint16_t version = 0, header_size = 0;
  if (!reader.U32(&magic) || !reader.U16(&version) ||
      !reader.U16(&header_size) || !reader.U32(size) ||
      magic != kProtocolMagic || version != kProtocolVersion ||
      header_size != kHeaderSize || *size < kHeaderSize ||
      *size > kMaxFrameSize) {
    if (error)
      *error = "invalid frame prefix";
    return false;
  }
  return true;
}

bool EncodeFrame(const Frame &frame, std::vector<uint8_t> *bytes,
                 std::string *error) {
  if (!bytes || !KnownCommand(static_cast<uint16_t>(frame.command)) ||
      !KnownStatus(static_cast<uint32_t>(frame.status)) ||
      (frame.flags & ~kFlagResponse) != 0 ||
      frame.payload.size() > kMaxFrameSize - kHeaderSize) {
    if (error)
      *error = "invalid frame fields";
    return false;
  }
  Writer writer;
  writer.U32(kProtocolMagic);
  writer.U16(kProtocolVersion);
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
}

bool DecodeFrame(std::span<const uint8_t> bytes, Frame *frame,
                 std::string *error) {
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
      (flags & ~kFlagResponse) != 0 || reserved != 0) {
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
  frame->command = static_cast<Command>(command);
  frame->flags = flags;
  frame->status = static_cast<Status>(status);
  frame->correlation = correlation;
  frame->payload.assign(payload.begin(), payload.end());
  return true;
}

bool EncodeOpenSession(std::string_view schema, std::vector<uint8_t> *payload,
                       std::string *error) {
  if (!payload)
    return false;
  Writer writer;
  if (!writer.String(schema, error))
    return false;
  *payload = writer.Take();
  return true;
}

bool DecodeOpenSession(std::span<const uint8_t> payload, std::string *schema,
                       std::string *error) {
  if (!schema)
    return false;
  Reader reader(payload);
  return reader.String(*schema, error) && Finish(reader, error);
}

bool EncodeKeyEvent(const KeyEvent &key, std::vector<uint8_t> *payload) {
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
}

bool DecodeKeyEvent(std::span<const uint8_t> payload, KeyEvent *key,
                    std::string *error) {
  if (!key)
    return false;
  Reader reader(payload);
  if (!reader.U32(&key->virtual_key) || !reader.U32(&key->scan_code) ||
      !reader.U32(&key->modifiers) || !reader.U32(&key->is_key_down) ||
      !reader.U64(&key->timestamp_ms) || key->is_key_down > 1) {
    if (error)
      *error = "invalid key payload";
    return false;
  }
  return Finish(reader, error);
}

bool EncodeCandidateIndex(uint32_t index, std::vector<uint8_t> *payload) {
  if (!payload)
    return false;
  Writer writer;
  writer.U32(index);
  *payload = writer.Take();
  return true;
}

bool DecodeCandidateIndex(std::span<const uint8_t> payload, uint32_t *index,
                          std::string *error) {
  if (!index)
    return false;
  Reader reader(payload);
  if (!reader.U32(index)) {
    if (error)
      *error = "invalid candidate index payload";
    return false;
  }
  return Finish(reader, error);
}

bool EncodeAbsoluteCandidateSelection(uint32_t index,
                                      uint64_t composition_sequence,
                                      std::vector<uint8_t> *payload) {
  if (!payload || composition_sequence == 0)
    return false;
  Writer writer;
  writer.U32(index);
  writer.U64(composition_sequence);
  *payload = writer.Take();
  return true;
}

bool DecodeAbsoluteCandidateSelection(std::span<const uint8_t> payload,
                                      uint32_t *index,
                                      uint64_t *composition_sequence,
                                      std::string *error) {
  if (!index || !composition_sequence)
    return false;
  Reader reader(payload);
  if (!reader.U32(index) || !reader.U64(composition_sequence) ||
      *composition_sequence == 0) {
    if (error)
      *error = "invalid absolute candidate selection payload";
    return false;
  }
  return Finish(reader, error);
}

bool EncodePageDirection(bool backward, std::vector<uint8_t> *payload) {
  if (!payload)
    return false;
  Writer writer;
  writer.U32(backward ? 1u : 0u);
  *payload = writer.Take();
  return true;
}

bool DecodePageDirection(std::span<const uint8_t> payload, bool *backward,
                         std::string *error) {
  if (!backward)
    return false;
  Reader reader(payload);
  uint32_t encoded = 0;
  if (!reader.U32(&encoded) || encoded > 1) {
    if (error)
      *error = "invalid page direction payload";
    return false;
  }
  *backward = encoded != 0;
  return Finish(reader, error);
}

bool EncodeUiState(const UiState &state, std::vector<uint8_t> *payload,
                   std::string *error) {
  const auto valid_rect = [](const UiRect &rect) {
    return rect.right >= rect.left && rect.bottom >= rect.top;
  };
  if (!payload || !valid_rect(state.caret) || !valid_rect(state.work_area) ||
      state.dpi < 48 || state.dpi > 768) {
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
  *payload = writer.Take();
  return true;
}

bool DecodeUiState(std::span<const uint8_t> payload, UiState *state,
                   std::string *error) {
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
                  (flags & 4u) != 0};
  const auto valid_rect = [](const UiRect &rect) {
    return rect.right >= rect.left && rect.bottom >= rect.top;
  };
  if (!reader.done() || (flags & ~7u) != 0 || decoded.dpi < 48 ||
      decoded.dpi > 768 || !valid_rect(decoded.caret) ||
      !valid_rect(decoded.work_area)) {
    if (error)
      *error = "invalid UI state";
    return false;
  }
  *state = decoded;
  return true;
}

bool EncodeComposition(const Composition &value, std::vector<uint8_t> *payload,
                       std::string *error) {
  if (!payload || value.candidates.size() > kMaxCandidateCount) {
    if (error)
      *error = "too many candidates";
    return false;
  }
  constexpr size_t kFixedBytes = 4 + 4 + (9 * 4);
  constexpr size_t kMaxPayload = kMaxFrameSize - kHeaderSize;
  size_t encoded_size = kFixedBytes + (value.candidates.size() * 8);
  const auto account_string = [&](std::string_view text) {
    if (text.size() > kMaxStringBytes || !IsValidUtf8(text) ||
        encoded_size > kMaxPayload ||
        text.size() + 4 > kMaxPayload - encoded_size) {
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
}

bool DecodeComposition(std::span<const uint8_t> payload, Composition *value,
                       std::string *error) {
  if (!value)
    return false;
  Reader reader(payload);
  uint8_t handled = 0, reserved8 = 0;
  uint16_t reserved16 = 0;
  uint32_t count = 0;
  if (!reader.U8(&handled) || !reader.U8(&reserved8) ||
      !reader.U16(&reserved16) || handled > 1 || reserved8 != 0 ||
      reserved16 != 0 || !reader.String(value->preedit, error) ||
      !reader.String(value->commit, error) ||
      !reader.String(value->commit_preview, error) ||
      !reader.String(value->schema_id, error) ||
      !reader.String(value->schema_name, error) || !reader.U32(&count) ||
      count > kMaxCandidateCount) {
    if (error && error->empty())
      *error = "invalid composition prefix";
    return false;
  }
  value->handled = handled != 0;
  value->candidates.clear();
  value->candidates.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Candidate candidate;
    if (!reader.String(candidate.text, error) ||
        !reader.String(candidate.comment, error) ||
        !reader.String(candidate.label, error) ||
        !reader.U32(&candidate.quality) || !reader.U32(&candidate.flags))
      return false;
    value->candidates.push_back(std::move(candidate));
  }
  if (!reader.U32(&value->highlighted_index) ||
      !reader.U32(&value->page_index) || !reader.U32(&value->page_size) ||
      !reader.U32(&value->state_flags) ||
      !reader.U32(&value->preedit_sel_start) ||
      !reader.U32(&value->preedit_sel_end) ||
      !reader.U32(&value->preedit_cursor_pos) ||
      !reader.U32(&value->status_flags) || !reader.U32(&value->is_last_page)) {
    if (error)
      *error = "truncated composition suffix";
    return false;
  }
  return Finish(reader, error);
}

} // namespace famo::runtime
