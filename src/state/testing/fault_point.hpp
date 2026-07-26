#pragma once

namespace Luna::Detail {

enum class StateFaultPoint {
  BindingRecordAllocation,
  BindingInstallation,
  // Preparation of one immutable freeze-cache generation, before any cache or
  // lifecycle flag is published.
  FreezePreparation,
  // Preparation of the replacement immutable stores of one transaction, before
  // any protected virtual-machine resource is staged.
  TransactionPreparation,
  // Capture of the exact prior value of one canonical virtual-machine path,
  // before that path is installed over.
  BindingPathJournal,
  // Preparation of the final candidate generation set, immediately before the
  // publication phase begins.
  TransactionPublication,
  // The internal consistency check that runs after installation and before
  // publication.
  TransactionConsistency,
  // Reverse restoration of one journalled virtual-machine path.
  TransactionUndo,
  ArgumentInspection,
  // Publication of the value one member getter already produced, immediately
  // before it would become a script-visible result.
  MemberValuePublication,
  ReturnStackCapacity,
  ReturnWrite,
  VoidFinalization,
  MissingMetadata,
  ExecutionThreadCreation,
  ExecutionErrorDiagnostic,
  Count
};

} // namespace Luna::Detail
