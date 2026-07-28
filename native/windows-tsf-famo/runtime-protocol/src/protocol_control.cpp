#include "famo_runtime_protocol.h"
#include "wire_io.h"
#include "protocol_boundary.h"

namespace famo::runtime {
namespace {

bool KnownControlState(uint32_t value) {
  return value >= static_cast<uint32_t>(ControlState::Pending) &&
         value <= static_cast<uint32_t>(ControlState::Failed);
}

bool KnownControlError(uint32_t value) {
  return value <= static_cast<uint32_t>(ControlError::UserDictionaryRollback);
}

bool KnownReadiness(uint32_t value) {
  return value <= static_cast<uint32_t>(RuntimeReadiness::Stopping);
}

} // namespace

bool IsControlOperation(Command command) {
  return command == Command::ControlReloadStyle ||
         command == Command::ControlReloadOptions ||
         command == Command::ControlSelectSchema ||
         command == Command::ControlDeploy ||
         command == Command::ControlResetUserDictionary ||
         command == Command::ControlShutdown;
}

bool EncodeControlOperationId(uint64_t operation_id,
                              std::vector<uint8_t> *payload) noexcept {
  return ProtocolBoundary(nullptr, [&] {
  if (!payload || operation_id == 0)
    return false;
  Writer writer;
  writer.U64(operation_id);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeControlOperationId(std::span<const uint8_t> payload,
                              uint64_t *operation_id,
                              std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!operation_id)
    return false;
  Reader reader(payload);
  uint64_t decoded = 0;
  if (!reader.U64(&decoded) || decoded == 0) {
    if (error)
      *error = "invalid control operation id";
    return false;
  }
  if (!Finish(reader, error))
    return false;
  *operation_id = decoded;
  return true;
  });
}

bool EncodeControlResult(const ControlResult &result,
                         std::vector<uint8_t> *payload,
                         std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!payload || result.operation_id == 0 ||
      !KnownControlState(static_cast<uint32_t>(result.state)) ||
      !KnownControlError(static_cast<uint32_t>(result.error)) ||
      !KnownReadiness(static_cast<uint32_t>(result.readiness)) ||
      (result.state != ControlState::Failed &&
       (result.error != ControlError::None || result.retryable))) {
    if (error)
      *error = "invalid control result";
    return false;
  }
  Writer writer;
  writer.U64(result.operation_id);
  writer.U32(static_cast<uint32_t>(result.state));
  writer.U32(static_cast<uint32_t>(result.error));
  writer.U32(result.retryable ? 1u : 0u);
  writer.U32(static_cast<uint32_t>(result.readiness));
  writer.U64(result.engine_generation);
  *payload = writer.Take();
  return true;
  });
}

bool DecodeControlResult(std::span<const uint8_t> payload,
                         ControlResult *result,
                         std::string *error) noexcept {
  return ProtocolBoundary(error, [&] {
  if (!result)
    return false;
  Reader reader(payload);
  ControlResult decoded;
  uint32_t state = 0, control_error = 0, retryable = 0, readiness = 0;
  if (!reader.U64(&decoded.operation_id) || !reader.U32(&state) ||
      !reader.U32(&control_error) || !reader.U32(&retryable) ||
      !reader.U32(&readiness) || !reader.U64(&decoded.engine_generation) ||
      !reader.done() || decoded.operation_id == 0 || retryable > 1 ||
      !KnownControlState(state) || !KnownControlError(control_error) ||
      !KnownReadiness(readiness)) {
    if (error)
      *error = "invalid control result";
    return false;
  }
  decoded.state = static_cast<ControlState>(state);
  decoded.error = static_cast<ControlError>(control_error);
  decoded.retryable = retryable != 0;
  decoded.readiness = static_cast<RuntimeReadiness>(readiness);
  if (decoded.state != ControlState::Failed &&
      (decoded.error != ControlError::None || decoded.retryable)) {
    if (error)
      *error = "non-failure control result carries an error";
    return false;
  }
  *result = decoded;
  return true;
  });
}

} // namespace famo::runtime
