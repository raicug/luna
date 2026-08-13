// clang-format off
#include "state/invocation/conversion/return_writer.hpp"

#include <luna/binding/class_construction.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_injector.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/owned_value_bridge.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/construction.hpp"

#include <lua.h>

#include <array>
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

  constexpr std::size_t InlineWriterCount = 4;
  std::array<const TypeRecord *, InlineWriterCount> InlineWriters{};
  std::vector<const TypeRecord *> OverflowWriters;
  const bool UsesOverflow = Staged.size() > InlineWriters.size();
  if (UsesOverflow)
    OverflowWriters.reserve(Staged.size());

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
    if (UsesOverflow)
      OverflowWriters.push_back(Record);
    else
      InlineWriters[Index] = Record;
  }

  const int PublishedCount = static_cast<int>(Staged.size());
  if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
      !lua_checkstack(State, PublishedCount + 1))
    return Failure(State, EntryDepth,
                   "Could not reserve stack capacity for " +
                       std::to_string(PublishedCount) + " return values.");

  for (std::size_t Index = 0; Index < Staged.size(); ++Index) {
    const TypeRecord *Record =
        UsesOverflow ? OverflowWriters[Index] : InlineWriters[Index];
    if (!Record->Write(State, Staged[Index]))
      return Failure(State, EntryDepth,
                     ReturnPositionText(Index + 1) + "could not be published.");
  }
  if (Faults.Consume(StateFaultPoint::ReturnWrite))
    return Failure(State, EntryDepth,
                   "Injected internal return-writer failure.");
  return {.Status = ReturnWriteStatus::PackPublished,
          .ReturnCount = PublishedCount};
}

[[nodiscard]] ReturnWriteResult
PublishOwnedValues(lua_State *State, int EntryDepth, bool IsSingleValue,
                   const InvocationOutcome &Outcome,
                   const TypeGeneration &Types, FaultInjector &Faults) {
  if (Outcome.Kind() != InvocationOutcomeKind::OwnedValues)
    return Failure(State, EntryDepth,
                   "Owned return metadata did not match callable outcome.");

  const ValuePack &Produced = Outcome.ReturnedOwnedValues();
  if (IsSingleValue && Produced.Size() != 1)
    return Failure(State, EntryDepth,
                   "A single owned return published " +
                       std::to_string(Produced.Size()) +
                       " values instead of one.");
  if (Produced.Size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return Failure(State, EntryDepth,
                   "Returned pack publishes more values than one call can "
                   "carry.");

  const std::size_t LargestString = Produced.LargestStringByteCount();
  if (LargestString > MaximumInvocationStringBytes)
    return Failure(State, EntryDepth,
                   "A returned string exceeds the " +
                       std::to_string(MaximumInvocationStringBytes) +
                       "-byte maximum.");

  for (std::size_t Index = 0; Index < Produced.Size(); ++Index) {
    const std::string Refusal =
        ClassifyPendingInstances(Produced.At(Index), Types);
    if (!Refusal.empty())
      return Failure(State, EntryDepth,
                     ReturnPositionText(Index + 1) +
                         "cannot be published: " + Refusal);
  }

  const int PublishedCount = static_cast<int>(Produced.Size());
  if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
      !lua_checkstack(State, PublishedCount + 1))
    return Failure(State, EntryDepth,
                   "Could not reserve stack capacity for " +
                       std::to_string(PublishedCount) + " return values.");

  for (std::size_t Index = 0; Index < Produced.Size(); ++Index) {
    if (!PushOwnedValueToStack(State, Produced.At(Index), Types))
      return Failure(State, EntryDepth,
                     ReturnPositionText(Index + 1) + "could not be published.");
  }
  if (Faults.Consume(StateFaultPoint::ReturnWrite))
    return Failure(State, EntryDepth,
                   "Injected internal return-writer failure.");
  return {.Status = IsSingleValue ? ReturnWriteStatus::ValueWritten
                                  : ReturnWriteStatus::PackPublished,
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
      if (Outcome.Kind() == InvocationOutcomeKind::OwnedValues)
        return PublishOwnedValues(State, EntryDepth, false, Outcome, Types,
                                  Faults);
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

    case ReturnDisposition::Owned:
    case ReturnDisposition::OwnedPack: {
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      return PublishOwnedValues(
          State, EntryDepth, Metadata.Disposition() == ReturnDisposition::Owned,
          Outcome, Types, Faults);
    }

    case ReturnDisposition::Chunk: {
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      if (Outcome.Kind() != InvocationOutcomeKind::Chunk ||
          Outcome.ChunkBytecode().empty())
        return Failure(State, EntryDepth,
                       "Chunk return metadata did not match callable outcome.");
      if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
          !lua_checkstack(State, 2))
        return Failure(State, EntryDepth,
                       "Could not reserve stack capacity for return value.");

      const std::string ChunkName =
          std::string("=") + (Outcome.ChunkName().empty()
                                  ? std::string("LunaChunk")
                                  : Outcome.ChunkName());
      const std::string &Bytecode = Outcome.ChunkBytecode();
      if (luau_load(State, ChunkName.c_str(), Bytecode.data(), Bytecode.size(),
                    0) != LUA_OK) {
        lua_settop(State, EntryDepth);
        return Failure(State, EntryDepth,
                       "Returned chunk could not be loaded as a callable "
                       "value.");
      }
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
