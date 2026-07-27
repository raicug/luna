#pragma once

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
  explicit RegistrationTransaction(TransactionCapture Capture);

  explicit RegistrationTransaction(
      std::shared_ptr<const GenerationSet> Captured);

  RegistrationTransaction(const RegistrationTransaction &) = delete;
  RegistrationTransaction &operator=(const RegistrationTransaction &) = delete;
  RegistrationTransaction(RegistrationTransaction &&) = delete;
  RegistrationTransaction &operator=(RegistrationTransaction &&) = delete;
  ~RegistrationTransaction() = default;

  [[nodiscard]] const GenerationSet &Captured() const noexcept {
    return *CapturedGenerations;
  }

  [[nodiscard]] const std::shared_ptr<const GenerationSet> &
  SharedCaptured() const noexcept {
    return CapturedGenerations;
  }

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

  std::size_t Append(DescriptorPlanEntry Entry);

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

  void Poison(ErrorDiagnostic Diagnostic);

  [[nodiscard]] const std::optional<ErrorDiagnostic> &Failure() const noexcept {
    return FailureValue;
  }

  void RecordNestedFailure(ErrorDiagnostic Diagnostic);

  [[nodiscard]] std::size_t NestedFailureCount() const noexcept {
    return NestedFailures;
  }

  [[nodiscard]] bool HasNestedFailure() const noexcept {
    return NestedFailures != 0;
  }

  [[nodiscard]] std::size_t NestedDepth() const noexcept {
    return NestedDepthValue;
  }

  [[nodiscard]] std::size_t JoinedSubmissionCount() const noexcept {
    return JoinedSubmissions;
  }

  [[nodiscard]] bool CanPublish() const noexcept {
    return IsOpen() && NestedDepthValue == 0;
  }

  void MarkCommitted() noexcept;

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

class ActiveTransactionScope final {
public:
  ActiveTransactionScope(RegistrationTransaction *&ActiveSlot,
                         RegistrationTransaction &Candidate) noexcept;

  ActiveTransactionScope(const ActiveTransactionScope &) = delete;
  ActiveTransactionScope &operator=(const ActiveTransactionScope &) = delete;
  ActiveTransactionScope(ActiveTransactionScope &&) = delete;
  ActiveTransactionScope &operator=(ActiveTransactionScope &&) = delete;

  ~ActiveTransactionScope() noexcept;

  [[nodiscard]] bool IsOuter() const noexcept { return Outer; }

  [[nodiscard]] RegistrationTransaction &Active() const noexcept {
    return *ActiveTransaction;
  }

  void ReportFailure(const ErrorDiagnostic &Diagnostic) const;

private:
  RegistrationTransaction **Slot;
  RegistrationTransaction *ActiveTransaction;
  bool Outer;
};

} // namespace Luna::Detail
