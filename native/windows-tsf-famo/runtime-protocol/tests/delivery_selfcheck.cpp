#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

#include "famo_runtime_service.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #x, __FILE__,         \
                   __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

using namespace famo::runtime;

namespace famo::runtime {

struct RuntimeServiceTestAccess {
  static uint64_t LastSequence(RuntimeService &service,
                               const Correlation &correlation) {
    std::lock_guard lock(service.mutex_);
    const RuntimeService::SessionKey key{
        correlation.client_id, correlation.activation_generation,
        correlation.connection_generation, correlation.session_id,
        correlation.session_generation};
    const auto found = service.sessions_.find(key);
    return found == service.sessions_.end() ? 0
                                            : found->second.last_sequence;
  }

  static size_t AcknowledgementCount(RuntimeService &service,
                                     uint64_t client_id) {
    std::lock_guard lock(service.mutex_);
    size_t count = 0;
    for (const auto &entry : service.acknowledged_deliveries_) {
      if (entry.reference.correlation.client_id == client_id)
        ++count;
    }
    return count;
  }

  static size_t DeliveryCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.deliveries_.size();
  }

  static size_t SessionCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.sessions_.size();
  }

  static size_t ClientCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.clients_.size();
  }

  static size_t RetiredContextCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.retired_contexts_.size();
  }

  static size_t AbandonedEpochCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.abandoned_epochs_.size();
  }

  static size_t AbandonedSessionCount(RuntimeService &service) {
    std::lock_guard lock(service.mutex_);
    return service.abandoned_sessions_.size();
  }

  static int32_t ProcessCount(RuntimeService &service,
                              const Correlation &correlation) {
    std::lock_guard lock(service.mutex_);
    const RuntimeService::SessionKey key{
        correlation.client_id, correlation.activation_generation,
        correlation.connection_generation, correlation.session_id,
        correlation.session_generation};
    const auto found = service.sessions_.find(key);
    if (found == service.sessions_.end())
      return -1;
    static constexpr char kOptionName[] = "test_process_count";
    const FamoUtf8String option{
        sizeof(FamoUtf8String), kOptionName,
        static_cast<uint32_t>(sizeof(kOptionName) - 1)};
    int32_t count = -1;
    return service.engine_.GetOption(found->second.context, &option, &count) ==
                   FAMO_ENGINE_OK
               ? count
               : -1;
  }

  static void SetReadiness(RuntimeService &service,
                           RuntimeReadiness readiness) {
    service.readiness_.store(readiness);
  }

  static constexpr size_t MaxClientsPerOwner() {
    return RuntimeService::kMaxClientsPerOwner;
  }

  static constexpr size_t MaxClients() {
    return RuntimeService::kMaxClients;
  }

  static constexpr size_t MaxSessionsPerClient() {
    return RuntimeService::kMaxSessionsPerClient;
  }

  static constexpr size_t MaxSessionsPerOwner() {
    return RuntimeService::kMaxSessionsPerOwner;
  }

  static constexpr size_t MaxAbandonedSessionsPerOwner() {
    return RuntimeService::kMaxAbandonedSessionsPerOwner;
  }
};

} // namespace famo::runtime

namespace {

PipeClientIdentity CurrentProcessIdentity() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    return {};
  ULARGE_INTEGER value{};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  return {GetCurrentProcessId(), value.QuadPart};
}

Frame Connection(Command command) {
  Frame frame;
  frame.command = command;
  frame.correlation = {101, 102, 103, 0, 0, 0};
  return frame;
}

Frame Session(Command command, uint64_t sequence) {
  Frame frame;
  frame.command = command;
  frame.correlation = {101, 102, 103, 104, 105, sequence};
  return frame;
}

Frame DeliveryControl(Command command, const DeliveryReference &reference) {
  Frame frame;
  frame.command = command;
  const Correlation &target = reference.correlation;
  frame.correlation = {target.client_id, target.activation_generation,
                       target.connection_generation, 0, 0, 0};
  EncodeDeliveryReference(reference, &frame.payload);
  return frame;
}

Frame ClientConnection(Command command, uint64_t client_id,
                       uint64_t activation = 1, uint64_t connection = 1) {
  Frame frame;
  frame.command = command;
  frame.correlation = {client_id, activation, connection, 0, 0, 0};
  return frame;
}

Frame ClientSession(Command command, uint64_t client_id, uint64_t session_id,
                    uint64_t sequence = 1) {
  Frame frame;
  frame.command = command;
  frame.correlation = {client_id, 1, 1, session_id, 1, sequence};
  return frame;
}

bool SpawnIdleOwner(PROCESS_INFORMATION *process,
                    PipeClientIdentity *identity) {
  if (!process || !identity)
    return false;
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    return false;
  path.resize(length);
  std::wstring command = L"\"" + path + L"\" --idle-owner";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, process)) {
    return false;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process->hProcess, &created, &exited, &kernel, &user)) {
    TerminateProcess(process->hProcess, 9);
    WaitForSingleObject(process->hProcess, 1000);
    CloseHandle(process->hThread);
    CloseHandle(process->hProcess);
    *process = {};
    return false;
  }
  ULARGE_INTEGER value{};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  *identity = {process->dwProcessId, value.QuadPart};
  return static_cast<bool>(*identity);
}

int LifecycleCapsAndDeadOwnerCleanup(const PipeClientIdentity &owner) {
  std::string error;
  RuntimeService service;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);

  for (size_t index = 0;
       index < RuntimeServiceTestAccess::MaxClientsPerOwner(); ++index) {
    CHECK(service.DispatchForDelivery(
                      ClientConnection(Command::Hello, 1000 + index), owner)
              .status == Status::Ok);
  }
  CHECK(RuntimeServiceTestAccess::ClientCount(service) ==
        RuntimeServiceTestAccess::MaxClientsPerOwner());
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, 2000), owner)
            .status == Status::Unavailable);
  for (size_t index = 0;
       index < RuntimeServiceTestAccess::MaxClientsPerOwner(); ++index) {
    CHECK(service.DispatchForDelivery(
                      ClientConnection(Command::AbandonConnection,
                                       1000 + index),
                      owner)
              .status == Status::Ok);
  }
  CHECK(RuntimeServiceTestAccess::ClientCount(service) == 0);
  CHECK(RuntimeServiceTestAccess::AbandonedEpochCount(service) <=
        RuntimeServiceTestAccess::MaxClientsPerOwner());

  constexpr uint64_t kFirstSessionClient = 3000;
  constexpr uint64_t kSecondSessionClient = 3001;
  constexpr uint64_t kThirdSessionClient = 3002;
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kFirstSessionClient),
                    owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kSecondSessionClient),
                    owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kThirdSessionClient),
                    owner)
            .status == Status::Ok);
  for (size_t index = 0;
       index < RuntimeServiceTestAccess::MaxSessionsPerClient(); ++index) {
    Frame open = ClientSession(Command::OpenSession, kFirstSessionClient,
                               10 + index);
    CHECK(EncodeOpenSession("test", &open.payload, &error));
    CHECK(service.DispatchForDelivery(open, owner).status == Status::Ok);
  }
  Frame over_client =
      ClientSession(Command::OpenSession, kFirstSessionClient, 100);
  CHECK(EncodeOpenSession("test", &over_client.payload, &error));
  CHECK(service.DispatchForDelivery(over_client, owner).status ==
        Status::Unavailable);

  for (size_t index = RuntimeServiceTestAccess::MaxSessionsPerClient();
       index < RuntimeServiceTestAccess::MaxSessionsPerOwner(); ++index) {
    Frame open = ClientSession(Command::OpenSession, kSecondSessionClient,
                               10 + index);
    CHECK(EncodeOpenSession("test", &open.payload, &error));
    CHECK(service.DispatchForDelivery(open, owner).status == Status::Ok);
  }
  CHECK(RuntimeServiceTestAccess::SessionCount(service) ==
        RuntimeServiceTestAccess::MaxSessionsPerOwner());
  Frame over_owner =
      ClientSession(Command::OpenSession, kThirdSessionClient, 100);
  CHECK(EncodeOpenSession("test", &over_owner.payload, &error));
  CHECK(service.DispatchForDelivery(over_owner, owner).status ==
        Status::Unavailable);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection,
                                     kFirstSessionClient),
                    owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection,
                                     kSecondSessionClient),
                    owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection,
                                     kThirdSessionClient),
                    owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 0);

  PROCESS_INFORMATION child{};
  PipeClientIdentity child_owner;
  CHECK(SpawnIdleOwner(&child, &child_owner));
  constexpr uint64_t kDeadClient = 4000;
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kDeadClient),
                    child_owner)
            .status == Status::Ok);
  Frame dead_open =
      ClientSession(Command::OpenSession, kDeadClient, 1);
  CHECK(EncodeOpenSession("test", &dead_open.payload, &error));
  CHECK(service.DispatchForDelivery(dead_open, child_owner).status ==
        Status::Ok);
  CHECK(TerminateProcess(child.hProcess, 0));
  CHECK(WaitForSingleObject(child.hProcess, 1000) == WAIT_OBJECT_0);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, 4001), owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 0);
  CHECK(RuntimeServiceTestAccess::ClientCount(service) == 1);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection, 4001),
                    owner)
            .status == Status::Ok);

  constexpr uint64_t kDestroyFailureClient = 5000;
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kDestroyFailureClient),
                    owner)
            .status == Status::Ok);
  Frame destroy_open =
      ClientSession(Command::OpenSession, kDestroyFailureClient, 1);
  CHECK(EncodeOpenSession("test", &destroy_open.payload, &error));
  CHECK(service.DispatchForDelivery(destroy_open, owner).status ==
        Status::Ok);
  CHECK(_putenv_s("FAMO_TEST_FAIL_DESTROY", "1") == 0);
  RuntimeServiceTestAccess::SetReadiness(
      service, RuntimeReadiness::Maintenance);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection,
                                     kDestroyFailureClient),
                    owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 0);
  CHECK(RuntimeServiceTestAccess::ClientCount(service) == 0);
  CHECK(RuntimeServiceTestAccess::RetiredContextCount(service) == 1);
  CHECK(_putenv_s("FAMO_TEST_FAIL_DESTROY", "") == 0);
  RuntimeServiceTestAccess::SetReadiness(service, RuntimeReadiness::Ready);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, 5001), owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::RetiredContextCount(service) == 0);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection, 5001),
                    owner)
            .status == Status::Ok);

  static_assert(RuntimeServiceTestAccess::MaxClients() ==
                kRuntimeClientCapacity);
  for (size_t index = 0;
       index < RuntimeServiceTestAccess::MaxClients(); ++index) {
    CHECK(service
              .Dispatch(ClientConnection(Command::Hello, 6000 + index))
              .status == Status::Ok);
  }
  CHECK(RuntimeServiceTestAccess::ClientCount(service) ==
        kRuntimeClientCapacity);
  CHECK(service
            .Dispatch(ClientConnection(Command::Hello,
                                       6000 + kRuntimeClientCapacity))
            .status == Status::Unavailable);
  service.Stop();
  return 0;
}

int AbandonedSessionHighWaterRejectsEvictedReplay(
    const PipeClientIdentity &owner) {
  constexpr uint64_t kClientId = 7000;
  std::string error;
  RuntimeService service;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::Hello, kClientId), owner)
            .status == Status::Ok);

  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_MALFORMED_FINAL_REPLY", L"1"));
  const uint64_t abandon_count =
      RuntimeServiceTestAccess::MaxAbandonedSessionsPerOwner() + 1;
  for (uint64_t session_id = 1; session_id <= abandon_count; ++session_id) {
    Frame open =
        ClientSession(Command::OpenSession, kClientId, session_id);
    CHECK(EncodeOpenSession("test", &open.payload, &error));
    CHECK(service.DispatchForDelivery(open, owner).status == Status::Ok);

    Frame key =
        ClientSession(Command::ProcessKey, kClientId, session_id, 2);
    CHECK(EncodeKeyEvent({static_cast<uint32_t>('A'), 0, 0, 1, session_id},
                         &key.payload));
    const DeliveryReference reference{key.command, key.correlation};
    CHECK(service.DispatchForDelivery(key, owner).status == Status::Prepared);
    CHECK(service.DispatchForDelivery(
                      DeliveryControl(Command::ExecutePrepared, reference),
                      owner)
              .status == Status::DeliveryFailed);
    CHECK(service.DispatchForDelivery(
                      DeliveryControl(Command::AbandonSession, reference),
                      owner)
              .status == Status::Ok);
  }
  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_MALFORMED_FINAL_REPLY", nullptr));
  CHECK(RuntimeServiceTestAccess::AbandonedSessionCount(service) ==
        RuntimeServiceTestAccess::MaxAbandonedSessionsPerOwner());

  Frame evicted_replay =
      ClientSession(Command::OpenSession, kClientId, 1);
  CHECK(EncodeOpenSession("test", &evicted_replay.payload, &error));
  CHECK(service.DispatchForDelivery(evicted_replay, owner).status ==
        Status::StaleRequest);

  Frame next_open =
      ClientSession(Command::OpenSession, kClientId, abandon_count + 1);
  CHECK(EncodeOpenSession("test", &next_open.payload, &error));
  CHECK(service.DispatchForDelivery(next_open, owner).status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    ClientConnection(Command::AbandonConnection, kClientId),
                    owner)
            .status == Status::Ok);
  service.Stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--idle-owner") == 0) {
    Sleep(60000);
    return 0;
  }
  std::string error;
  const PipeClientIdentity owner = CurrentProcessIdentity();
  CHECK(owner);
  CHECK(LifecycleCapsAndDeadOwnerCleanup(owner) == 0);
  CHECK(AbandonedSessionHighWaterRejectsEvictedReplay(owner) == 0);

  RuntimeService service;
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  CHECK(service.DispatchForDelivery(Connection(Command::Hello), owner).status ==
        Status::Ok);
  Frame open = Session(Command::OpenSession, 1);
  CHECK(EncodeOpenSession("test", &open.payload, &error));
  CHECK(service.DispatchForDelivery(open, owner).status == Status::Ok);

  Frame key_n = Session(Command::ProcessKey, 2);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 1},
                       &key_n.payload));
  const DeliveryReference n_reference{key_n.command, key_n.correlation};
  CHECK(service.DispatchForDelivery(key_n, owner).status == Status::Prepared);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult, n_reference), owner)
            .status == Status::Prepared);

  const Frame n_final = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, n_reference), owner);
  CHECK(n_final.status == Status::Ok);
  CHECK(n_final.command == key_n.command);
  CHECK(n_final.correlation == key_n.correlation);
  Composition composition;
  CHECK(DecodeComposition(n_final.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");

  const Frame n_duplicate = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, n_reference), owner);
  CHECK(n_duplicate.command == n_final.command);
  CHECK(n_duplicate.correlation == n_final.correlation);
  CHECK(n_duplicate.payload == n_final.payload);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, n_reference), owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult, n_reference), owner)
            .status == Status::StaleRequest);
  CHECK(RuntimeServiceTestAccess::ProcessCount(service, key_n.correlation) ==
        1);

  Frame commit = Session(Command::ProcessKey, 3);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>(' '), 0, 0, 1, 2},
                       &commit.payload));
  const DeliveryReference commit_reference{commit.command,
                                           commit.correlation};
  const size_t abandoned_before_commit =
      RuntimeServiceTestAccess::AbandonedSessionCount(service);
  CHECK(service.DispatchForDelivery(commit, owner).status ==
        Status::Prepared);

  // Neither the host nor a mutating control operation may destroy/replace the
  // context while this exact delivery is still outstanding.
  CHECK(service.DispatchForDelivery(Session(Command::CloseSession, 4), owner)
            .status == Status::Unavailable);
  CHECK(service.ExecuteControl(Command::ControlDeploy) ==
        ControlError::Runtime);
  CHECK(!service.SetOption("ascii_mode", true));

  CHECK(_putenv_s("FAMO_TEST_RUNTIME_COPY_FAILURE", "1") == 0);
  const Frame recovery_pending = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, commit_reference), owner);
  CHECK(recovery_pending.status == Status::RecoveryPending);
  CHECK(RuntimeServiceTestAccess::ProcessCount(service, commit.correlation) ==
        2);
  CHECK(_putenv_s("FAMO_TEST_RUNTIME_COPY_FAILURE", "") == 0);

  CHECK(_putenv_s(
            "FAMO_TEST_RUNTIME_DELIVERY_ENCODING_ALLOCATION_FAILURE", "1") ==
        0);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const Frame encoding_pending = service.DispatchForDelivery(
        DeliveryControl(Command::ClaimResult, commit_reference), owner);
    CHECK(encoding_pending.status == Status::RecoveryPending);
    CHECK(RuntimeServiceTestAccess::ProcessCount(service, commit.correlation) ==
          2);
    CHECK(RuntimeServiceTestAccess::DeliveryCount(service) == 1);
    CHECK(RuntimeServiceTestAccess::AbandonedSessionCount(service) ==
          abandoned_before_commit);
  }
  CHECK(_putenv_s(
            "FAMO_TEST_RUNTIME_DELIVERY_ENCODING_ALLOCATION_FAILURE", "") ==
        0);

  const Frame commit_final = service.DispatchForDelivery(
      DeliveryControl(Command::ClaimResult, commit_reference), owner);
  CHECK(commit_final.status == Status::Ok);
  CHECK(commit_final.command == commit.command);
  CHECK(commit_final.correlation == commit.correlation);
  CHECK(DecodeComposition(commit_final.payload, &composition, &error));
  CHECK(composition.handled && composition.commit == "n" &&
        composition.preedit.empty());
  CHECK(RuntimeServiceTestAccess::ProcessCount(service, commit.correlation) ==
        2);
  CHECK(RuntimeServiceTestAccess::AbandonedSessionCount(service) ==
        abandoned_before_commit);
  const Frame commit_duplicate = service.DispatchForDelivery(
      DeliveryControl(Command::ClaimResult, commit_reference), owner);
  CHECK(commit_duplicate.command == commit_final.command);
  CHECK(commit_duplicate.correlation == commit_final.correlation);
  CHECK(commit_duplicate.payload == commit_final.payload);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, commit_reference),
                    owner)
            .status == Status::Ok);

  Frame key_i = Session(Command::ProcessKey, 4);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('I'), 0, 0, 1, 2},
                       &key_i.payload));
  const DeliveryReference i_reference{key_i.command, key_i.correlation};
  CHECK(service.DispatchForDelivery(key_i, owner).status == Status::Prepared);
  const Frame i_final = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, i_reference), owner);
  CHECK(i_final.status == Status::Ok);
  CHECK(DecodeComposition(i_final.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "i");
  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_ACK_ALLOCATION_FAILURE", L"1"));
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, i_reference), owner)
            .status == Status::Unavailable);
  const Frame i_after_ack_oom = service.DispatchForDelivery(
      DeliveryControl(Command::ClaimResult, i_reference), owner);
  CHECK(i_after_ack_oom.status == Status::Ok);
  CHECK(i_after_ack_oom.payload == i_final.payload);
  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_ACK_ALLOCATION_FAILURE", nullptr));
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, i_reference), owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, i_reference), owner)
            .status == Status::Ok);

  const PipeClientIdentity intruder{
      owner.process_id, owner.process_creation_time + 1};
  CHECK(service.DispatchForDelivery(Connection(Command::Hello), intruder)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(open, intruder).status ==
        Status::StaleRequest);

  Frame cancelled_prepared = Session(Command::ProcessKey, 5);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('X'), 0, 0, 1, 3},
                       &cancelled_prepared.payload));
  const DeliveryReference cancelled_prepared_reference{
      cancelled_prepared.command, cancelled_prepared.correlation};
  CHECK(service.DispatchForDelivery(cancelled_prepared, owner).status ==
        Status::Prepared);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult,
                                    cancelled_prepared_reference),
                    intruder)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    cancelled_prepared_reference),
                    intruder)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    cancelled_prepared_reference),
                    owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::LastSequence(
            service, cancelled_prepared.correlation) == 5);
  CHECK(service.DispatchForDelivery(cancelled_prepared, owner).status ==
        Status::StaleRequest);

  Frame cancelled_missing = Session(Command::ProcessKey, 6);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('Y'), 0, 0, 1, 4},
                       &cancelled_missing.payload));
  const DeliveryReference cancelled_missing_reference{
      cancelled_missing.command, cancelled_missing.correlation};
  Frame wrong_connection =
      DeliveryControl(Command::AckResult, cancelled_missing_reference);
  wrong_connection.correlation.connection_generation = 999;
  CHECK(service.DispatchForDelivery(wrong_connection, owner).status ==
        Status::StaleRequest);
  DeliveryReference missing_session_reference =
      cancelled_missing_reference;
  missing_session_reference.correlation.session_id = 999;
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    missing_session_reference),
                    owner)
            .status == Status::StaleRequest);
  DeliveryReference old_sequence_reference{
      Command::ClearComposition, Session(Command::ClearComposition, 4)
                                     .correlation};
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    old_sequence_reference),
                    owner)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    cancelled_missing_reference),
                    intruder)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    cancelled_missing_reference),
                    owner)
            .status == Status::Ok);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult,
                                    cancelled_missing_reference),
                    owner)
            .status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::LastSequence(
            service, cancelled_missing.correlation) == 6);
  CHECK(service.DispatchForDelivery(cancelled_missing, owner).status ==
        Status::StaleRequest);

  Frame key_z = Session(Command::ProcessKey, 7);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('Z'), 0, 0, 1, 5},
                       &key_z.payload));
  const DeliveryReference z_reference{key_z.command, key_z.correlation};
  CHECK(service.DispatchForDelivery(key_z, owner).status == Status::Prepared);
  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_DELIVERY_ENCODING_ALLOCATION_FAILURE", L"1"));
  const Frame z_final = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, z_reference), owner);
  CHECK(z_final.status == Status::RecoveryPending);
  CHECK(SetEnvironmentVariableW(
      L"FAMO_TEST_RUNTIME_DELIVERY_ENCODING_ALLOCATION_FAILURE", nullptr));
  const Frame z_retried = service.DispatchForDelivery(
      DeliveryControl(Command::ClaimResult, z_reference), owner);
  CHECK(z_retried.status == Status::Ok);
  CHECK(DecodeComposition(z_retried.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "iz");
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, z_reference), owner)
            .status == Status::Ok);

  // Per-client tombstones are bounded. Even after the oldest exact ACK record
  // is evicted, the monotonically advanced session sequence permanently
  // rejects its late Prepare without mutating the engine.
  for (uint64_t sequence = 8; sequence < 32; ++sequence) {
    const DeliveryReference cancelled{
        Command::ClearComposition,
        Session(Command::ClearComposition, sequence).correlation};
    CHECK(service.DispatchForDelivery(
                      DeliveryControl(Command::AckResult, cancelled), owner)
              .status == Status::Ok);
  }
  CHECK(RuntimeServiceTestAccess::AcknowledgementCount(service, 101) <= 16);
  CHECK(RuntimeServiceTestAccess::LastSequence(
            service, Session(Command::ClearComposition, 31).correlation) ==
        31);
  Frame evicted_cancel = Session(Command::ClearComposition, 8);
  CHECK(service.DispatchForDelivery(evicted_cancel, owner).status ==
        Status::StaleRequest);

  Frame key_w = Session(Command::ProcessKey, 32);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('W'), 0, 0, 1, 6},
                       &key_w.payload));
  const DeliveryReference w_reference{key_w.command, key_w.correlation};
  CHECK(service.DispatchForDelivery(key_w, owner).status == Status::Prepared);
  const Frame w_final = service.DispatchForDelivery(
      DeliveryControl(Command::ExecutePrepared, w_reference), owner);
  CHECK(w_final.status == Status::Ok);
  CHECK(DecodeComposition(w_final.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "izw");
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, w_reference), owner)
            .status == Status::Ok);

  Frame sibling_open;
  sibling_open.command = Command::OpenSession;
  sibling_open.correlation = {101, 102, 103, 204, 205, 1};
  CHECK(EncodeOpenSession("test", &sibling_open.payload, &error));
  CHECK(service.DispatchForDelivery(sibling_open, owner).status == Status::Ok);

  Frame terminal_key = Session(Command::ProcessKey, 33);
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('Q'), 0, 0, 1, 7},
                       &terminal_key.payload));
  const DeliveryReference terminal_reference{terminal_key.command,
                                             terminal_key.correlation};
  Frame sibling_key;
  sibling_key.command = Command::ProcessKey;
  sibling_key.correlation = {101, 102, 103, 204, 205, 2};
  CHECK(EncodeKeyEvent({static_cast<uint32_t>('N'), 0, 0, 1, 8},
                       &sibling_key.payload));
  const DeliveryReference sibling_reference{sibling_key.command,
                                             sibling_key.correlation};
  CHECK(service.DispatchForDelivery(terminal_key, owner).status ==
        Status::Prepared);
  CHECK(service.DispatchForDelivery(sibling_key, owner).status ==
        Status::Prepared);

  // Both logical sessions have already mutated inside the same authenticated
  // connection epoch and owe their exact recovered snapshots.
  CHECK(SetEnvironmentVariableW(L"FAMO_TEST_RUNTIME_COPY_FAILURE", L"1"));
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ExecutePrepared,
                                    terminal_reference),
                    owner)
            .status == Status::RecoveryPending);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ExecutePrepared,
                                    sibling_reference),
                    owner)
            .status == Status::RecoveryPending);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult,
                                    terminal_reference),
                    owner)
            .status == Status::RecoveryPending);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult,
                                    terminal_reference),
                    owner)
            .status == Status::DeliveryFailed);
  CHECK(SetEnvironmentVariableW(L"FAMO_TEST_RUNTIME_COPY_FAILURE", nullptr));

  DeliveryReference wrong_delivery = terminal_reference;
  ++wrong_delivery.correlation.sequence;
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AbandonSession, wrong_delivery),
                    owner)
            .status == Status::StaleRequest);
  CHECK(RuntimeServiceTestAccess::DeliveryCount(service) == 2);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 2);

  const Frame abandon_session =
      DeliveryControl(Command::AbandonSession, terminal_reference);
  CHECK(service.DispatchForDelivery(abandon_session, intruder).status ==
        Status::StaleRequest);
  CHECK(service.DispatchForDelivery(abandon_session, owner).status ==
        Status::Ok);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AbandonSession, wrong_delivery),
                    owner)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(abandon_session, owner).status ==
        Status::Ok);
  CHECK(RuntimeServiceTestAccess::DeliveryCount(service) == 1);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 1);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult,
                                    terminal_reference),
                    owner)
            .status == Status::StaleRequest);
  CHECK(service.DispatchForDelivery(open, owner).status ==
        Status::StaleRequest);
  CHECK(service.DispatchForDelivery(Connection(Command::Hello), owner).status ==
        Status::Ok);

  const Frame sibling_final = service.DispatchForDelivery(
      DeliveryControl(Command::ClaimResult, sibling_reference), owner);
  CHECK(sibling_final.status == Status::Ok);
  CHECK(DecodeComposition(sibling_final.payload, &composition, &error));
  CHECK(composition.handled && composition.preedit == "n");
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::AckResult, sibling_reference),
                    owner)
            .status == Status::Ok);
  Frame sibling_close;
  sibling_close.command = Command::CloseSession;
  sibling_close.correlation = {101, 102, 103, 204, 205, 3};
  CHECK(service.DispatchForDelivery(sibling_close, owner).status ==
        Status::Ok);

  const Frame abandon = Connection(Command::AbandonConnection);
  CHECK(service.DispatchForDelivery(abandon, owner).status == Status::Ok);
  CHECK(RuntimeServiceTestAccess::DeliveryCount(service) == 0);
  CHECK(RuntimeServiceTestAccess::SessionCount(service) == 0);
  CHECK(service.DispatchForDelivery(Connection(Command::Hello), owner).status ==
        Status::StaleRequest);
  Frame next_epoch = Connection(Command::Hello);
  ++next_epoch.correlation.activation_generation;
  ++next_epoch.correlation.connection_generation;
  CHECK(service.DispatchForDelivery(next_epoch, owner).status == Status::Ok);

  service.Stop();
  CHECK(service.Start(L"FamoTestEngine.dll", "", &error));
  CHECK(service.InitializeControlState() == ControlError::None);
  CHECK(service.DispatchForDelivery(Connection(Command::Hello), owner).status ==
        Status::Ok);
  CHECK(service.DispatchForDelivery(
                    DeliveryControl(Command::ClaimResult, i_reference), owner)
            .status == Status::StaleRequest);
  service.Stop();
  std::printf("delivery_selfcheck: OK\n");
  return 0;
}
