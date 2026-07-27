#pragma once

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
  std::size_t Submitted = 0;
  std::size_t Planned = 0;
  std::size_t Prepared = 0;
  std::size_t JoinedSubmissions = 0;

  std::size_t NestedFailures = 0;
  bool OuterCouldPublish = false;
  TransactionStatus Status = TransactionStatus::Open;
  std::optional<ErrorDiagnostic> Failure;

  PreparationStatus Preparation = PreparationStatus::Prepared;
  std::uint64_t CandidateGeneration = 0;
  std::size_t CandidateSymbols = 0;

  std::size_t CommittedSymbolsInView = 0;
  std::size_t PendingSymbolsInView = 0;

  std::uint64_t PublishedGenerationWhileOpen = 0;
  std::size_t PublishedSymbolsWhileOpen = 0;
  std::uint64_t ReflectionGenerationWhileOpen = 0;
  std::size_t StagedBindingsWhileOpen = 0;
  std::size_t VmVisibleDeclarationsWhileOpen = 0;
  int StackDepthWhileOpen = 0;
  int EntryStackDepth = 0;

  PublicationObservation Publication;

  std::uint64_t PublishedGenerationAfter = 0;
  std::size_t PublishedSymbolsAfter = 0;
  std::size_t StagedBindingsAfter = 0;
  std::size_t CommittedBindingsAfter = 0;
  std::size_t VmVisibleDeclarationsAfter = 0;
};

struct CallbackBoundaryObservation final {
  std::size_t Submitted = 0;

  bool CallbackThrew = false;
  bool ExceptionContained = false;
  std::string ExceptionKind;

  std::size_t PlannedWhileOpen = 0;
  std::size_t PendingSymbolsInView = 0;
  std::size_t NestedFailures = 0;
  bool CouldPublishWhileOpen = false;

  std::uint64_t GenerationWhileOpen = 0;
  std::size_t GenerationSymbolsWhileOpen = 0;
  std::uint64_t SnapshotGenerationWhileOpen = 0;
  std::size_t SnapshotSymbolsWhileOpen = 0;
  std::uint64_t ForeignSnapshotGenerationWhileOpen = 0;
  std::size_t ForeignSnapshotSymbolsWhileOpen = 0;
  std::vector<std::string> VmPathKindsWhileOpen;
  std::size_t DispatchVisibleWhileOpen = 0;
  std::size_t StagedWhileOpen = 0;
  std::size_t CommittedWhileOpen = 0;
  int EntryStackDepth = 0;
  int StackDepthWhileOpen = 0;

  ReflectionSnapshot SnapshotWhileOpen;

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
