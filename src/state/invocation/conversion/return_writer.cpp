// clang-format off
#include "state/invocation/conversion/return_writer.hpp"

#include <luna/binding/class_construction.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_injector.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/construction.hpp"

#include <lua.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ReturnWriteResult Failure(lua_State *State, int EntryDepth,
                                        std::string Message) {
  if (State)
    lua_settop(State, EntryDepth);
  return {.Status = ReturnWriteStatus::InternalFailure,
          .ReturnCount = 0,
          .Diagnostic = ErrorDiagnostic::Create(ErrorCategory::Internal,
                                                std::move(Message))};
}

[[nodiscard]] bool ValueMatches(const TypeRecord &Record,
                                const Value &ReturnedValue) {
  if (!Record.ValueRepresentation)
    return false;
  switch (*Record.ValueRepresentation) {
  case ValueKind::Boolean:
    return std::holds_alternative<bool>(ReturnedValue);
  case ValueKind::Integer:
    return std::holds_alternative<int>(ReturnedValue);
  case ValueKind::Number:
    return std::holds_alternative<double>(ReturnedValue);
  case ValueKind::String:
    return std::holds_alternative<std::string>(ReturnedValue);
  }
  return false;
}

[[nodiscard]] bool ExceedsSizePolicy(const TypeRecord &Record,
                                     const Value &ReturnedValue) {
  if (!Record.MaximumByteCount)
    return false;
  const auto *Text = std::get_if<std::string>(&ReturnedValue);
  return Text && Text->size() > *Record.MaximumByteCount;
}

[[nodiscard]] ValueKind StagedValueKind(const Value &Staged) {
  if (std::holds_alternative<bool>(Staged))
    return ValueKind::Boolean;
  if (std::holds_alternative<int>(Staged))
    return ValueKind::Integer;
  if (std::holds_alternative<double>(Staged))
    return ValueKind::Number;
  return ValueKind::String;
}

[[nodiscard]] std::string ReturnPositionText(std::size_t Position) {
  return "Return value " + std::to_string(Position) + " ";
}

[[nodiscard]] ReturnWriteResult
PublishReturnPack(lua_State *State, int EntryDepth,
                  const ReturnMetadata &Metadata,
                  const InvocationOutcome &Outcome, const TypeGeneration &Types,
                  FaultInjector &Faults) {
  if (Outcome.Kind() != InvocationOutcomeKind::Values)
    return Failure(State, EntryDepth,
                   "Pack return metadata did not match callable outcome.");

  const std::span<const Value> Staged = Outcome.ReturnedValues();
  const std::span<const ValueKind> Declared = Metadata.PackKinds();
  if (Metadata.HasDeclaredPackShape() && Declared.size() != Staged.size())
    return Failure(State, EntryDepth,
                   "Returned pack produced " + std::to_string(Staged.size()) +
                       " values but its declared shape publishes " +
                       std::to_string(Declared.size()) + ".");
  if (Staged.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return Failure(State, EntryDepth,
                   "Returned pack publishes more values than one call can "
                   "carry.");

  std::vector<const TypeRecord *> Writers;
  Writers.reserve(Staged.size());
  for (std::size_t Index = 0; Index < Staged.size(); ++Index) {
    const ValueKind Kind = Metadata.HasDeclaredPackShape()
                               ? Declared[Index]
                               : StagedValueKind(Staged[Index]);
    const std::string Position = ReturnPositionText(Index + 1);

    const TypeRecord *Record = Types.Find(Kind);
    if (!Record || !Record->IsWritable || !Record->Write)
      return Failure(State, EntryDepth,
                     Position + "has a type that is unavailable in the "
                                "captured type registry.");
    if (!ValueMatches(*Record, Staged[Index]))
      return Failure(State, EntryDepth,
                     Position + "did not match callable metadata.");
    if (ExceedsSizePolicy(*Record, Staged[Index]))
      return Failure(State, EntryDepth,
                     Position + "exceeds the " +
                         std::to_string(*Record->MaximumByteCount) +
                         "-byte maximum.");
    Writers.push_back(Record);
  }

  const int PublishedCount = static_cast<int>(Staged.size());
  if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
      !lua_checkstack(State, PublishedCount + 1))
    return Failure(State, EntryDepth,
                   "Could not reserve stack capacity for " +
                       std::to_string(PublishedCount) + " return values.");

  for (std::size_t Index = 0; Index < Staged.size(); ++Index) {
    if (!Writers[Index]->Write(State, Staged[Index]))
      return Failure(State, EntryDepth,
                     ReturnPositionText(Index + 1) + "could not be published.");
  }
  if (Faults.Consume(StateFaultPoint::ReturnWrite))
    return Failure(State, EntryDepth,
                   "Injected internal return-writer failure.");
  return {.Status = ReturnWriteStatus::PackPublished,
          .ReturnCount = PublishedCount};
}

} // namespace

ReturnWriteResult WriteInvocationReturn(lua_State *State,
                                        const ReturnMetadata &Metadata,
                                        const InvocationOutcome &Outcome,
                                        const TypeGeneration &Types,
                                        FaultInjector &Faults) noexcept {
  const int EntryDepth = State ? lua_gettop(State) : 0;
  try {
    switch (Metadata.Disposition()) {
    case ReturnDisposition::Suppress:
      return {.Status = ReturnWriteStatus::Suppressed, .ReturnCount = 0};

    case ReturnDisposition::Void:
      if (Faults.Consume(StateFaultPoint::VoidFinalization))
        return Failure(State, EntryDepth,
                       "Injected internal void-finalization failure.");
      if (Outcome.Kind() != InvocationOutcomeKind::Void)
        return Failure(State, EntryDepth,
                       "Void return metadata did not match callable outcome.");
      return {.Status = ReturnWriteStatus::VoidCompleted, .ReturnCount = 0};

    case ReturnDisposition::Pack: {
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      return PublishReturnPack(State, EntryDepth, Metadata, Outcome, Types,
                               Faults);
    }

    case ReturnDisposition::Value: {
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      if (!Metadata.Kind() || Outcome.Kind() != InvocationOutcomeKind::Value ||
          !Outcome.ReturnedValue())
        return Failure(State, EntryDepth,
                       "Value return metadata did not match callable outcome.");

      const TypeRecord *Record = Types.Find(*Metadata.Kind());
      if (!Record || !Record->IsWritable || !Record->Write)
        return Failure(State, EntryDepth,
                       "Returned value type is unavailable in the captured "
                       "type registry.");
      if (!ValueMatches(*Record, *Outcome.ReturnedValue()))
        return Failure(State, EntryDepth,
                       "Returned value type did not match callable metadata.");
      if (ExceedsSizePolicy(*Record, *Outcome.ReturnedValue()))
        return Failure(State, EntryDepth,
                       "Returned string exceeds the " +
                           std::to_string(*Record->MaximumByteCount) +
                           "-byte maximum.");
      if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
          !lua_checkstack(State, 1))
        return Failure(State, EntryDepth,
                       "Could not reserve stack capacity for return value.");

      if (!Record->Write(State, *Outcome.ReturnedValue()))
        return Failure(State, EntryDepth,
                       "Return writer could not publish the return value.");
      if (Faults.Consume(StateFaultPoint::ReturnWrite))
        return Failure(State, EntryDepth,
                       "Injected internal return-writer failure.");
      return {.Status = ReturnWriteStatus::ValueWritten, .ReturnCount = 1};
    }

    case ReturnDisposition::Instance: {
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      const StableTypeKey *Class = Metadata.InstanceKey();
      const ConstructedInstance *Produced = Outcome.ProducedInstance();
      if (!Class || Outcome.Kind() != InvocationOutcomeKind::Instance ||
          !Produced)
        return Failure(
            State, EntryDepth,
            "Instance return metadata did not match callable outcome.");
      if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
          !lua_checkstack(State, 1))
        return Failure(State, EntryDepth,
                       "Could not reserve stack capacity for return value.");

      const InstancePublication Published =
          PublishConstructedInstance(State, Types, *Class, *Produced);
      if (!Published.IsSuccess())
        return Failure(State, EntryDepth, Published.Diagnostic);
      if (Faults.Consume(StateFaultPoint::ReturnWrite)) {
        if (Published.EstablishedOwner)
          static_cast<void>(ReleasePublishedInstance(State, Published.Storage));
        return Failure(State, EntryDepth,
                       "Injected internal return-writer failure.");
      }
      return {.Status = ReturnWriteStatus::ValueWritten,
              .ReturnCount = Published.PublishedCount};
    }
    }
  } catch (...) {
    try {
      return Failure(State, EntryDepth,
                     "Unexpected internal return conversion failure.");
    } catch (...) {
      if (State)
        lua_settop(State, EntryDepth);
      return {};
    }
  }

  return Failure(State, EntryDepth,
                 "Unknown return disposition prevented conversion.");
}

ReturnWriteResult WriteInvocationReturn(lua_State *State,
                                        const ReturnMetadata &Metadata,
                                        const InvocationOutcome &Outcome,
                                        FaultInjector &Faults) noexcept {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  if (!Types)
    return Failure(State, State ? lua_gettop(State) : 0,
                   "Return writer has no captured type registry.");
  return WriteInvocationReturn(State, Metadata, Outcome, *Types, Faults);
}

} // namespace Luna::Detail
