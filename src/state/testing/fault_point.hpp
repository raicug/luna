#pragma once

namespace Luna::Detail {

enum class StateFaultPoint {
  BindingRecordAllocation,
  BindingInstallation,
  FreezePreparation,
  TransactionPreparation,
  BindingPathJournal,
  TransactionPublication,
  TransactionConsistency,
  TransactionUndo,
  LifecycleModuleStaging,
  LifecycleTypeStaging,
  LifecycleReflectionStaging,
  LifecycleCallback,
  LifecycleMigration,
  LifecycleCachePreparation,
  LifecycleDispatchStaging,
  LifecyclePublication,
  ArgumentInspection,
  MemberValuePublication,
  ReturnStackCapacity,
  ReturnWrite,
  VoidFinalization,
  MissingMetadata,
  ExecutionThreadCreation,
  ExecutionErrorDiagnostic,
  Count
};

}
