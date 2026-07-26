#pragma once

// One group of declarations submitted through one outermost transaction, the
// way a scope builder, a module callback, or a registration callback submits
// them. Every declaration of the group joins the active outer transaction
// instead of opening one of its own, so the group shares one entry capture, one
// canonical descriptor plan, one validation symbol view, and one publication
// decision.
//
// The report is what the group observed: how much it planned and prepared, what
// the first deterministic diagnostic was, whether a nested failure poisoned the
// attempt, and what an ordinary virtual-machine or reflection query saw while
// the transaction was still open.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include "state/transaction/installation.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct JoinedFunctionDeclaration final {
  std::string Name;
  ErasedCallableDescriptor Callable;

  JoinedFunctionDeclaration(std::string NameValue,
                            ErasedCallableDescriptor CallableValue)
      : Name(std::move(NameValue)), Callable(std::move(CallableValue)) {}

  JoinedFunctionDeclaration(const JoinedFunctionDeclaration &) = delete;
  JoinedFunctionDeclaration &
  operator=(const JoinedFunctionDeclaration &) = delete;
  JoinedFunctionDeclaration(JoinedFunctionDeclaration &&) noexcept = default;
  JoinedFunctionDeclaration &
  operator=(JoinedFunctionDeclaration &&) noexcept = default;
  ~JoinedFunctionDeclaration() = default;
};

struct JoinedSubmissionReport final {
  // How many declarations joined the outer transaction, how many of them landed
  // in its canonical plan, and how many staged their protected resources.
  std::size_t Submitted = 0;
  std::size_t Planned = 0;
  std::size_t Prepared = 0;
  std::size_t JoinedSubmissions = 0;

  // Nested results the callback may have ignored. A non-zero count poisons the
  // attempt, so the outer transaction can no longer publish.
  std::size_t NestedFailures = 0;
  bool OuterCouldPublish = false;
  TransactionStatus Status = TransactionStatus::Open;
  std::optional<ErrorDiagnostic> Failure;

  // Preparation of the replacement immutable stores.
  PreparationStatus Preparation = PreparationStatus::Prepared;
  std::uint64_t CandidateGeneration = 0;
  std::size_t CandidateSymbols = 0;

  // What validation saw inside the transaction.
  std::size_t CommittedSymbolsInView = 0;
  std::size_t PendingSymbolsInView = 0;

  // What an ordinary query saw while the transaction was still open. Pending
  // data must be invisible to all of them.
  std::uint64_t PublishedGenerationWhileOpen = 0;
  std::size_t PublishedSymbolsWhileOpen = 0;
  std::uint64_t ReflectionGenerationWhileOpen = 0;
  std::size_t StagedBindingsWhileOpen = 0;
  std::size_t VmVisibleDeclarationsWhileOpen = 0;
  int StackDepthWhileOpen = 0;
  int EntryStackDepth = 0;

  // What the installation and publication phases did, when the group ran them.
  PublicationObservation Publication;

  // What an ordinary query sees once the group has finished, whether it
  // published everything or restored everything.
  std::uint64_t PublishedGenerationAfter = 0;
  std::size_t PublishedSymbolsAfter = 0;
  std::size_t StagedBindingsAfter = 0;
  std::size_t CommittedBindingsAfter = 0;
  std::size_t VmVisibleDeclarationsAfter = 0;
};

// What one group observed when it ran behind the private callback boundary: a
// builder, module, or registration callback that submits several declarations
// and may throw partway through instead of returning a result.
//
// Nothing a callback throws may cross the boundary, so the observation also
// records what every ordinary query - the committed generation set, an owning
// public reflection snapshot taken on this thread and on another one, the
// canonical virtual-machine paths, and dispatch - saw while the attempt was
// still in flight.
struct CallbackBoundaryObservation final {
  std::size_t Submitted = 0;

  // The callback threw, and the boundary contained it instead of letting it
  // escape into the caller.
  bool CallbackThrew = false;
  bool ExceptionContained = false;
  std::string ExceptionKind;

  std::size_t PlannedWhileOpen = 0;
  std::size_t PendingSymbolsInView = 0;
  std::size_t NestedFailures = 0;
  bool CouldPublishWhileOpen = false;

  // Ordinary queries taken while the outermost transaction was still open.
  std::uint64_t GenerationWhileOpen = 0;
  std::size_t GenerationSymbolsWhileOpen = 0;
  std::uint64_t SnapshotGenerationWhileOpen = 0;
  std::size_t SnapshotSymbolsWhileOpen = 0;
  std::uint64_t ForeignSnapshotGenerationWhileOpen = 0;
  std::size_t ForeignSnapshotSymbolsWhileOpen = 0;
  std::vector<std::string> VmPathKindsWhileOpen;
  std::size_t DispatchVisibleWhileOpen = 0;
  // Staged, still uncommitted records, and committed ones. A staged record is
  // never counted as committed, so pending work is visible only here.
  std::size_t StagedWhileOpen = 0;
  std::size_t CommittedWhileOpen = 0;
  int EntryStackDepth = 0;
  int StackDepthWhileOpen = 0;

  // The snapshot captured while the transaction was open. It is retained here
  // so a test can prove it never changes, whatever the attempt then does.
  ReflectionSnapshot SnapshotWhileOpen;

  // What the attempt did once the callback returned or was contained.
  bool Published = false;
  TransactionStatus Status = TransactionStatus::Open;
  std::optional<ErrorDiagnostic> Failure;
  std::size_t JournalledEntries = 0;
  std::size_t InstalledPaths = 0;
  bool RestoredEveryEntry = false;
  bool RestoredEntryStackDepth = false;

  std::uint64_t GenerationAfter = 0;
  std::size_t GenerationSymbolsAfter = 0;
  std::size_t StagedAfter = 0;
  std::size_t CommittedAfter = 0;
  std::size_t DispatchVisibleAfter = 0;
  std::vector<std::string> VmPathKindsAfter;
  int StackDepthAfter = 0;
};

} // namespace Luna::Detail
