#include "famo_runtime_service.h"

#include <algorithm>
#include <chrono>
#include <new>

namespace famo::runtime {
namespace {

bool SameLogicalSession(const Correlation &left, const Correlation &right) {
  return left.client_id == right.client_id &&
         left.activation_generation == right.activation_generation &&
         left.connection_generation == right.connection_generation &&
         left.session_id == right.session_id &&
         left.session_generation == right.session_generation;
}

bool SameFrame(const Frame &left, const Frame &right) {
  return left.command == right.command && left.flags == right.flags &&
         left.status == right.status && left.correlation == right.correlation &&
         left.payload == right.payload;
}

bool TestFaultEnabled(const wchar_t *name) {
  wchar_t value[2]{};
  return GetEnvironmentVariableW(name, value,
                                 static_cast<DWORD>(std::size(value))) > 0;
}

bool ValidFinalReply(const Frame &reply,
                     const DeliveryReference &reference) {
  return reply.flags == kFlagResponse &&
         static_cast<uint32_t>(reply.status) <=
             static_cast<uint32_t>(Status::DeliveryFailed) &&
         reply.command == reference.command &&
         reply.correlation == reference.correlation &&
         reply.payload.size() <= kMaxFramePayloadSize;
}

} // namespace

Frame RuntimeService::DispatchForDelivery(const Frame &request,
                                          const PipeClientIdentity &owner) {
  if (!owner)
    return Reply(request, Status::InvalidFrame);

  const bool delivery_control =
      request.command == Command::ExecutePrepared ||
      request.command == Command::ClaimResult ||
      request.command == Command::AckResult;
  const bool terminal_connection_abandon =
      request.command == Command::AbandonConnection;
  const bool terminal_session_abandon =
      request.command == Command::AbandonSession;
  const bool reconnect_hello = request.command == Command::Hello;
  if (!terminal_connection_abandon && !terminal_session_abandon &&
      !reconnect_hello &&
      readiness_.load() != RuntimeReadiness::Ready) {
    return Reply(request, Status::Unavailable);
  }
  if (request.command == Command::UpdateUiState)
    return DispatchUiState(request, &owner);

  std::unique_lock<std::timed_mutex> lock(mutex_, std::try_to_lock);
  if (!lock && !lock.try_lock_for(std::chrono::milliseconds(1)))
    return Reply(request, Status::Unavailable);
  if (!lock)
    return Reply(request, Status::Unavailable);

  if (terminal_connection_abandon)
    return AbandonConnectionLocked(request, owner);
  if (terminal_session_abandon) {
    DeliveryReference reference;
    std::string error;
    if (request.flags != 0 || request.status != Status::Ok ||
        !DecodeDeliveryReference(request.payload, &reference, &error)) {
      return Reply(request, Status::InvalidFrame);
    }
    return AbandonSessionLocked(request, reference, owner);
  }
  if (readiness_.load() != RuntimeReadiness::Ready) {
    const Correlation &c = request.correlation;
    if (request.flags != 0 || request.status != Status::Ok ||
        !request.payload.empty() || c.client_id == 0 ||
        c.activation_generation == 0 || c.connection_generation == 0 ||
        c.session_id != 0 || c.session_generation != 0 || c.sequence != 0) {
      return Reply(request, Status::InvalidFrame);
    }
    const auto client = clients_.find(c.client_id);
    if (client != clients_.end() &&
        client->second.activation_generation == c.activation_generation &&
        client->second.connection_generation == c.connection_generation &&
        client->second.owner == owner) {
      return Reply(request, Status::Ok);
    }
    if (IsAbandonedEpochLocked(c) || client != clients_.end())
      return Reply(request, Status::StaleRequest);
    return Reply(request, Status::Unavailable);
  }
  if (!IsDeliveryTracked(request.command) && !delivery_control)
    return DispatchLocked(request, &owner);

  const Correlation &caller = request.correlation;
  const auto client = clients_.find(caller.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation !=
          caller.activation_generation ||
      client->second.connection_generation !=
          caller.connection_generation ||
      client->second.owner != owner) {
    return Reply(request, Status::StaleRequest);
  }
  if (IsDeliveryTracked(request.command))
    return PrepareDeliveryLocked(request, owner);

  DeliveryReference reference;
  std::string error;
  if (request.flags != 0 || request.status != Status::Ok ||
      !DecodeDeliveryReference(request.payload, &reference, &error)) {
    return Reply(request, Status::InvalidFrame);
  }
  if (request.command == Command::ExecutePrepared)
    return ExecutePreparedLocked(request, reference, owner);
  if (request.command == Command::ClaimResult)
    return ClaimDeliveryLocked(request, reference, owner);
  return AcknowledgeDeliveryLocked(request, reference, owner);
}

Frame RuntimeService::AbandonConnectionLocked(
    const Frame &request, const PipeClientIdentity &owner) {
  const Correlation &c = request.correlation;
  if (!started_)
    return Reply(request, Status::Unavailable);
  if (!owner || request.flags != 0 || request.status != Status::Ok ||
      !request.payload.empty() || c.client_id == 0 ||
      c.activation_generation == 0 || c.connection_generation == 0 ||
      c.session_id != 0 || c.session_generation != 0 || c.sequence != 0) {
    return Reply(request, Status::InvalidFrame);
  }
  const auto client = clients_.find(c.client_id);
  if (client == clients_.end()) {
    return Reply(request,
                 IsAbandonedEpochLocked(c, &owner) ? Status::Ok
                                                   : Status::StaleRequest);
  }
  if (client->second.activation_generation != c.activation_generation ||
      client->second.connection_generation != c.connection_generation ||
      client->second.owner != owner) {
    return Reply(request, Status::StaleRequest);
  }

  const size_t context_count =
      std::count_if(sessions_.begin(), sessions_.end(),
                    [&](const auto &entry) {
                      return entry.first.client_id == c.client_id &&
                             entry.first.activation_generation ==
                                 c.activation_generation &&
                             entry.first.connection_generation ==
                                 c.connection_generation;
                    });
  if (!EnsureRetiredCapacityLocked(context_count) ||
      !RememberAbandonedEpochLocked(c, owner)) {
    return Reply(request, Status::Unavailable);
  }

  // Record the terminal epoch before erasing its exact deliveries. Any late
  // Prepare already read from an older physical pipe is serialized by mutex_
  // and will match the tombstone instead of reviving this connection.
  std::erase_if(deliveries_, [&](const DeliveryEntry &entry) {
    const Correlation &target = entry.reference.correlation;
    return target.client_id == c.client_id &&
           target.activation_generation == c.activation_generation &&
           target.connection_generation == c.connection_generation;
  });
  std::erase_if(acknowledged_deliveries_,
                [&](const AcknowledgedDelivery &entry) {
                  const Correlation &target =
                      entry.reference.correlation;
                  return target.client_id == c.client_id &&
                         target.activation_generation ==
                             c.activation_generation &&
                         target.connection_generation ==
                             c.connection_generation;
                });
  std::erase_if(abandoned_sessions_, [&](const AbandonedSession &entry) {
    const Correlation &target = entry.reference.correlation;
    return target.client_id == c.client_id &&
           target.activation_generation == c.activation_generation &&
           target.connection_generation == c.connection_generation;
  });

  std::lock_guard ui_lock(ui_sessions_mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->first.client_id == c.client_id &&
        it->first.activation_generation == c.activation_generation &&
        it->first.connection_generation == c.connection_generation) {
      Publish(it->second, false);
      ui_sessions_.erase(it->first);
      (void)DestroyOrRetireContextLocked(it->second.context);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  clients_.erase(client);
  return Reply(request, Status::Ok);
}

Frame RuntimeService::AbandonSessionLocked(
    const Frame &request, const DeliveryReference &reference,
    const PipeClientIdentity &owner) {
  const Correlation &caller = request.correlation;
  const Correlation &target = reference.correlation;
  if (!started_ || !owner || request.flags != 0 ||
      request.status != Status::Ok ||
      !IsDeliveryTracked(reference.command) || caller.client_id == 0 ||
      caller.activation_generation == 0 ||
      caller.connection_generation == 0 || caller.session_id != 0 ||
      caller.session_generation != 0 || caller.sequence != 0 ||
      target.client_id != caller.client_id ||
      target.activation_generation != caller.activation_generation ||
      target.connection_generation != caller.connection_generation ||
      target.session_id == 0 || target.session_generation == 0 ||
      target.sequence == 0) {
    return Reply(request, Status::InvalidFrame);
  }

  const AbandonedSession *tombstone =
      FindAbandonedSessionLocked(target);
  const auto client = clients_.find(caller.client_id);
  if (client == clients_.end()) {
    return Reply(request,
                 tombstone && tombstone->owner == owner &&
                         tombstone->reference == reference
                     ? Status::Ok
                     : Status::StaleRequest);
  }
  if (client->second.activation_generation !=
          caller.activation_generation ||
      client->second.connection_generation !=
          caller.connection_generation ||
      client->second.owner != owner) {
    return Reply(request, Status::StaleRequest);
  }
  if (tombstone) {
    return Reply(request,
                 tombstone->owner == owner &&
                         tombstone->reference == reference
                     ? Status::Ok
                     : Status::StaleRequest);
  }

  const auto delivery = std::find_if(
      deliveries_.begin(), deliveries_.end(),
      [&](const DeliveryEntry &entry) {
        return entry.reference == reference;
      });
  if (delivery == deliveries_.end() || delivery->owner != owner ||
      delivery->stage != DeliveryStage::TerminalFailed) {
    return Reply(request, Status::StaleRequest);
  }
  const SessionKey key{
      target.client_id, target.activation_generation,
      target.connection_generation, target.session_id,
      target.session_generation};
  const auto session = sessions_.find(key);
  if (session == sessions_.end())
    return Reply(request, Status::StaleRequest);
  if (!EnsureRetiredCapacityLocked(1) ||
      !RememberAbandonedSessionLocked(reference, owner)) {
    return Reply(request, Status::Unavailable);
  }

  // The exact owner/session/delivery tombstone is durable before destructive
  // cleanup. Late Claim, Recover, Prepare, or OpenSession calls therefore fail
  // closed without affecting sibling sessions in the connection epoch.
  std::erase_if(deliveries_, [&](const DeliveryEntry &entry) {
    return SameLogicalSession(entry.reference.correlation, target);
  });
  std::erase_if(acknowledged_deliveries_,
                [&](const AcknowledgedDelivery &entry) {
                  return SameLogicalSession(entry.reference.correlation,
                                            target);
                });
  {
    std::lock_guard ui_lock(ui_sessions_mutex_);
    Publish(session->second, false);
    ui_sessions_.erase(key);
    (void)DestroyOrRetireContextLocked(session->second.context);
    sessions_.erase(session);
  }
  return Reply(request, Status::Ok);
}

bool RuntimeService::ValidateDeliveryRequestLocked(
    const Frame &request) const {
  if (!started_ || request.flags != 0 || request.status != Status::Ok ||
      !IsDeliveryTracked(request.command))
    return false;
  const Correlation &c = request.correlation;
  if (c.client_id == 0 || c.activation_generation == 0 ||
      c.connection_generation == 0 || c.session_id == 0 ||
      c.session_generation == 0 || c.sequence == 0) {
    return false;
  }
  const auto client = clients_.find(c.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != c.activation_generation ||
      client->second.connection_generation != c.connection_generation) {
    return false;
  }
  const SessionKey key{c.client_id, c.activation_generation,
                       c.connection_generation, c.session_id,
                       c.session_generation};
  const auto session = sessions_.find(key);
  if (session == sessions_.end() || c.sequence <= session->second.last_sequence)
    return false;

  std::string error;
  if (request.command == Command::ProcessKey) {
    KeyEvent value;
    return DecodeKeyEvent(request.payload, &value, &error);
  }
  if (request.command == Command::SelectCandidate ||
      request.command == Command::HighlightCandidate) {
    uint32_t index = 0;
    return DecodeCandidateIndex(request.payload, &index, &error);
  }
  if (request.command == Command::SelectCandidateAbsolute) {
    uint32_t index = 0;
    uint64_t expected_sequence = 0;
    return DecodeAbsoluteCandidateSelection(request.payload, &index,
                                            &expected_sequence, &error);
  }
  if (request.command == Command::ChangePage) {
    bool backward = false;
    return DecodePageDirection(request.payload, &backward, &error);
  }
  return request.payload.empty();
}

Frame RuntimeService::PrepareDeliveryLocked(
    const Frame &request, const PipeClientIdentity &owner) {
  if (!owner || (request.flags & kFlagResponse) != 0 ||
      (request.flags & ~kFlagAcknowledgePrevious) != 0 ||
      request.status != Status::Ok) {
    return Reply(request, Status::InvalidFrame);
  }
  Frame prepared_request = request;
  prepared_request.flags = 0;
  const DeliveryReference reference{request.command, request.correlation};

  const auto cancelled = std::find_if(
      acknowledged_deliveries_.begin(), acknowledged_deliveries_.end(),
      [&](const AcknowledgedDelivery &entry) {
        return entry.reference == reference;
      });
  if (cancelled != acknowledged_deliveries_.end())
    return Reply(request, Status::StaleRequest);

  const Correlation &target = reference.correlation;
  const SessionKey target_key{
      target.client_id, target.activation_generation,
      target.connection_generation, target.session_id,
      target.session_generation};
  const auto target_session = sessions_.find(target_key);
  if (FindAbandonedSessionLocked(target)) {
    return Reply(request, Status::StaleRequest);
  }
  if (target_session != sessions_.end() &&
      target.sequence <= target_session->second.last_sequence) {
    return Reply(request, Status::StaleRequest);
  }

  const auto existing = std::find_if(
      deliveries_.begin(), deliveries_.end(), [&](const DeliveryEntry &entry) {
        return entry.reference == reference;
      });
  if (existing != deliveries_.end()) {
    return existing->stage == DeliveryStage::Prepared &&
                   existing->owner == owner &&
                   SameFrame(existing->request, prepared_request)
               ? Reply(request, Status::Prepared)
               : Reply(request, Status::StaleRequest);
  }

  if ((request.flags & kFlagAcknowledgePrevious) != 0) {
    const auto previous = std::find_if(
        deliveries_.begin(), deliveries_.end(),
        [&](const DeliveryEntry &entry) {
          return SameLogicalSession(entry.reference.correlation,
                                    request.correlation);
        });
    if (previous != deliveries_.end()) {
      if (previous->owner != owner ||
          previous->stage != DeliveryStage::Completed) {
        return Reply(request, Status::StaleRequest);
      }
      const DeliveryReference acknowledged = previous->reference;
      if (!RememberAcknowledgedDeliveryLocked(acknowledged, owner))
        return Reply(request, Status::Unavailable);
      deliveries_.erase(previous);
      CleanupDeliverySessionLocked(acknowledged);
    }
  }

  if (!ValidateDeliveryRequestLocked(prepared_request))
    return Reply(request, Status::InvalidFrame);
  const bool session_busy =
      std::any_of(deliveries_.begin(), deliveries_.end(),
                  [&](const DeliveryEntry &entry) {
                    return SameLogicalSession(entry.reference.correlation,
                                              request.correlation);
                  });
  if (session_busy)
    return Reply(request, Status::Unavailable);

  if (deliveries_.size() >= kMaxDeliveries ||
      std::count_if(deliveries_.begin(), deliveries_.end(),
                    [&](const DeliveryEntry &entry) {
                      return entry.reference.correlation.client_id ==
                             request.correlation.client_id;
                    }) >= kMaxDeliveriesPerClient) {
    try {
      CleanupDeadDeliveriesLocked();
    } catch (...) {
      return Reply(request, Status::Unavailable);
    }
  }
  if (deliveries_.size() >= kMaxDeliveries ||
      std::count_if(deliveries_.begin(), deliveries_.end(),
                    [&](const DeliveryEntry &entry) {
                      return entry.reference.correlation.client_id ==
                             request.correlation.client_id;
                    }) >= kMaxDeliveriesPerClient) {
    return Reply(request, Status::Unavailable);
  }

  try {
    deliveries_.push_back(
        {reference, std::move(prepared_request), {}, owner,
         DeliveryStage::Prepared});
  } catch (...) {
    return Reply(request, Status::Unavailable);
  }
  return Reply(request, Status::Prepared);
}

Frame RuntimeService::ExecutePreparedLocked(
    const Frame &request, const DeliveryReference &reference,
    const PipeClientIdentity &owner) {
  const Correlation &caller = request.correlation;
  if (!owner || caller.client_id == 0 ||
      caller.activation_generation == 0 ||
      caller.connection_generation == 0 || caller.session_id != 0 ||
      caller.session_generation != 0 || caller.sequence != 0) {
    return Reply(request, Status::InvalidFrame);
  }
  if (caller.client_id != reference.correlation.client_id ||
      caller.activation_generation !=
          reference.correlation.activation_generation ||
      caller.connection_generation !=
          reference.correlation.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto client = clients_.find(caller.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != caller.activation_generation ||
      client->second.connection_generation != caller.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto found = std::find_if(
      deliveries_.begin(), deliveries_.end(), [&](const DeliveryEntry &entry) {
        return entry.reference == reference;
      });
  if (found == deliveries_.end() || found->owner != owner)
    return Reply(request, Status::StaleRequest);
  if (found->stage == DeliveryStage::Completed)
    return found->final_reply;
  if (found->stage == DeliveryStage::TerminalFailed)
    return Reply(found->request, Status::DeliveryFailed);
  if (found->stage == DeliveryStage::EncodingFailed)
    return RetryCachedDeliveryEncodingLocked(*found);
  if (found->stage != DeliveryStage::Prepared &&
      found->stage != DeliveryStage::PendingRecovery) {
    return Reply(request, Status::Unavailable);
  }
  return AdvanceDeliveryLocked(*found);
}

Frame RuntimeService::AdvanceDeliveryLocked(DeliveryEntry &delivery) {
  // One recovery/finalization attempt is made while executing the prepared
  // request. Claims consume the remaining shared budget.
  constexpr uint32_t kMaxRecoveryAttempts = 3;
  const Correlation &target = delivery.reference.correlation;
  const SessionKey key{target.client_id, target.activation_generation,
                       target.connection_generation, target.session_id,
                       target.session_generation};
  const auto session = sessions_.find(key);
  const bool recovering = delivery.stage == DeliveryStage::PendingRecovery;
  if (session == sessions_.end() ||
      (!recovering && target.sequence <= session->second.last_sequence) ||
      (recovering && target.sequence != session->second.last_sequence)) {
    delivery.final_reply = Reply(delivery.request, Status::StaleRequest);
    delivery.stage = DeliveryStage::EncodingFailed;
    return RetryCachedDeliveryEncodingLocked(delivery);
  }

  delivery.stage = DeliveryStage::Executing;
  Frame final_reply;
  try {
    final_reply =
        recovering
            ? RecoverSessionCommand(delivery.request, key, session->second)
            : DispatchSessionCommand(delivery.request, key, session->second);
    if (final_reply.status == Status::RecoveryPending) {
      if (!recovering)
        delivery.recovery_attempts = 1;
      if (recovering &&
          ++delivery.recovery_attempts >= kMaxRecoveryAttempts) {
        delivery.stage = DeliveryStage::TerminalFailed;
        return Reply(delivery.request, Status::DeliveryFailed);
      }
      delivery.stage = DeliveryStage::PendingRecovery;
      return final_reply;
    }
    delivery.final_reply = std::move(final_reply);
    if (TestFaultEnabled(L"FAMO_TEST_RUNTIME_MALFORMED_FINAL_REPLY"))
      delivery.final_reply.flags = 0;
    delivery.stage = DeliveryStage::EncodingFailed;
    return RetryCachedDeliveryEncodingLocked(delivery);
  } catch (...) {
    // DispatchSessionCommand records exact RECOVER metadata after a business
    // mutation whose final snapshot is still owed. Only that evidence permits
    // a Claim retry; an otherwise empty/invalid final frame is terminal and
    // must never spin forever or replay the original action.
    if (ValidFinalReply(delivery.final_reply, delivery.reference)) {
      delivery.stage = DeliveryStage::EncodingFailed;
      return RetryCachedDeliveryEncodingLocked(delivery);
    }
    const auto recoverable_session = sessions_.find(key);
    const bool recoverable =
        recoverable_session != sessions_.end() &&
        recoverable_session->second.pending_recovery_action != 0 &&
        recoverable_session->second.pending_recovery_sequence ==
            target.sequence;
    if (recoverable && !recovering)
      delivery.recovery_attempts = 1;
    if (recoverable && recovering &&
        ++delivery.recovery_attempts >= kMaxRecoveryAttempts) {
      delivery.stage = DeliveryStage::TerminalFailed;
      return Reply(delivery.request, Status::DeliveryFailed);
    }
    delivery.stage = recoverable ? DeliveryStage::PendingRecovery
                                 : DeliveryStage::TerminalFailed;
    return Reply(delivery.request, recoverable ? Status::RecoveryPending
                                               : Status::DeliveryFailed);
  }
}

Frame RuntimeService::RetryCachedDeliveryEncodingLocked(
    DeliveryEntry &delivery) {
  if (!ValidFinalReply(delivery.final_reply, delivery.reference)) {
    delivery.stage = DeliveryStage::TerminalFailed;
    return Reply(delivery.request, Status::DeliveryFailed);
  }
  try {
    if (TestFaultEnabled(
            L"FAMO_TEST_RUNTIME_DELIVERY_ENCODING_ALLOCATION_FAILURE")) {
      throw std::bad_alloc();
    }
    std::vector<uint8_t> encoded;
    std::string error;
    if (!EncodeFrame(delivery.final_reply, &encoded, &error)) {
      // Structural fields were validated above, so a remaining encode failure
      // is transient allocation pressure. Preserve the exact cached result.
      delivery.stage = DeliveryStage::EncodingFailed;
      return Reply(delivery.request, Status::RecoveryPending);
    }
    delivery.stage = DeliveryStage::Completed;
  } catch (...) {
    delivery.stage = DeliveryStage::EncodingFailed;
    return Reply(delivery.request, Status::RecoveryPending);
  }
  return delivery.final_reply;
}

Frame RuntimeService::ClaimDeliveryLocked(
    const Frame &request, const DeliveryReference &reference,
    const PipeClientIdentity &owner) {
  const Correlation &caller = request.correlation;
  if (!owner || caller.client_id == 0 ||
      caller.activation_generation == 0 ||
      caller.connection_generation == 0 || caller.session_id != 0 ||
      caller.session_generation != 0 || caller.sequence != 0) {
    return Reply(request, Status::InvalidFrame);
  }
  if (caller.client_id != reference.correlation.client_id ||
      caller.activation_generation !=
          reference.correlation.activation_generation ||
      caller.connection_generation !=
          reference.correlation.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto client = clients_.find(caller.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != caller.activation_generation ||
      client->second.connection_generation != caller.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto found = std::find_if(
      deliveries_.begin(), deliveries_.end(), [&](const DeliveryEntry &entry) {
        return entry.reference == reference;
      });
  if (found == deliveries_.end() || found->owner != owner)
    return Reply(request, Status::StaleRequest);
  if (found->stage == DeliveryStage::Prepared)
    return Reply(request, Status::Prepared);
  if (found->stage == DeliveryStage::Executing)
    return Reply(request, Status::Unavailable);
  if (found->stage == DeliveryStage::TerminalFailed)
    return Reply(found->request, Status::DeliveryFailed);
  if (found->stage == DeliveryStage::PendingRecovery)
    return AdvanceDeliveryLocked(*found);
  if (found->stage == DeliveryStage::EncodingFailed)
    return RetryCachedDeliveryEncodingLocked(*found);
  return found->final_reply;
}

Frame RuntimeService::AcknowledgeDeliveryLocked(
    const Frame &request, const DeliveryReference &reference,
    const PipeClientIdentity &owner) {
  const Correlation &caller = request.correlation;
  if (!owner || caller.client_id == 0 ||
      caller.activation_generation == 0 ||
      caller.connection_generation == 0 || caller.session_id != 0 ||
      caller.session_generation != 0 || caller.sequence != 0) {
    return Reply(request, Status::InvalidFrame);
  }
  if (caller.client_id != reference.correlation.client_id ||
      caller.activation_generation !=
          reference.correlation.activation_generation ||
      caller.connection_generation !=
          reference.correlation.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto client = clients_.find(caller.client_id);
  if (client == clients_.end() ||
      client->second.activation_generation != caller.activation_generation ||
      client->second.connection_generation != caller.connection_generation) {
    return Reply(request, Status::StaleRequest);
  }
  const auto found = std::find_if(
      deliveries_.begin(), deliveries_.end(), [&](const DeliveryEntry &entry) {
        return entry.reference == reference;
      });
  if (found != deliveries_.end()) {
    if (found->owner != owner ||
        (found->stage != DeliveryStage::Prepared &&
         found->stage != DeliveryStage::Completed))
      return Reply(request, Status::StaleRequest);
    const bool cancels_prepared =
        found->stage == DeliveryStage::Prepared;
    const Correlation cancelled_correlation =
        found->reference.correlation;
    auto cancelled_session = sessions_.end();
    if (cancels_prepared) {
      const SessionKey key{
          cancelled_correlation.client_id,
          cancelled_correlation.activation_generation,
          cancelled_correlation.connection_generation,
          cancelled_correlation.session_id,
          cancelled_correlation.session_generation};
      cancelled_session = sessions_.find(key);
      if (cancelled_session == sessions_.end() ||
          cancelled_correlation.sequence <=
              cancelled_session->second.last_sequence) {
        return Reply(request, Status::StaleRequest);
      }
    }
    if (!RememberAcknowledgedDeliveryLocked(reference, owner))
      return Reply(request, Status::Unavailable);
    if (cancels_prepared) {
      cancelled_session->second.last_sequence =
          cancelled_correlation.sequence;
      cancelled_session->second.correlation = cancelled_correlation;
    }
    deliveries_.erase(found);
    CleanupDeliverySessionLocked(reference);
    return Reply(request, Status::Ok);
  }
  const bool acknowledged =
      std::any_of(acknowledged_deliveries_.begin(),
                  acknowledged_deliveries_.end(),
                  [&](const AcknowledgedDelivery &entry) {
                    return entry.reference == reference &&
                           entry.owner == owner;
                  });
  if (acknowledged)
    return Reply(request, Status::Ok);
  // ACK also cancels a Prepare whose response was lost and whose server-side
  // dispatch may still be racing this reconnect. Record the tombstone before
  // replying so that an exact late Prepare cannot create an orphan delivery.
  const Correlation &target = reference.correlation;
  const SessionKey key{target.client_id, target.activation_generation,
                       target.connection_generation, target.session_id,
                       target.session_generation};
  const auto session = sessions_.find(key);
  if (session == sessions_.end() ||
      target.sequence <= session->second.last_sequence ||
      HasOutstandingDeliveryLocked(key)) {
    return Reply(request, Status::StaleRequest);
  }
  if (!RememberAcknowledgedDeliveryLocked(reference, owner))
    return Reply(request, Status::Unavailable);
  session->second.last_sequence = target.sequence;
  session->second.correlation = target;
  return Reply(request, Status::Ok);
}

bool RuntimeService::RememberAcknowledgedDeliveryLocked(
    const DeliveryReference &reference, const PipeClientIdentity &owner) {
  wchar_t injected[2]{};
  if (GetEnvironmentVariableW(
          L"FAMO_TEST_RUNTIME_ACK_ALLOCATION_FAILURE", injected,
          static_cast<DWORD>(2)) > 0) {
    return false;
  }
  try {
    acknowledged_deliveries_.push_back({reference, owner});
  } catch (...) {
    return false;
  }
  if (acknowledged_deliveries_.size() > kMaxAcknowledgements)
    acknowledged_deliveries_.erase(acknowledged_deliveries_.begin());
  while (std::count_if(
             acknowledged_deliveries_.begin(),
             acknowledged_deliveries_.end(),
             [&](const AcknowledgedDelivery &entry) {
               return entry.reference.correlation.client_id ==
                      reference.correlation.client_id;
             }) > kMaxAcknowledgementsPerClient) {
    const auto oldest = std::find_if(
        acknowledged_deliveries_.begin(), acknowledged_deliveries_.end(),
        [&](const AcknowledgedDelivery &entry) {
          return entry.reference.correlation.client_id ==
                     reference.correlation.client_id &&
                 entry.reference != reference;
        });
    if (oldest == acknowledged_deliveries_.end())
      break;
    acknowledged_deliveries_.erase(oldest);
  }
  return true;
}

bool RuntimeService::HasOutstandingDeliveryLocked(
    const SessionKey &key) const {
  return std::any_of(deliveries_.begin(), deliveries_.end(),
                     [&](const DeliveryEntry &entry) {
                       const Correlation &c = entry.reference.correlation;
                       return c.client_id == key.client_id &&
                              c.activation_generation ==
                                  key.activation_generation &&
                              c.connection_generation ==
                                  key.connection_generation &&
                              c.session_id == key.session_id &&
                              c.session_generation == key.session_generation;
                     });
}

bool RuntimeService::HasOutstandingConnectionLocked(
    uint64_t client_id, uint64_t activation_generation,
    uint64_t connection_generation) const {
  return std::any_of(
      deliveries_.begin(), deliveries_.end(),
      [&](const DeliveryEntry &entry) {
        const Correlation &c = entry.reference.correlation;
        return c.client_id == client_id &&
               c.activation_generation == activation_generation &&
               c.connection_generation == connection_generation;
      });
}

void RuntimeService::CleanupDeliverySessionLocked(
    const DeliveryReference &reference) {
  const Correlation &c = reference.correlation;
  const SessionKey key{c.client_id, c.activation_generation,
                       c.connection_generation, c.session_id,
                       c.session_generation};
  if (HasOutstandingDeliveryLocked(key))
    return;
  const auto client = clients_.find(c.client_id);
  if (client != clients_.end() &&
      client->second.activation_generation == c.activation_generation &&
      client->second.connection_generation == c.connection_generation) {
    return;
  }
  const auto session = sessions_.find(key);
  if (session == sessions_.end())
    return;
  if (!EnsureRetiredCapacityLocked(1))
    return;
  {
    std::lock_guard ui_lock(ui_sessions_mutex_);
    ui_sessions_.erase(key);
    Publish(session->second, false);
  }
  if (!DestroyOrRetireContextLocked(session->second.context))
    return;
  sessions_.erase(session);
}

void RuntimeService::CleanupDeadDeliveriesLocked() {
  // A dead owner terminates its whole authenticated epoch. Removing only the
  // delivery would leave the client/session/context alive because normal
  // reconnect cleanup deliberately preserves sessions for a live owner.
  CleanupDeadOwnersLocked();
}

} // namespace famo::runtime
