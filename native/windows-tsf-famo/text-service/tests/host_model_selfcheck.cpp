#include <cstdio>

#include "famo_tsf_host_model.h"
#include "famo_utf_conversion.h"

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #value, __FILE__,   \
                   __LINE__);                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

using famo::runtime::Candidate;
using famo::runtime::Composition;
using famo::runtime::Correlation;
using famo::tsf::ContextPhase;
using famo::tsf::ContextState;
using famo::tsf::HostKey;
using famo::tsf::RequestKind;

namespace {

Correlation Identity(uint64_t session) {
  return {10, 20, 30, session, 40 + session, 0};
}

HostKey Down(uint32_t key) { return {key, 1, 0, true, 100}; }

Composition Preedit(std::string text, size_t candidates = 0) {
  Composition value;
  value.handled = true;
  value.preedit = std::move(text);
  for (size_t index = 0; index < candidates; ++index)
    value.candidates.push_back(Candidate{std::to_string(index), "", "", 0, 0});
  return value;
}

bool PureTestAndExactlyOnce() {
  ContextState state;
  state.Open(Identity(50));
  const HostKey n = Down('N');
  CHECK(state.TestKey(n));
  CHECK(state.TestKey(n));
  CHECK(state.pending_sequence() == 0);

  const auto plan = state.PlanKey(n);
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.correlation.sequence == 1);
  CHECK(state.PlanKey(n).request == RequestKind::None);
  CHECK(state.AcceptReply(plan.correlation));
  state.ApplySucceeded(Preedit("n"));
  const auto ui = state.PlanUiState();
  CHECK(ui && ui->sequence == 2);

  HostKey up = n;
  up.is_key_down = false;
  CHECK(!state.TestKey(up));
  CHECK(state.PlanKey(up).request == RequestKind::None);
  CHECK(state.PlanKey(Down(0xffc9)).request == RequestKind::None);
  return true;
}

bool DigitCanStartSchemaInput() {
  ContextState state;
  state.Open(Identity(55));
  CHECK(state.TestKey(Down('7')));
  const auto plan = state.PlanKey(Down('7'));
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.key.virtual_key == '7');
  state.CompleteUnhandled();
  return true;
}

bool SecurityUiStateCanPassPendingComposition() {
  ContextState state;
  state.Open(Identity(57));
  const auto pending = state.PlanKey(Down('N'));
  CHECK(pending.sends_request());
  CHECK(!state.PlanUiState());
  const auto hidden = state.PlanSecurityUiState();
  CHECK(hidden && hidden->sequence == pending.correlation.sequence + 1);
  CHECK(state.AcceptReply(pending.correlation));
  state.ApplySucceeded(Preedit("n"));
  return true;
}

bool CandidateAndCorrelation() {
  ContextState state;
  state.Open(Identity(60));
  auto plan = state.PlanKey(Down('N'));
  state.ApplySucceeded(Preedit("ni", 3));
  CHECK(!state.PlanAbsoluteCandidate(plan.correlation.sequence - 1));
  const auto absolute =
      state.PlanAbsoluteCandidate(plan.correlation.sequence);
  CHECK(absolute && absolute->sequence == plan.correlation.sequence + 1);
  state.CompleteUnhandled();
  plan = state.PlanKey(Down('2'));
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.key.virtual_key == '2');
  state.CompleteUnhandled();
  plan = state.PlanKey(Down('0'));
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.key.virtual_key == '0');
  state.CompleteUnhandled();
  plan = state.PlanKey(Down('9'));
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.key.virtual_key == '9');
  state.CompleteUnhandled();
  // A schema may use punctuation (for example ';') as a select key. The host
  // must forward the physical key and let librime interpret the real
  // candidate labels/select_keys instead of fabricating a numeric index.
  plan = state.PlanKey(Down(';'));
  CHECK(plan.request == RequestKind::ProcessKey);
  CHECK(plan.key.virtual_key == ';');
  Correlation stale = plan.correlation;
  --stale.connection_generation;
  CHECK(!state.AcceptReply(stale));
  CHECK(state.AcceptReply(plan.correlation));
  Composition committed;
  committed.handled = true;
  committed.commit = "second";
  state.ApplySucceeded(committed);
  CHECK(state.displayed().commit == "second");
  return true;
}

bool FailureLatchesRecovery() {
  ContextState state;
  state.Open(Identity(70));
  auto plan = state.PlanKey(Down('N'));
  CHECK(state.AcceptReply(plan.correlation));
  state.ApplySucceeded(Preedit("confirmed"));
  CHECK(state.PlanKey(Down('I')).sends_request());

  const auto first = state.Fail();
  CHECK(first.commit_preedit == "confirmed");
  CHECK(state.phase() == ContextPhase::Poisoned);
  CHECK(!state.TestKey(Down('X')));
  CHECK(!state.PlanKey(Down('X')).sends_request());

  const auto second = state.Fail();
  CHECK(!second.commit_preedit);
  return true;
}

bool ContextsAndGenerationAreIndependent() {
  ContextState first;
  ContextState second;
  first.Open(Identity(80));
  second.Open(Identity(81));
  const auto a = first.PlanKey(Down('A'));
  const auto b = second.PlanKey(Down('B'));
  CHECK(a.correlation.session_id != b.correlation.session_id);
  CHECK(first.AcceptReply(a.correlation));
  CHECK(second.AcceptReply(b.correlation));
  first.Fail();
  CHECK(first.phase() == ContextPhase::Poisoned);
  CHECK(second.phase() == ContextPhase::Ready);

  Correlation newer = Identity(80);
  ++newer.connection_generation;
  ++newer.session_generation;
  first.Open(newer);
  CHECK(first.phase() == ContextPhase::Ready);
  CHECK(!first.AcceptReply(a.correlation));
  CHECK(first.PlanKey(Down('C')).correlation.connection_generation ==
        newer.connection_generation);
  return true;
}

bool TransientUnavailableReleasesPendingKey() {
  ContextState state;
  const Correlation identity = Identity(90);
  state.Open(identity);
  const auto busy = state.PlanKey(Down('N'));
  CHECK(busy.sends_request());
  CHECK(state.pending_sequence() == busy.correlation.sequence);

  state.CompleteUnhandled();
  CHECK(state.phase() == ContextPhase::Ready);
  CHECK(state.pending_sequence() == 0);

  const auto retry = state.PlanKey(Down('I'));
  CHECK(retry.sends_request());
  CHECK(retry.correlation.client_id == identity.client_id);
  CHECK(retry.correlation.activation_generation ==
        identity.activation_generation);
  CHECK(retry.correlation.connection_generation ==
        identity.connection_generation);
  CHECK(retry.correlation.session_id == identity.session_id);
  CHECK(retry.correlation.session_generation == identity.session_generation);
  CHECK(retry.correlation.sequence == busy.correlation.sequence + 1);
  return true;
}

bool UtfConversionIsStrict() {
  std::wstring output;
  CHECK(famo::tsf::Utf8ToUtf16("\xe4\xbd\xa0\xe5\xa5\xbd", &output));
  CHECK(output == L"\u4f60\u597d");
  CHECK(!famo::tsf::Utf8ToUtf16("\xff", &output));

  famo::tsf::Utf16Preedit preedit;
  CHECK(famo::tsf::Utf8PreeditToUtf16("abcd", 0, 0, 2, &preedit));
  CHECK(preedit.selection_start == 0 && preedit.selection_end == 0 &&
        preedit.cursor == 2);
  CHECK(famo::tsf::Utf8PreeditToUtf16("abcdef", 1, 4, 4, &preedit));
  CHECK(preedit.selection_start == 1 && preedit.selection_end == 4 &&
        preedit.cursor == 4);
  CHECK(famo::tsf::Utf8PreeditToUtf16("\xe4\xbd\xa0" "A", 0, 0, 4,
                                      &preedit));
  CHECK(preedit.text == L"\u4f60" L"A" && preedit.cursor == 2);
  CHECK(famo::tsf::Utf8PreeditToUtf16("\xf0\x9f\x98\x80" "A", 0, 0, 4,
                                      &preedit));
  CHECK(preedit.text == L"\xd83d\xde00" L"A" && preedit.cursor == 2);
  CHECK(!famo::tsf::Utf8PreeditToUtf16("\xe4\xbd\xa0", 0, 0, 1,
                                       &preedit));
  return true;
}

} // namespace

int main() {
  if (!PureTestAndExactlyOnce() || !DigitCanStartSchemaInput() ||
      !SecurityUiStateCanPassPendingComposition() ||
      !CandidateAndCorrelation() ||
      !FailureLatchesRecovery() || !ContextsAndGenerationAreIndependent() ||
      !TransientUnavailableReleasesPendingKey() || !UtfConversionIsStrict())
    return 1;
  std::printf("host_model_selfcheck: OK\n");
  return 0;
}
