# Windows Search candidate contract

Bridge ABI 5 exposes `ITfFnSearchCandidateProvider` for Windows Search. The
provider is deliberately a stateless conversion lane, separate from ordinary
TSF composition sessions.

## First production slice

- The query is a non-empty Rime reading made of ASCII letters, digits, or an
  apostrophe. ASCII letters are case-folded. Unsupported input returns an empty
  list instead of creating an IME session.
- Runtime creates one ephemeral engine context, feeds the reading through the
  normal engine action API, and returns only the engine's current composition
  candidates. Famo does not synthesize or append prediction candidates.
- Results exclude the raw reading, exact duplicates, and a candidate whose
  text redundantly extends another returned candidate. At most 20 candidates
  cross protocol v4.
- The application ID is accepted but does not alter ranking in this slice.
  `ITfFnSearchCandidateProvider::SetResult` does not train engine state.
- A query publishes no Runtime snapshot, candidate HWND, IME message, event,
  logical session, or user-data mutation. Candidate-list COM objects own an
  immutable copy and remain valid after the text service deactivates.

This narrower slice intentionally does not promise non-ASCII readings,
application-specific history, learning from Search result selection, or
prediction classification that the engine ABI does not expose. Expanding any
of those requires a separately versioned contract and privacy review.

## Acceptance boundary

Source tests cover discovery, protocol versioning, engine-backed enumeration,
ownership/lifetime, filtering, and session/UI isolation. Release acceptance
still requires the physical Windows Search host to show host-owned candidates,
complete the controlled phrase, keep the Famo self-drawn window hidden, and
pass selection, paging, cancel, focus-switch, and repeated-run checks.
