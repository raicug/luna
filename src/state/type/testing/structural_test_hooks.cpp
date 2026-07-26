// clang-format off
#include "state/type/testing/structural_test_hooks.hpp"

#include <luna/type/type_descriptor.hpp>

#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>
#include <lualib.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

class ScratchState final {
public:
  ScratchState() : Handle(luaL_newstate()) {}
  ~ScratchState() {
    if (Handle)
      lua_close(Handle);
  }

  ScratchState(const ScratchState &) = delete;
  ScratchState &operator=(const ScratchState &) = delete;

  [[nodiscard]] lua_State *Get() const noexcept { return Handle; }

private:
  lua_State *Handle = nullptr;
};

[[nodiscard]] bool Push(lua_State *State, const ScriptValue &Input) {
  if (!State || !lua_checkstack(State, 4))
    return false;

  switch (Input.Kind()) {
  case ScriptValue::ScriptKind::Nil:
    lua_pushnil(State);
    return true;
  case ScriptValue::ScriptKind::Boolean:
    lua_pushboolean(State, Input.BooleanValue() ? 1 : 0);
    return true;
  case ScriptValue::ScriptKind::Number:
    lua_pushnumber(State, Input.NumberValue());
    return true;
  case ScriptValue::ScriptKind::Text:
    lua_pushlstring(State, Input.TextValue().data(), Input.TextValue().size());
    return true;
  case ScriptValue::ScriptKind::Array: {
    const std::vector<ScriptValue> &Items = Input.Items();
    lua_createtable(State, static_cast<int>(Items.size()), 0);
    const int TableIndex = lua_gettop(State);
    for (std::size_t Index = 0; Index < Items.size(); ++Index) {
      if (!Push(State, Items[Index]))
        return false;
      lua_rawseti(State, TableIndex, static_cast<int>(Index + 1));
    }
    return true;
  }
  case ScriptValue::ScriptKind::Table: {
    const std::vector<ScriptValue> &Items = Input.Items();
    lua_createtable(State, 0, static_cast<int>(Items.size() / 2));
    const int TableIndex = lua_gettop(State);
    for (std::size_t Index = 0; Index + 1 < Items.size(); Index += 2) {
      if (!Push(State, Items[Index]) || !Push(State, Items[Index + 1]))
        return false;
      lua_rawset(State, TableIndex);
    }
    return true;
  }
  }
  return false;
}

[[nodiscard]] StructuredDiagnostic Unavailable(const TypeDescriptor &Type) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = StructuredFailure::UnavailableType;
  Diagnostic.ExpectedType = StructuralPublicName(Type);
  return Diagnostic;
}

// The canonical type one published pack element belongs to.
[[nodiscard]] TypeDescriptor ElementTypeOf(const TypeDescriptor &Type,
                                           std::size_t Position) {
  const std::span<const TypeDescriptor> Children = Type.Children();
  if (Children.empty())
    return TypeDescriptor::Unsupported();
  if (Children.size() == 1)
    return Children[0];
  return Position < Children.size() ? Children[Position]
                                    : TypeDescriptor::Unsupported();
}

[[nodiscard]] bool IsOrderedPack(const TypeDescriptor &Type) {
  return Type.Kind() == TypeKind::Pair || Type.Kind() == TypeKind::Tuple ||
         Type.Kind() == TypeKind::ReturnPack;
}

} // namespace

ScriptValue ScriptValue::Nil() { return ScriptValue(); }

ScriptValue ScriptValue::Boolean(bool Flag) {
  ScriptValue Described;
  Described.KindValue = ScriptKind::Boolean;
  Described.FlagValue = Flag;
  return Described;
}

ScriptValue ScriptValue::Number(double Value) {
  ScriptValue Described;
  Described.KindValue = ScriptKind::Number;
  Described.NumberStorage = Value;
  return Described;
}

ScriptValue ScriptValue::Text(std::string Value) {
  ScriptValue Described;
  Described.KindValue = ScriptKind::Text;
  Described.TextStorage = std::move(Value);
  return Described;
}

ScriptValue ScriptValue::Array(std::vector<ScriptValue> Elements) {
  ScriptValue Described;
  Described.KindValue = ScriptKind::Array;
  Described.ItemValues = std::move(Elements);
  return Described;
}

ScriptValue ScriptValue::Table(std::vector<ScriptValue> Entries) {
  ScriptValue Described;
  Described.KindValue = ScriptKind::Table;
  Described.ItemValues = std::move(Entries);
  return Described;
}

std::shared_ptr<const TypeGeneration>
StructuralConversionTestHooks::GenerationFor(const TypeDescriptor &Type,
                                             std::vector<TypeRecord> Extra) {
  std::shared_ptr<const TypeGeneration> Types = BuiltInTypeGeneration();
  if (!Types)
    return nullptr;

  if (!Extra.empty()) {
    TypeDeclarationStatus Status = TypeDeclarationStatus::Acceptable;
    Types = TypeGeneration::Derive(*Types, std::move(Extra), Status);
    if (!Types)
      return nullptr;
  }

  std::vector<TypeRecord> Declared;
  TypeDescriptor Blocking;
  if (DeclareStructuralTypes(*Types, Type, Declared, Blocking) !=
      StructuralDeclarationStatus::Declared)
    return nullptr;
  if (Declared.empty())
    return Types;

  TypeDeclarationStatus Status = TypeDeclarationStatus::Acceptable;
  return TypeGeneration::Derive(*Types, std::move(Declared), Status);
}

StructuralReadObservation
StructuralConversionTestHooks::Read(const ScriptValue &Input,
                                    const TypeDescriptor &Type,
                                    std::vector<TypeRecord> Extra) {
  StructuralReadObservation Observation;
  const std::shared_ptr<const TypeGeneration> Types =
      GenerationFor(Type, std::move(Extra));
  ScratchState State;
  if (!Types || !State.Get()) {
    Observation.Diagnostic = Unavailable(Type);
    return Observation;
  }

  const int EntryDepth = lua_gettop(State.Get());
  if (!Push(State.Get(), Input)) {
    Observation.Diagnostic = Unavailable(Type);
    return Observation;
  }

  const int ValueIndex = lua_gettop(State.Get());
  StructuredReadResult Result =
      ReadStructuredValue(*Types, State.Get(), ValueIndex, Type);
  Observation.Accepted = Result.IsSuccess();
  Observation.ConvertedValue = std::move(Result.ConvertedValue);
  Observation.Diagnostic = std::move(Result.Diagnostic);
  Observation.StackDepthDelta = lua_gettop(State.Get()) - EntryDepth - 1;
  return Observation;
}

StructuralReadObservation StructuralConversionTestHooks::ReadPack(
    const std::vector<ScriptValue> &Inputs, const TypeDescriptor &Type,
    std::size_t FirstArgumentPosition, std::vector<TypeRecord> Extra) {
  StructuralReadObservation Observation;
  const std::shared_ptr<const TypeGeneration> Types =
      GenerationFor(Type, std::move(Extra));
  ScratchState State;
  if (!Types || !State.Get()) {
    Observation.Diagnostic = Unavailable(Type);
    return Observation;
  }

  const int EntryDepth = lua_gettop(State.Get());
  for (const ScriptValue &Input : Inputs) {
    if (!Push(State.Get(), Input)) {
      Observation.Diagnostic = Unavailable(Type);
      return Observation;
    }
  }

  StructuredReadResult Result = ReadArgumentPack(
      *Types, State.Get(), EntryDepth + 1, static_cast<int>(Inputs.size()),
      FirstArgumentPosition, Type);
  Observation.Accepted = Result.IsSuccess();
  Observation.ConvertedValue = std::move(Result.ConvertedValue);
  Observation.Diagnostic = std::move(Result.Diagnostic);
  Observation.StackDepthDelta =
      lua_gettop(State.Get()) - EntryDepth - static_cast<int>(Inputs.size());
  return Observation;
}

StructuralWriteObservation
StructuralConversionTestHooks::Write(const StructuredValue &Source,
                                     const TypeDescriptor &Type,
                                     std::vector<TypeRecord> Extra) {
  StructuralWriteObservation Observation;
  const std::shared_ptr<const TypeGeneration> Types =
      GenerationFor(Type, std::move(Extra));
  ScratchState State;
  if (!Types || !State.Get()) {
    Observation.Diagnostic = Unavailable(Type);
    return Observation;
  }

  const int EntryDepth = lua_gettop(State.Get());
  StructuredWriteResult Result =
      WriteStructuredValue(*Types, State.Get(), Type, Source);
  Observation.Accepted = Result.IsSuccess();
  Observation.PublishedCount = Result.PublishedCount;
  Observation.Diagnostic = std::move(Result.Diagnostic);
  Observation.StackDepthDelta = lua_gettop(State.Get()) - EntryDepth;

  if (Observation.Accepted && Observation.PublishedCount == 1) {
    const StructuredReadResult ReadBack =
        ReadStructuredValue(*Types, State.Get(), lua_gettop(State.Get()), Type);
    Observation.RoundTripMatches =
        ReadBack.IsSuccess() &&
        HasSameStructure(Source, ReadBack.ConvertedValue);
  }
  return Observation;
}

StructuralWriteObservation
StructuralConversionTestHooks::PublishReturn(const StructuredValue &Source,
                                             const TypeDescriptor &Type,
                                             std::vector<TypeRecord> Extra) {
  StructuralWriteObservation Observation;
  const std::shared_ptr<const TypeGeneration> Types =
      GenerationFor(Type, std::move(Extra));
  ScratchState State;
  if (!Types || !State.Get()) {
    Observation.Diagnostic = Unavailable(Type);
    return Observation;
  }

  const int EntryDepth = lua_gettop(State.Get());
  StructuredWriteResult Result =
      PublishReturnShape(*Types, State.Get(), Type, Source);
  Observation.Accepted = Result.IsSuccess();
  Observation.PublishedCount = Result.PublishedCount;
  Observation.Diagnostic = std::move(Result.Diagnostic);
  Observation.StackDepthDelta = lua_gettop(State.Get()) - EntryDepth;

  if (!Observation.Accepted)
    return Observation;

  if (!IsOrderedPack(Type)) {
    if (Observation.PublishedCount == 1) {
      const StructuredReadResult ReadBack = ReadStructuredValue(
          *Types, State.Get(), lua_gettop(State.Get()), Type);
      Observation.RoundTripMatches =
          ReadBack.IsSuccess() &&
          HasSameStructure(Source, ReadBack.ConvertedValue);
    } else {
      Observation.RoundTripMatches = Source.IsNull();
    }
    return Observation;
  }

  // Ordered multiple values read back one element at a time, in order.
  const int First = lua_gettop(State.Get()) - Observation.PublishedCount + 1;
  std::vector<StructuredValue> Elements;
  bool Recovered = true;
  for (int Offset = 0; Offset < Observation.PublishedCount; ++Offset) {
    const TypeDescriptor ElementType =
        ElementTypeOf(Type, static_cast<std::size_t>(Offset));
    const StructuredReadResult ReadBack =
        ReadStructuredValue(*Types, State.Get(), First + Offset, ElementType);
    if (!ReadBack.IsSuccess()) {
      Recovered = false;
      break;
    }
    Elements.push_back(ReadBack.ConvertedValue);
  }
  Observation.RoundTripMatches =
      Recovered &&
      HasSameStructure(Source, StructuredValue::List(std::move(Elements)));
  return Observation;
}

} // namespace Luna::Detail
