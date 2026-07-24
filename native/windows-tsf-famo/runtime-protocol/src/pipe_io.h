#pragma once

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

#include "famo_runtime_protocol.h"

namespace famo::runtime::pipe_io {

enum class Result { Ok, Timeout, Disconnected, Failed };
using Deadline = std::chrono::steady_clock::time_point;

class RetirementGate;
using RetirementGatePtr = std::shared_ptr<RetirementGate>;

RetirementGatePtr MakeRetirementGate();
bool RetirementReady(const RetirementGatePtr &gate);

Result WriteBytes(HANDLE pipe, std::span<const uint8_t> bytes,
                  Deadline deadline, std::string *error,
                  const RetirementGatePtr &gate = {});
Result ReadBytes(HANDLE pipe, std::span<uint8_t> bytes, Deadline deadline,
                 std::string *error, const RetirementGatePtr &gate = {});
Result WriteFrame(HANDLE pipe, const Frame &frame, Deadline deadline,
                  std::string *error, const RetirementGatePtr &gate = {});
Result ReadFrame(HANDLE pipe, Frame *frame, Deadline deadline,
                 std::string *error, const RetirementGatePtr &gate = {});

} // namespace famo::runtime::pipe_io
