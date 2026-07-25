#pragma once

namespace Luna::Detail {

enum class StateFaultPoint {
  BindingRecordAllocation,
  BindingInstallation,
  ArgumentInspection,
  ReturnStackCapacity,
  ReturnWrite,
  VoidFinalization,
  MissingMetadata,
  ExecutionThreadCreation,
  ExecutionErrorDiagnostic,
  Count
};

} // namespace Luna::Detail
