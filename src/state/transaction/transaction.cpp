// clang-format off
#include "state/transaction/transaction.hpp"

#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/registration/plan.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"

#include <cstddef>
#include <memory>
#include <utility>
// clang-format on

namespace Luna::Detail {

RegistrationTransaction::RegistrationTransaction(TransactionCapture Capture)
    : CaptureValue(std::move(Capture)),
      CapturedGenerations(CaptureValue.SharedGenerations()) {
  CaptureValue.Generations = CapturedGenerations;
}

RegistrationTransaction::RegistrationTransaction(
    std::shared_ptr<const GenerationSet> Captured)
    : CapturedGenerations(Captured ? std::move(Captured)
                                   : GenerationSet::Initial()) {
  CaptureValue.Generations = CapturedGenerations;
}

std::size_t RegistrationTransaction::Append(DescriptorPlanEntry Entry) {
  return PlannedEntries.Append(std::move(Entry));
}

SymbolView RegistrationTransaction::Symbols() const noexcept {
  return SymbolView(CapturedGenerations->Symbols(), PlannedEntries);
}

void RegistrationTransaction::Poison(ErrorDiagnostic Diagnostic) {
  if (!FailureValue)
    FailureValue = std::move(Diagnostic);
  if (StatusValue == TransactionStatus::Open)
    StatusValue = TransactionStatus::Poisoned;
}

void RegistrationTransaction::RecordNestedFailure(ErrorDiagnostic Diagnostic) {
  ++NestedFailures;
  Poison(std::move(Diagnostic));
}

void RegistrationTransaction::MarkCommitted() noexcept {
  if (StatusValue == TransactionStatus::Open)
    StatusValue = TransactionStatus::Committed;
}

void RegistrationTransaction::MarkRolledBack() noexcept {
  if (StatusValue == TransactionStatus::Open ||
      StatusValue == TransactionStatus::Poisoned)
    StatusValue = TransactionStatus::RolledBack;
}

void RegistrationTransaction::EnterNested() noexcept {
  ++NestedDepthValue;
  ++JoinedSubmissions;
}

void RegistrationTransaction::LeaveNested() noexcept {
  if (NestedDepthValue != 0)
    --NestedDepthValue;
}

ActiveTransactionScope::ActiveTransactionScope(
    RegistrationTransaction *&ActiveSlot,
    RegistrationTransaction &Candidate) noexcept
    : Slot(&ActiveSlot),
      ActiveTransaction(ActiveSlot ? ActiveSlot : &Candidate),
      Outer(ActiveSlot == nullptr) {
  if (Outer)
    ActiveSlot = &Candidate;
  else
    ActiveTransaction->EnterNested();
}

ActiveTransactionScope::~ActiveTransactionScope() noexcept {
  if (Outer)
    *Slot = nullptr;
  else
    ActiveTransaction->LeaveNested();
}

void ActiveTransactionScope::ReportFailure(
    const ErrorDiagnostic &Diagnostic) const {
  if (Outer)
    ActiveTransaction->Poison(Diagnostic);
  else
    ActiveTransaction->RecordNestedFailure(Diagnostic);
}

} // namespace Luna::Detail
