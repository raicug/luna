#pragma once

// Resolving one call against one overload set.
//
// Dispatch reads the candidates of one binding record in their canonical order
// - encoded signature first, candidate identity last, never registration order
// - and resolves the received arguments against the type generation the
// invocation captured at entry:
//
//   1. Only committed candidates participate; a candidate an open transaction
//      staged is invisible here.
//   2. Candidates whose arity window cannot accept the received count are
//      rejected before any value is inspected.
//   3. Every supplied argument is probed side-effect-free, and only the first
//      deterministic rejection of a candidate is recorded.
//   4. Viable candidates are compared by Pareto dominance over their rank
//      sequences, with the signature shape as one further dimension.
//
// Nothing here converts, constructs, invokes, or mutates. When no candidate is
// viable the diagnostic names the qualified name, the received arguments, and
// every available signature with its first rejection; when several candidates
// are non-dominated the diagnostic lists the ambiguous frontier in canonical
// order.

// clang-format off
#include <luna/binding/instance_receiver.hpp>

#include "state/invocation/overload/resolution.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;

struct OverloadDispatchResult final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;

  // Index of the selected candidate inside the record, valid only on selection.
  std::size_t SelectedCandidate = 0;

  // The one deterministic diagnostic of a refused call. Empty on selection.
  std::string Diagnostic;

  // How many candidates participated and how many were viable. Private
  // diagnostics and tests read them; no public API can reach them.
  std::size_t Considered = 0;
  std::size_t Viable = 0;

  [[nodiscard]] bool HasSelection() const noexcept {
    return Status == OverloadSelectionStatus::Selected;
  }
};

// Resolves the call currently on `State` against the committed candidates of
// `Record`.
//
// `Receiver` is the already validated object of one instance-member set, or
// null for every ordinary callable. Supplying one makes the receiver rank
// position zero of every candidate's rank sequence and shifts the ordinary
// arguments to the positions after it, so an instance member is ranked by
// exactly the same Pareto rules with one more leading dimension. A candidate
// that would mutate a const receiver is refused there, before any of its
// ordinary arguments is even probed.
[[nodiscard]] OverloadDispatchResult
ResolveOverloadedCall(const BindingRecord &Record, lua_State *State,
                      const TypeGeneration &Types,
                      const InstanceReceiver *Receiver = nullptr);

} // namespace Luna::Detail
