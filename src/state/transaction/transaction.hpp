#pragma once

// One registration transaction. A transaction is the pending half of
// registration: it captures the generation set it validates against, owns the
// canonical descriptor plan every category appends to, and carries the first
// deterministic diagnostic of the attempt.
//
// A State has at most one active outer transaction. `ActiveTransactionScope`
// publishes it for the duration of one submission, so a nested builder, module,
// or registration callback joins the outer transaction instead of committing
// independently. A nested failure is recorded on the outer transaction, so an
// ignored nested result still poisons the attempt and the outer transaction can
// no longer publish. Protected installation, undo journaling, and publication
// are separate concerns built on this structure.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>

#include "state/registration/plan.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
// clang-format on

namespace Luna::Detail {

enum class TransactionStatus { Open, Poisoned, Committed, RolledBack };

class RegistrationTransaction final {
public:
  // Entry capture of one attempt: owner thread, readiness and freeze phase,
  // entry stack depth, State identity with its epochs, and the generation set
  // every decision of the attempt reads.
  explicit RegistrationTransaction(TransactionCapture Capture);

  explicit RegistrationTransaction(
      std::shared_ptr<const GenerationSet> Captured);

  RegistrationTransaction(const RegistrationTransaction &) = delete;
  RegistrationTransaction &operator=(const RegistrationTransaction &) = delete;
  RegistrationTransaction(RegistrationTransaction &&) = delete;
  RegistrationTransaction &operator=(RegistrationTransaction &&) = delete;
  ~RegistrationTransaction() = default;

  // The generation set captured at entry. Every validation and publication
  // decision of this transaction reads this generation set, never a later one.
  [[nodiscard]] const GenerationSet &Captured() const noexcept {
    return *CapturedGenerations;
  }

  [[nodiscard]] const std::shared_ptr<const GenerationSet> &
  SharedCaptured() const noexcept {
    return CapturedGenerations;
  }

  // The complete entry capture of the attempt.
  [[nodiscard]] const TransactionCapture &Entry() const noexcept {
    return CaptureValue;
  }

  [[nodiscard]] int EntryStackDepth() const noexcept {
    return CaptureValue.EntryStackDepth;
  }

  [[nodiscard]] DescriptorPlan &Plan() noexcept { return PlannedEntries; }
  [[nodiscard]] const DescriptorPlan &Plan() const noexcept {
    return PlannedEntries;
  }

  // Appends one planned declaration and returns its plan index.
  std::size_t Append(DescriptorPlanEntry Entry);

  // Committed plus pending symbols, read through one canonical schema.
  [[nodiscard]] SymbolView Symbols() const noexcept;

  [[nodiscard]] TransactionStatus Status() const noexcept {
    return StatusValue;
  }

  [[nodiscard]] bool IsOpen() const noexcept {
    return StatusValue == TransactionStatus::Open;
  }

  [[nodiscard]] bool IsPoisoned() const noexcept {
    return StatusValue == TransactionStatus::Poisoned;
  }

  // Records the first deterministic diagnostic of the attempt. A later
  // diagnostic never replaces it, and an already poisoned transaction can no
  // longer commit, which is how an ignored nested failure poisons the outer
  // transaction.
  void Poison(ErrorDiagnostic Diagnostic);

  [[nodiscard]] const std::optional<ErrorDiagnostic> &Failure() const noexcept {
    return FailureValue;
  }

  // A nested submission failed. Because a callback may ignore the nested
  // result, the failure is recorded on the outer transaction: the attempt keeps
  // the first deterministic diagnostic and can no longer publish.
  void RecordNestedFailure(ErrorDiagnostic Diagnostic);

  [[nodiscard]] std::size_t NestedFailureCount() const noexcept {
    return NestedFailures;
  }

  [[nodiscard]] bool HasNestedFailure() const noexcept {
    return NestedFailures != 0;
  }

  // Number of joined submissions currently inside this transaction, and the
  // total number that ever joined it.
  [[nodiscard]] std::size_t NestedDepth() const noexcept {
    return NestedDepthValue;
  }

  [[nodiscard]] std::size_t JoinedSubmissionCount() const noexcept {
    return JoinedSubmissions;
  }

  // Only the outermost submission of an open, unpoisoned transaction may
  // publish, and only once every joined submission has returned.
  [[nodiscard]] bool CanPublish() const noexcept {
    return IsOpen() && NestedDepthValue == 0;
  }

  // Publication succeeded: the transaction observed a coherent generation set.
  void MarkCommitted() noexcept;

  // Every pending effect was discarded and the committed model is unchanged.
  void MarkRolledBack() noexcept;

private:
  friend class ActiveTransactionScope;

  void EnterNested() noexcept;
  void LeaveNested() noexcept;

  TransactionCapture CaptureValue;
  std::shared_ptr<const GenerationSet> CapturedGenerations;
  DescriptorPlan PlannedEntries;
  TransactionStatus StatusValue = TransactionStatus::Open;
  std::optional<ErrorDiagnostic> FailureValue;
  std::size_t NestedDepthValue = 0;
  std::size_t JoinedSubmissions = 0;
  std::size_t NestedFailures = 0;
};

// Publishes one transaction as the active outer transaction of a State for the
// duration of a scope. When a transaction is already active, the scope joins
// it, leaves the outer transaction in place, and counts itself as a nested
// submission so the outer transaction cannot publish while it is running.
class ActiveTransactionScope final {
public:
  ActiveTransactionScope(RegistrationTransaction *&ActiveSlot,
                         RegistrationTransaction &Candidate) noexcept;

  ActiveTransactionScope(const ActiveTransactionScope &) = delete;
  ActiveTransactionScope &operator=(const ActiveTransactionScope &) = delete;
  ActiveTransactionScope(ActiveTransactionScope &&) = delete;
  ActiveTransactionScope &operator=(ActiveTransactionScope &&) = delete;

  ~ActiveTransactionScope() noexcept;

  // True when this scope opened the outer transaction. Only the outer scope may
  // publish or roll back.
  [[nodiscard]] bool IsOuter() const noexcept { return Outer; }

  // The transaction every declaration of this scope appends to.
  [[nodiscard]] RegistrationTransaction &Active() const noexcept {
    return *ActiveTransaction;
  }

  // Reports one failure of this submission. An outer submission keeps the first
  // deterministic diagnostic of the attempt; a nested submission additionally
  // marks the attempt as having failed nested work, so an ignored nested result
  // still prevents publication.
  void ReportFailure(const ErrorDiagnostic &Diagnostic) const;

private:
  RegistrationTransaction **Slot;
  RegistrationTransaction *ActiveTransaction;
  bool Outer;
};

} // namespace Luna::Detail
