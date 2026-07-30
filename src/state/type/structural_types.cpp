// clang-format off
#include "state/type/structural_types.hpp"

#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/member_diagnostics.hpp"
#include "state/userdata/ownership.hpp"
#include "state/userdata/value_exposure.hpp"
#include "state/vm/enum_item.hpp"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr double MaximumSinglePrecisionMagnitude =
    static_cast<double>(std::numeric_limits<float>::max());

constexpr std::size_t MaximumTableElementPosition =
    static_cast<std::size_t>(std::numeric_limits<int>::max());

[[nodiscard]] int AbsoluteIndex(lua_State *State, int StackIndex) {
  if (StackIndex > 0 || StackIndex <= LUA_REGISTRYINDEX)
    return StackIndex;
  return lua_gettop(State) + StackIndex + 1;
}

[[nodiscard]] StructuredDiagnostic Internal(const ConversionScope &Scope,
                                            const TypeRecord &Record) {
  return Scope.Reject(StructuredFailure::InternalFailure, Record);
}

[[nodiscard]] StructuredDiagnostic Mismatch(const ConversionScope &Scope,
                                            const TypeRecord &Record,
                                            int ActualType) {
  StructuredDiagnostic Diagnostic =
      Scope.Reject(StructuredFailure::TypeMismatch, Record);
  const char *Name =
      Scope.State() ? lua_typename(Scope.State(), ActualType) : nullptr;
  Diagnostic.ReceivedType = Name ? Name : "unknown";
  return Diagnostic;
}

[[nodiscard]] StructuredDiagnostic Unreserved(const ConversionScope &Scope,
                                              const TypeRecord &Record,
                                              std::size_t RequestedSlots) {
  StructuredDiagnostic Diagnostic =
      Scope.Reject(StructuredFailure::StackUnavailable, Record);
  Diagnostic.ReceivedCount = RequestedSlots;
  return Diagnostic;
}

[[nodiscard]] bool Reserve(lua_State *State, int Slots) {
  return State != nullptr && lua_checkstack(State, Slots) != 0;
}

[[nodiscard]] const TypeRecord *ChildRecordOf(const ConversionScope &Scope,
                                              const TypeRecord &Record,
                                              std::size_t Index) {
  if (Index >= Record.NestedTypes.size())
    return nullptr;
  return Scope.Types().Find(Record.NestedTypes[Index]);
}

[[nodiscard]] const TypeRecord *ElementRecordOf(const ConversionScope &Scope,
                                                const TypeRecord &Record,
                                                std::size_t Position) {
  if (Record.NestedTypes.size() == 1)
    return ChildRecordOf(Scope, Record, 0);
  return ChildRecordOf(Scope, Record, Position);
}

struct SequenceShape final {
  bool Accepted = false;
  std::size_t Count = 0;
  StructuredFailure Failure = StructuredFailure::None;
};

[[nodiscard]] SequenceShape InspectSequenceShape(lua_State *State,
                                                 int TableIndex) {
  SequenceShape Shape;
  if (!Reserve(State, 3)) {
    Shape.Failure = StructuredFailure::StackUnavailable;
    return Shape;
  }

  std::size_t Count = 0;
  double HighestPosition = 0.0;
  int Iterator = 0;
  while ((Iterator = lua_rawiter(State, TableIndex, Iterator)) >= 0) {
    bool KeyIsPosition = false;
    if (lua_type(State, -2) == LUA_TNUMBER) {
      const double Position = lua_tonumberx(State, -2, nullptr);
      if (std::isfinite(Position) && std::trunc(Position) == Position &&
          Position >= 1.0 &&
          Position <= static_cast<double>(MaximumTableElementPosition)) {
        KeyIsPosition = true;
        HighestPosition = std::max(HighestPosition, Position);
      }
    }
    lua_pop(State, 2);
    if (!KeyIsPosition) {
      Shape.Failure = StructuredFailure::ForeignTableKey;
      return Shape;
    }
    ++Count;
  }

  if (HighestPosition != static_cast<double>(Count)) {
    Shape.Failure = StructuredFailure::ForeignTableKey;
    return Shape;
  }

  Shape.Accepted = true;
  Shape.Count = Count;
  return Shape;
}

struct RawMapKey final {
  int Rank = 0;
  bool BooleanKey = false;
  double NumberKey = 0.0;
  std::string TextKey;
};

[[nodiscard]] bool RawMapKeyPrecedes(const RawMapKey &Left,
                                     const RawMapKey &Right) {
  if (Left.Rank != Right.Rank)
    return Left.Rank < Right.Rank;
  switch (Left.Rank) {
  case 0:
    return static_cast<int>(Left.BooleanKey) <
           static_cast<int>(Right.BooleanKey);
  case 1:
    return Left.NumberKey < Right.NumberKey;
  default:
    return Left.TextKey < Right.TextKey;
  }
}

[[nodiscard]] std::string MapKeyPathText(const RawMapKey &Key) {
  switch (Key.Rank) {
  case 0:
    return Key.BooleanKey ? "true" : "false";
  case 1:
    return FormatConversionNumber(Key.NumberKey);
  default:
    return "\"" + Key.TextKey + "\"";
  }
}

void PushRawMapKey(lua_State *State, const RawMapKey &Key) {
  switch (Key.Rank) {
  case 0:
    lua_pushboolean(State, Key.BooleanKey ? 1 : 0);
    return;
  case 1:
    lua_pushnumber(State, Key.NumberKey);
    return;
  default:
    lua_pushlstring(State, Key.TextKey.data(), Key.TextKey.size());
    return;
  }
}

} // namespace

std::string_view
StructuralDeclarationStatusText(StructuralDeclarationStatus Status) noexcept {
  switch (Status) {
  case StructuralDeclarationStatus::Declared:
    return "declared";
  case StructuralDeclarationStatus::UnsupportedDescriptor:
    return "unsupported_descriptor";
  case StructuralDeclarationStatus::UnavailableLeaf:
    return "unavailable_leaf";
  }
  return "unsupported_descriptor";
}

TypeDescriptor OptionalTypeOf(TypeDescriptor Inner) {
  std::vector<TypeDescriptor> Children;
  Children.push_back(std::move(Inner));
  return TypeDescriptor::ForStructure(TypeKind::Optional, std::move(Children));
}

TypeDescriptor SequenceTypeOf(TypeDescriptor Element) {
  std::vector<TypeDescriptor> Children;
  Children.push_back(std::move(Element));
  return TypeDescriptor::ForStructure(TypeKind::Sequence, std::move(Children));
}

TypeDescriptor FixedArrayTypeOf(TypeDescriptor Element, std::size_t Extent) {
  return TypeDescriptor::ForArray(std::move(Element), Extent);
}

TypeDescriptor MapTypeOf(TypeDescriptor Key, TypeDescriptor Mapped) {
  std::vector<TypeDescriptor> Children;
  Children.push_back(std::move(Key));
  Children.push_back(std::move(Mapped));
  return TypeDescriptor::ForStructure(TypeKind::Map, std::move(Children));
}

TypeDescriptor PairTypeOf(TypeDescriptor First, TypeDescriptor Second) {
  std::vector<TypeDescriptor> Children;
  Children.push_back(std::move(First));
  Children.push_back(std::move(Second));
  return TypeDescriptor::ForStructure(TypeKind::Pair, std::move(Children));
}

TypeDescriptor TupleTypeOf(std::vector<TypeDescriptor> Elements) {
  return TypeDescriptor::ForStructure(TypeKind::Tuple, std::move(Elements));
}

TypeDescriptor ArgumentPackTypeOf(std::vector<TypeDescriptor> Elements) {
  return TypeDescriptor::ForStructure(TypeKind::ArgumentPack,
                                      std::move(Elements));
}

TypeDescriptor ReturnPackTypeOf(std::vector<TypeDescriptor> Elements) {
  return TypeDescriptor::ForStructure(TypeKind::ReturnPack,
                                      std::move(Elements));
}

std::string StructuralPublicName(const TypeDescriptor &Type) {
  if (const auto Fixed = Type.FixedKey()) {
    switch (*Fixed) {
    case FixedTypeKey::Void:
      return "void";
    case FixedTypeKey::Boolean:
      return "boolean";
    case FixedTypeKey::Int32:
      return "signed 32-bit integer";
    case FixedTypeKey::Float:
      return "single-precision number";
    case FixedTypeKey::Double:
      return "number";
    case FixedTypeKey::String:
      return "string";
    case FixedTypeKey::StringView:
      return "string view";
    case FixedTypeKey::CString:
      return "C string";
    case FixedTypeKey::Null:
      return "null";
    case FixedTypeKey::Value:
      return "value";
    case FixedTypeKey::ValuePack:
      return "value pack";
    }
    return "unsupported type";
  }

  const std::span<const TypeDescriptor> Children = Type.Children();
  const auto ChildName = [&Children](std::size_t Index) {
    return Index < Children.size() ? StructuralPublicName(Children[Index])
                                   : std::string("unsupported type");
  };
  const auto ChildList = [&Children]() {
    std::string Text;
    for (std::size_t Index = 0; Index < Children.size(); ++Index) {
      if (Index != 0)
        Text += ", ";
      Text += StructuralPublicName(Children[Index]);
    }
    return Text;
  };

  switch (Type.Kind()) {
  case TypeKind::Enumeration:
  case TypeKind::Class:
  case TypeKind::Converted:
    return Type.Key().Text();
  case TypeKind::Optional:
    return "optional " + ChildName(0);
  case TypeKind::Sequence:
    return "sequence of " + ChildName(0);
  case TypeKind::Array:
    return "array of " + std::to_string(Type.ArrayExtent()) + " " +
           ChildName(0);
  case TypeKind::Map:
    return "map of " + ChildName(0) + " to " + ChildName(1);
  case TypeKind::Pair:
    return "pair of " + ChildName(0) + " and " + ChildName(1);
  case TypeKind::Tuple:
    return "tuple of (" + ChildList() + ")";
  case TypeKind::ArgumentPack:
    return "argument pack of (" + ChildList() + ")";
  case TypeKind::ReturnPack:
    return "return pack of (" + ChildList() + ")";
  case TypeKind::Pointer:
    return "pointer to " + ChildName(0);
  case TypeKind::SharedOwnership:
    return "shared " + ChildName(0);
  case TypeKind::BorrowedReference:
    return "borrowed " + ChildName(0);
  case TypeKind::Fixed:
  case TypeKind::Unsupported:
    break;
  }
  return "unsupported type";
}

} // namespace Luna::Detail

namespace Luna::Detail {
namespace {

[[nodiscard]] StructuredReadResult ReadSinglePrecision(ConversionScope &Scope,
                                                       const TypeRecord &Record,
                                                       int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType != LUA_TNUMBER)
    return StructuredReadResult::Reject(Mismatch(Scope, Record, ActualType));

  const double Number = lua_tonumberx(State, StackIndex, nullptr);
  if (std::isfinite(Number) && (Number < -MaximumSinglePrecisionMagnitude ||
                                Number > MaximumSinglePrecisionMagnitude)) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::SinglePrecisionRange, Record);
    Diagnostic.ReceivedType = "number";
    Diagnostic.ReceivedNumber = Number;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  const double Narrowed = static_cast<double>(static_cast<float>(Number));
  return StructuredReadResult::Accept(StructuredValue::Scalar(Value(Narrowed)));
}

[[nodiscard]] StructuredWriteResult
WriteSinglePrecision(ConversionScope &Scope, const TypeRecord &Record,
                     const StructuredValue &Source) {
  lua_State *State = Scope.State();
  const Value *Scalar = Source.ScalarValue();
  const auto *Number = Scalar ? std::get_if<double>(Scalar) : nullptr;
  if (!State || !Number)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  if (std::isfinite(*Number) && (*Number < -MaximumSinglePrecisionMagnitude ||
                                 *Number > MaximumSinglePrecisionMagnitude)) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::SinglePrecisionRange, Record);
    Diagnostic.ReceivedNumber = *Number;
    return StructuredWriteResult::Reject(std::move(Diagnostic));
  }
  if (!Reserve(State, 1))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));

  lua_pushnumber(State, static_cast<double>(static_cast<float>(*Number)));
  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] bool IsCStringType(const TypeRecord &Record) {
  return Record.Descriptor.FixedKey() == FixedTypeKey::CString;
}

[[nodiscard]] StructuredReadResult ReadStringLike(ConversionScope &Scope,
                                                  const TypeRecord &Record,
                                                  int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType != LUA_TSTRING)
    return StructuredReadResult::Reject(Mismatch(Scope, Record, ActualType));

  std::size_t Length = 0;
  const char *Bytes = lua_tolstring(State, StackIndex, &Length);
  if (!Bytes && Length != 0)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const std::size_t Permitted =
      Record.MaximumByteCount.value_or(MaximumInvocationStringBytes);
  if (Length > Permitted) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::StringTooLong, Record);
    Diagnostic.ReceivedType = "string";
    Diagnostic.ReceivedCount = Length;
    Diagnostic.PermittedCount = Permitted;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  const std::string_view Text(Bytes ? Bytes : "", Length);
  if (IsCStringType(Record) && Text.find('\0') != std::string_view::npos) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::EmbeddedNullByte, Record);
    Diagnostic.ReceivedType = "string";
    Diagnostic.ReceivedCount = Length;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  return StructuredReadResult::Accept(
      StructuredValue::Scalar(Value(std::string(Text))));
}

[[nodiscard]] StructuredWriteResult
WriteStringLike(ConversionScope &Scope, const TypeRecord &Record,
                const StructuredValue &Source) {
  lua_State *State = Scope.State();
  const Value *Scalar = Source.ScalarValue();
  const auto *Text = Scalar ? std::get_if<std::string>(Scalar) : nullptr;
  if (!State || !Text)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  const std::size_t Permitted =
      Record.MaximumByteCount.value_or(MaximumInvocationStringBytes);
  if (Text->size() > Permitted) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::StringTooLong, Record);
    Diagnostic.ReceivedCount = Text->size();
    Diagnostic.PermittedCount = Permitted;
    return StructuredWriteResult::Reject(std::move(Diagnostic));
  }
  if (IsCStringType(Record) && Text->find('\0') != std::string::npos) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::EmbeddedNullByte, Record);
    Diagnostic.ReceivedCount = Text->size();
    return StructuredWriteResult::Reject(std::move(Diagnostic));
  }
  if (!Reserve(State, 1))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));

  lua_pushlstring(State, Text->data(), Text->size());
  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] StructuredReadResult
ReadNull(ConversionScope &Scope, const TypeRecord &Record, int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType == LUA_TNIL || ActualType == LUA_TNONE)
    return StructuredReadResult::Accept(StructuredValue::Null());
  return StructuredReadResult::Reject(Mismatch(Scope, Record, ActualType));
}

[[nodiscard]] StructuredWriteResult WriteNull(ConversionScope &Scope,
                                              const TypeRecord &Record,
                                              const StructuredValue &Source) {
  lua_State *State = Scope.State();
  if (!State || !Source.IsNull())
    return StructuredWriteResult::Reject(Internal(Scope, Record));
  if (!Reserve(State, 1))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));

  lua_pushnil(State);
  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] StructuredReadResult
ReadOptional(ConversionScope &Scope, const TypeRecord &Record, int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType == LUA_TNIL || ActualType == LUA_TNONE)
    return StructuredReadResult::Accept(StructuredValue::Null());

  const TypeRecord *Inner = ChildRecordOf(Scope, Record, 0);
  if (!Inner)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));
  return ReadThroughRecord(Scope, *Inner, StackIndex);
}

[[nodiscard]] StructuredWriteResult
WriteOptional(ConversionScope &Scope, const TypeRecord &Record,
              const StructuredValue &Source) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  if (Source.IsNull()) {
    if (!Reserve(State, 1))
      return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));
    lua_pushnil(State);
    return StructuredWriteResult::Accept(1);
  }

  const TypeRecord *Inner = ChildRecordOf(Scope, Record, 0);
  if (!Inner)
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));
  return WriteThroughRecord(Scope, *Inner, Source);
}

[[nodiscard]] bool PermittedElementCount(const TypeRecord &Record,
                                         std::size_t &Permitted) {
  switch (Record.Descriptor.Kind()) {
  case TypeKind::Array:
    Permitted = Record.Descriptor.ArrayExtent();
    return true;
  case TypeKind::Pair:
  case TypeKind::Tuple:
    Permitted = Record.NestedTypes.size();
    return true;
  case TypeKind::ArgumentPack:
  case TypeKind::ReturnPack:
    if (Record.NestedTypes.size() <= 1)
      return false;
    Permitted = Record.NestedTypes.size();
    return true;
  default:
    return false;
  }
}

[[nodiscard]] StructuredDiagnostic CountMismatch(const ConversionScope &Scope,
                                                 const TypeRecord &Record,
                                                 std::size_t Received,
                                                 std::size_t Permitted) {
  StructuredDiagnostic Diagnostic =
      Scope.Reject(StructuredFailure::ElementCountMismatch, Record);
  Diagnostic.ReceivedCount = Received;
  Diagnostic.PermittedCount = Permitted;
  return Diagnostic;
}

[[nodiscard]] StructuredReadResult ReadTableAsList(ConversionScope &Scope,
                                                   const TypeRecord &Record,
                                                   int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType != LUA_TTABLE)
    return StructuredReadResult::Reject(Mismatch(Scope, Record, ActualType));

  const int TableIndex = AbsoluteIndex(State, StackIndex);
  const SequenceShape Shape = InspectSequenceShape(State, TableIndex);
  if (!Shape.Accepted) {
    if (Shape.Failure == StructuredFailure::StackUnavailable)
      return StructuredReadResult::Reject(Unreserved(Scope, Record, 3));
    return StructuredReadResult::Reject(Scope.Reject(Shape.Failure, Record));
  }

  std::size_t Permitted = 0;
  if (PermittedElementCount(Record, Permitted) && Shape.Count != Permitted)
    return StructuredReadResult::Reject(
        CountMismatch(Scope, Record, Shape.Count, Permitted));

  if (!Reserve(State, 2))
    return StructuredReadResult::Reject(Unreserved(Scope, Record, 2));

  std::vector<StructuredValue> Elements;
  Elements.reserve(Shape.Count);
  for (std::size_t Position = 0; Position < Shape.Count; ++Position) {
    const TypeRecord *Element = ElementRecordOf(Scope, Record, Position);
    if (!Element)
      return StructuredReadResult::Reject(
          Scope.Reject(StructuredFailure::UnavailableType, Record));

    lua_rawgeti(State, TableIndex, static_cast<int>(Position + 1));
    const int ElementIndex = lua_gettop(State);

    StructuredReadResult Staged;
    {
      const ConversionPathScope Path(Scope, Position + 1);
      Staged = ReadThroughRecord(Scope, *Element, ElementIndex);
    }
    lua_settop(State, ElementIndex - 1);
    if (!Staged.IsSuccess())
      return Staged;
    Elements.push_back(std::move(Staged.ConvertedValue));
  }

  return StructuredReadResult::Accept(
      StructuredValue::List(std::move(Elements)));
}

[[nodiscard]] StructuredWriteResult
WriteListAsTable(ConversionScope &Scope, const TypeRecord &Record,
                 const StructuredValue &Source) {
  lua_State *State = Scope.State();
  if (!State || Source.Kind() != StructuredKind::List)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  const std::size_t Count = Source.Size();
  std::size_t Permitted = 0;
  if (PermittedElementCount(Record, Permitted) && Count != Permitted)
    return StructuredWriteResult::Reject(
        CountMismatch(Scope, Record, Count, Permitted));
  if (Count > MaximumTableElementPosition)
    return StructuredWriteResult::Reject(
        CountMismatch(Scope, Record, Count, MaximumTableElementPosition));
  for (std::size_t Position = 0; Position < Count; ++Position) {
    if (!ElementRecordOf(Scope, Record, Position))
      return StructuredWriteResult::Reject(
          Scope.Reject(StructuredFailure::UnavailableType, Record));
  }
  if (!Reserve(State, 3))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 3));

  lua_createtable(State, static_cast<int>(Count), 0);
  const int TableIndex = lua_gettop(State);
  for (std::size_t Position = 0; Position < Count; ++Position) {
    const TypeRecord *Element = ElementRecordOf(Scope, Record, Position);
    const StructuredValue *Staged = Source.ElementAt(Position);
    if (!Element || !Staged) {
      lua_settop(State, TableIndex - 1);
      return StructuredWriteResult::Reject(Internal(Scope, Record));
    }

    StructuredWriteResult Written;
    {
      const ConversionPathScope Path(Scope, Position + 1);
      Written = WriteThroughRecord(Scope, *Element, *Staged);
    }
    if (!Written.IsSuccess() || Written.PublishedCount != 1) {
      lua_settop(State, TableIndex - 1);
      return Written.IsSuccess()
                 ? StructuredWriteResult::Reject(Internal(Scope, Record))
                 : Written;
    }
    lua_rawseti(State, TableIndex, static_cast<int>(Position + 1));
  }

  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] StructuredReadResult
ReadMap(ConversionScope &Scope, const TypeRecord &Record, int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int ActualType = lua_type(State, StackIndex);
  if (ActualType != LUA_TTABLE)
    return StructuredReadResult::Reject(Mismatch(Scope, Record, ActualType));

  const TypeRecord *KeyType = ChildRecordOf(Scope, Record, 0);
  const TypeRecord *MappedType = ChildRecordOf(Scope, Record, 1);
  if (!KeyType || !MappedType)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));
  if (!Reserve(State, 4))
    return StructuredReadResult::Reject(Unreserved(Scope, Record, 4));

  const int TableIndex = AbsoluteIndex(State, StackIndex);

  std::vector<RawMapKey> Keys;
  int Iterator = 0;
  while ((Iterator = lua_rawiter(State, TableIndex, Iterator)) >= 0) {
    const int RawKeyType = lua_type(State, -2);
    RawMapKey Key;
    bool Supported = true;
    switch (RawKeyType) {
    case LUA_TBOOLEAN:
      Key.Rank = 0;
      Key.BooleanKey = lua_toboolean(State, -2) != 0;
      break;
    case LUA_TNUMBER:
      Key.Rank = 1;
      Key.NumberKey = lua_tonumberx(State, -2, nullptr);
      break;
    case LUA_TSTRING: {
      Key.Rank = 2;
      std::size_t Length = 0;
      const char *Bytes = lua_tolstring(State, -2, &Length);
      Key.TextKey.assign(Bytes ? Bytes : "", Length);
      break;
    }
    default:
      Supported = false;
      break;
    }
    lua_pop(State, 2);

    if (!Supported) {
      StructuredDiagnostic Diagnostic =
          Scope.Reject(StructuredFailure::UnsupportedMapKey, Record);
      const char *Name = lua_typename(State, RawKeyType);
      Diagnostic.ReceivedType = Name ? Name : "unknown";
      return StructuredReadResult::Reject(std::move(Diagnostic));
    }
    Keys.push_back(std::move(Key));
  }
  std::sort(Keys.begin(), Keys.end(), RawMapKeyPrecedes);

  std::vector<StructuredValue> Entries;
  Entries.reserve(Keys.size() * 2);
  for (const RawMapKey &Key : Keys) {
    const std::string KeyText = MapKeyPathText(Key);

    PushRawMapKey(State, Key);
    lua_pushvalue(State, -1);
    lua_rawget(State, TableIndex);
    const int MappedIndex = lua_gettop(State);
    const int KeyIndex = MappedIndex - 1;

    StructuredReadResult StagedKey;
    {
      const ConversionPathScope KeyPath(Scope, KeyText, false);
      const ConversionPathScope KeyField(Scope, "Key", true);
      StagedKey = ReadThroughRecord(Scope, *KeyType, KeyIndex);
    }
    if (!StagedKey.IsSuccess()) {
      lua_settop(State, KeyIndex - 1);
      return StagedKey;
    }

    for (std::size_t Existing = 0; Existing * 2 < Entries.size(); ++Existing) {
      if (!HasSameStructure(Entries[Existing * 2], StagedKey.ConvertedValue))
        continue;
      lua_settop(State, KeyIndex - 1);
      const ConversionPathScope KeyPath(Scope, KeyText, false);
      return StructuredReadResult::Reject(
          Scope.Reject(StructuredFailure::DuplicateMapKey, Record));
    }

    StructuredReadResult StagedValue;
    {
      const ConversionPathScope ValuePath(Scope, KeyText, false);
      StagedValue = ReadThroughRecord(Scope, *MappedType, MappedIndex);
    }
    lua_settop(State, KeyIndex - 1);
    if (!StagedValue.IsSuccess())
      return StagedValue;

    Entries.push_back(std::move(StagedKey.ConvertedValue));
    Entries.push_back(std::move(StagedValue.ConvertedValue));
  }

  return StructuredReadResult::Accept(StructuredValue::Map(std::move(Entries)));
}

[[nodiscard]] StructuredWriteResult WriteMap(ConversionScope &Scope,
                                             const TypeRecord &Record,
                                             const StructuredValue &Source) {
  lua_State *State = Scope.State();
  if (!State || Source.Kind() != StructuredKind::Map)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  const TypeRecord *KeyType = ChildRecordOf(Scope, Record, 0);
  const TypeRecord *MappedType = ChildRecordOf(Scope, Record, 1);
  if (!KeyType || !MappedType)
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));

  const std::size_t Count = Source.Size();
  if (Count > MaximumTableElementPosition)
    return StructuredWriteResult::Reject(
        CountMismatch(Scope, Record, Count, MaximumTableElementPosition));
  if (!Reserve(State, 4))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 4));

  lua_createtable(State, 0, static_cast<int>(Count));
  const int TableIndex = lua_gettop(State);
  for (std::size_t Entry = 0; Entry < Count; ++Entry) {
    const StructuredValue *Key = Source.KeyAt(Entry);
    const StructuredValue *Mapped = Source.MappedAt(Entry);
    if (!Key || !Mapped) {
      lua_settop(State, TableIndex - 1);
      return StructuredWriteResult::Reject(Internal(Scope, Record));
    }

    StructuredWriteResult WrittenKey;
    {
      const ConversionPathScope Path(Scope, Entry + 1);
      const ConversionPathScope Field(Scope, "Key", true);
      WrittenKey = WriteThroughRecord(Scope, *KeyType, *Key);
    }
    if (!WrittenKey.IsSuccess() || WrittenKey.PublishedCount != 1) {
      lua_settop(State, TableIndex - 1);
      return WrittenKey.IsSuccess()
                 ? StructuredWriteResult::Reject(Internal(Scope, Record))
                 : WrittenKey;
    }

    const int PublishedKeyType = lua_type(State, -1);
    const bool KeyIsStorable = PublishedKeyType != LUA_TNIL &&
                               PublishedKeyType != LUA_TNONE &&
                               (PublishedKeyType != LUA_TNUMBER ||
                                !std::isnan(lua_tonumberx(State, -1, nullptr)));
    if (!KeyIsStorable) {
      lua_settop(State, TableIndex - 1);
      StructuredDiagnostic Diagnostic =
          Scope.Reject(StructuredFailure::UnsupportedMapKey, Record);
      const char *Name = lua_typename(State, PublishedKeyType);
      Diagnostic.ReceivedType = Name ? Name : "unknown";
      return StructuredWriteResult::Reject(std::move(Diagnostic));
    }

    StructuredWriteResult WrittenValue;
    {
      const ConversionPathScope Path(Scope, Entry + 1);
      WrittenValue = WriteThroughRecord(Scope, *MappedType, *Mapped);
    }
    if (!WrittenValue.IsSuccess() || WrittenValue.PublishedCount != 1) {
      lua_settop(State, TableIndex - 1);
      return WrittenValue.IsSuccess()
                 ? StructuredWriteResult::Reject(Internal(Scope, Record))
                 : WrittenValue;
    }
    lua_rawset(State, TableIndex);
  }

  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] const TypeRecord *IntegerRecordOf(const ConversionScope &Scope) {
  return Scope.Types().Find(TypeDescriptor::ForFixed(FixedTypeKey::Int32));
}

[[nodiscard]] StructuredDiagnostic
RejectEnumerator(const ConversionScope &Scope, const TypeRecord &Record,
                 int Candidate) {
  const bool IsBitflags = Record.Enumeration && Record.Enumeration->IsBitflags;
  StructuredDiagnostic Diagnostic =
      Scope.Reject(IsBitflags ? StructuredFailure::UnsupportedFlagBits
                              : StructuredFailure::UndeclaredEnumerator,
                   Record);
  Diagnostic.ReceivedNumber = static_cast<double>(Candidate);
  return Diagnostic;
}

[[nodiscard]] bool RefusesEnumerator(const TypeRecord &Record,
                                     int Candidate) noexcept {
  return Record.Enumeration &&
         !Record.Enumeration->Accepts(static_cast<std::int64_t>(Candidate));
}

[[nodiscard]] bool PublishesEnumeratorObjects(const TypeRecord &Record) {
  return Record.Enumeration && Record.Enumeration->PublishesObjects;
}

[[nodiscard]] StructuredReadResult
ReadEnumeratorObject(ConversionScope &Scope, const TypeRecord &Record,
                     int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int Index = AbsoluteIndex(State, StackIndex);
  const int Received = lua_type(State, Index);
  if (Received == LUA_TNONE)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::MissingElement, Record));
  if (Received != LUA_TUSERDATA)
    return StructuredReadResult::Reject(Mismatch(Scope, Record, Received));

  const void *Block = lua_touserdata(State, Index);
  const std::size_t ByteCount =
      Block ? static_cast<std::size_t>(lua_objlen(State, Index)) : 0;
  const EnumItemPayload *Payload = InspectEnumItem(Block, ByteCount);
  if (Payload == nullptr) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::ForeignUserdata, Record);
    Diagnostic.ReceivedType = "userdata";
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }
  if (!(Payload->Enumeration == Record.Identity)) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::UserdataTypeMismatch, Record);
    Diagnostic.ReceivedType = std::string(EnumItemTypeName);
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  const auto Candidate = static_cast<int>(Payload->Numeric);
  if (RefusesEnumerator(Record, Candidate))
    return StructuredReadResult::Reject(
        RejectEnumerator(Scope, Record, Candidate));
  return StructuredReadResult::Accept(
      StructuredValue::Scalar(Value(Candidate)));
}

[[nodiscard]] StructuredWriteResult
WriteEnumeratorObject(ConversionScope &Scope, const TypeRecord &Record,
                      int Candidate) {
  lua_State *State = Scope.State();
  EnumItemRegistry *Items = ObserveEnumItemRegistry(State);
  if (!State || Items == nullptr)
    return StructuredWriteResult::Reject(Internal(Scope, Record));
  if (!Reserve(State, 8))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 8));

  const std::string_view Name =
      Record.Enumeration->NameOf(static_cast<std::int64_t>(Candidate));
  if (!Items->Publish(State, Record.Identity,
                      static_cast<std::int64_t>(Candidate), Record.PublicName,
                      Name))
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableConversion, Record));
  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] StructuredReadResult ReadEnumeration(ConversionScope &Scope,
                                                   const TypeRecord &Record,
                                                   int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));
  if (PublishesEnumeratorObjects(Record))
    return ReadEnumeratorObject(Scope, Record, StackIndex);

  const TypeRecord *Integer = IntegerRecordOf(Scope);
  if (!Integer || !Integer->Read)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));

  const ArgumentReadResult Read = Integer->Read(State, StackIndex);
  if (!Read.IsSuccess())
    return StructuredReadResult::Reject(
        StructuredDiagnosticFrom(Read, Record.PublicName, Scope.Path()));

  const int Candidate = std::get<int>(*Read.ConvertedValue);
  if (RefusesEnumerator(Record, Candidate))
    return StructuredReadResult::Reject(
        RejectEnumerator(Scope, Record, Candidate));
  return StructuredReadResult::Accept(
      StructuredValue::Scalar(*Read.ConvertedValue));
}

[[nodiscard]] StructuredWriteResult
WriteEnumeration(ConversionScope &Scope, const TypeRecord &Record,
                 const StructuredValue &Source) {
  lua_State *State = Scope.State();
  const Value *Scalar = Source.ScalarValue();
  if (!State || !Scalar || !std::holds_alternative<int>(*Scalar))
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  const int Candidate = std::get<int>(*Scalar);
  if (RefusesEnumerator(Record, Candidate))
    return StructuredWriteResult::Reject(
        RejectEnumerator(Scope, Record, Candidate));
  if (PublishesEnumeratorObjects(Record))
    return WriteEnumeratorObject(Scope, Record, Candidate);

  const TypeRecord *Integer = IntegerRecordOf(Scope);
  if (!Integer || !Integer->Write)
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));
  if (!Reserve(State, 1))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));
  if (!Integer->Write(State, *Scalar))
    return StructuredWriteResult::Reject(Internal(Scope, Record));
  return StructuredWriteResult::Accept(1);
}

[[nodiscard]] StructuredDiagnostic RejectAccess(const ConversionScope &Scope,
                                                const TypeRecord &Record,
                                                UserdataAccessFailure Failure) {
  const StructuredFailure Reported =
      StructuredFailureForUserdataAccess(Failure);
  StructuredDiagnostic Diagnostic = Scope.Reject(Reported, Record);
  Diagnostic.ReceivedType = std::string(UserdataAccessFailureText(Failure));
  return Diagnostic;
}

[[nodiscard]] StructuredReadResult ReadClassHandle(ConversionScope &Scope,
                                                   const TypeRecord &Record,
                                                   int StackIndex) {
  lua_State *State = Scope.State();
  if (!State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  const int Index = AbsoluteIndex(State, StackIndex);
  const int Received = lua_type(State, Index);
  if (Received == LUA_TNONE)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::MissingElement, Record));

  const UserdataAccessContext *Context = ObserveUserdataAccessContext(State);
  const RegisteredClass *Registered =
      Context && Context->Classes ? Context->Classes->Find(Record.Identity)
                                  : nullptr;
  if (!Registered || !Registered->IsComplete())
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));

  UserdataAccessRequest Request;
  Request.Origin = Context->Origin;
  Request.Metatable = Registered->Metatable;
  Request.RequestedType = Registered->Type;
  Request.HandleProbe = Context->HandleProbe;
  Request.Relationships = &Context->Classes->Relationships();

  if (Received != LUA_TUSERDATA) {
    StructuredDiagnostic Diagnostic =
        Scope.Reject(StructuredFailure::ForeignUserdata, Record);
    const char *Name = lua_typename(State, Received);
    Diagnostic.ReceivedType = Name ? Name : "unknown";
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  const void *Block = lua_touserdata(State, Index);
  const std::size_t ByteCount =
      Block ? static_cast<std::size_t>(lua_objlen(State, Index)) : 0;
  const UserdataAccessResult Access =
      InspectUserdataAccess(Block, ByteCount, Request);
  if (!Access.IsPermitted())
    return StructuredReadResult::Reject(
        RejectAccess(Scope, Record, Access.Failure));
  return StructuredReadResult::Accept(
      StructuredValue::Handle(Access.Storage, Access.PermitsMutation));
}

[[nodiscard]] StructuredDiagnostic
RejectExposure(ConversionScope &Scope, const TypeRecord &Record,
               const ClassExposureResult &Exposed) {
  StructuredFailure Reported = StructuredFailure::InternalFailure;
  switch (Exposed.Status) {
  case ClassExposureStatus::ConflictingOwnership:
  case ClassExposureStatus::IncompatibleAccess:
    Reported = StructuredFailure::ConflictingOwnership;
    break;
  case ClassExposureStatus::IncompatibleType:
    Reported = StructuredFailure::UserdataTypeMismatch;
    break;
  case ClassExposureStatus::StackCapacityFailure:
    Reported = StructuredFailure::StackUnavailable;
    break;
  case ClassExposureStatus::UnestablishedOwnership:
    Reported =
        Exposed.Ownership == OwnershipFailure::MissingLifetimeHandle ||
                Exposed.Ownership == OwnershipFailure::ExpiredLifetimeHandle
            ? StructuredFailure::ExpiredUserdata
            : StructuredFailure::InternalFailure;
    break;
  case ClassExposureStatus::Created:
  case ClassExposureStatus::Reused:
  case ClassExposureStatus::UnavailableRequest:
  case ClassExposureStatus::NullStorage:
  case ClassExposureStatus::StorageUnavailable:
  case ClassExposureStatus::MetatableUnavailable:
  case ClassExposureStatus::ProtectedFailure:
    Reported = StructuredFailure::InternalFailure;
    break;
  }

  StructuredDiagnostic Diagnostic = Scope.Reject(Reported, Record);
  Diagnostic.ReceivedType =
      Exposed.Status == ClassExposureStatus::UnestablishedOwnership
          ? std::string(OwnershipFailureText(Exposed.Ownership))
          : std::string(ClassExposureStatusText(Exposed.Status));
  return Diagnostic;
}

[[nodiscard]] StructuredWriteResult
WriteClassHandle(ConversionScope &Scope, const TypeRecord &Record,
                 const StructuredValue &Source) {
  lua_State *State = Scope.State();
  if (!State || Source.Kind() != StructuredKind::Handle)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  const ClassExposureIntent *Intent = Source.HandleExposure();
  if (!Intent)
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableConversion, Record));

  UserdataExposureContext *Context = ObserveUserdataExposureContext(State);
  RegisteredClass *Registered =
      Context && Context->Classes
          ? Context->Classes->FindForUpdate(Record.Identity)
          : nullptr;
  if (!Registered || !Registered->IsComplete())
    return StructuredWriteResult::Reject(
        Scope.Reject(StructuredFailure::UnavailableType, Record));
  if (!Reserve(State, 8))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 8));

  const ClassExposureResult Exposed =
      PushExposedClassValue(State, *Context, *Registered, *Intent);
  if (!Exposed.IsSuccess())
    return StructuredWriteResult::Reject(
        RejectExposure(Scope, Record, Exposed));
  return StructuredWriteResult::Accept(1);
}

} // namespace
} // namespace Luna::Detail

namespace Luna::Detail {
namespace {

struct DeclarationTraits final {
  StructuredReadFunction Read = nullptr;
  StructuredWriteFunction Write = nullptr;
  LuauRepresentation Representation = LuauRepresentation::Table;
  bool IsNullable = false;
  ConversionRankCategory Rank = ConversionRankCategory::SafeBuiltIn;
  std::optional<ValueKind> Mapping;
  std::optional<std::size_t> MaximumBytes;
};

[[nodiscard]] TypeId IdentityOf(const TypeDescriptor &Type) {
  if (const auto Identity = TypeIdentityRegistry::ComputeIdentity(Type))
    return *Identity;
  return TypeId();
}

[[nodiscard]] TypeRecord Declare(const TypeDescriptor &Type,
                                 std::string PublicName,
                                 const DeclarationTraits &Traits) {
  TypeRecord Record;
  Record.Descriptor = Type;
  Record.Identity = IdentityOf(Type);
  Record.PublicName = std::move(PublicName);
  Record.Representation = Traits.Representation;
  Record.IsNullable = Traits.IsNullable;
  Record.IsReadable = true;
  Record.IsWritable = true;
  Record.Rank = Traits.Rank;
  Record.ValueRepresentation = Traits.Mapping;
  Record.MaximumByteCount = Traits.MaximumBytes;
  Record.StructuredRead = Traits.Read;
  Record.StructuredWrite = Traits.Write;
  for (const TypeDescriptor &Child : Type.Children())
    Record.NestedTypes.push_back(IdentityOf(Child));
  return Record;
}

[[nodiscard]] const TypeRecord *
FindStaged(const std::vector<TypeRecord> &Staged, const TypeDescriptor &Type) {
  for (const TypeRecord &Record : Staged) {
    if (Record.Descriptor == Type)
      return &Record;
  }
  return nullptr;
}

[[nodiscard]] const TypeRecord *FindKnown(const TypeGeneration &Known,
                                          const std::vector<TypeRecord> &Staged,
                                          const TypeDescriptor &Type) {
  if (const TypeRecord *Record = Known.Find(Type))
    return Record;
  return FindStaged(Staged, Type);
}

[[nodiscard]] DeclarationTraits SinglePrecisionTraits() {
  DeclarationTraits Traits;
  Traits.Read = &ReadSinglePrecision;
  Traits.Write = &WriteSinglePrecision;
  Traits.Representation = LuauRepresentation::Number;
  Traits.Mapping = ValueKind::Number;
  return Traits;
}

[[nodiscard]] DeclarationTraits TextTraits() {
  DeclarationTraits Traits;
  Traits.Read = &ReadStringLike;
  Traits.Write = &WriteStringLike;
  Traits.Representation = LuauRepresentation::String;
  Traits.Mapping = ValueKind::String;
  Traits.MaximumBytes = MaximumInvocationStringBytes;
  return Traits;
}

[[nodiscard]] DeclarationTraits NullTraits() {
  DeclarationTraits Traits;
  Traits.Read = &ReadNull;
  Traits.Write = &WriteNull;
  Traits.Representation = LuauRepresentation::Nil;
  Traits.IsNullable = true;
  Traits.Rank = ConversionRankCategory::Exact;
  return Traits;
}

[[nodiscard]] DeclarationTraits AggregateTraits() {
  DeclarationTraits Traits;
  Traits.Read = &ReadTableAsList;
  Traits.Write = &WriteListAsTable;
  Traits.Representation = LuauRepresentation::Table;
  return Traits;
}

[[nodiscard]] DeclarationTraits MapTraits() {
  DeclarationTraits Traits;
  Traits.Read = &ReadMap;
  Traits.Write = &WriteMap;
  Traits.Representation = LuauRepresentation::Table;
  return Traits;
}

[[nodiscard]] bool BuildRecord(const TypeGeneration &Known,
                               const std::vector<TypeRecord> &Staged,
                               const TypeDescriptor &Type, TypeRecord &Record) {
  const std::string PublicName = StructuralPublicName(Type);

  if (const auto Fixed = Type.FixedKey()) {
    switch (*Fixed) {
    case FixedTypeKey::Float:
      Record = Declare(Type, PublicName, SinglePrecisionTraits());
      return true;
    case FixedTypeKey::StringView:
    case FixedTypeKey::CString:
      Record = Declare(Type, PublicName, TextTraits());
      return true;
    case FixedTypeKey::Null:
      Record = Declare(Type, PublicName, NullTraits());
      return true;
    case FixedTypeKey::Void:
    case FixedTypeKey::Boolean:
    case FixedTypeKey::Int32:
    case FixedTypeKey::Double:
    case FixedTypeKey::String: {
      const TypeRecord *Base = FindKnown(
          Known, Staged, Type.WithQualification(CvQualification::None));
      if (!Base)
        return false;
      Record = *Base;
      Record.Descriptor = Type;
      Record.Identity = IdentityOf(Type);
      return Record.Identity.IsValid();
    }
    case FixedTypeKey::Value:
    case FixedTypeKey::ValuePack:
      return false;
    }
    return false;
  }

  switch (Type.Kind()) {
  case TypeKind::Optional: {
    const std::span<const TypeDescriptor> Children = Type.Children();
    if (Children.empty())
      return false;
    const TypeRecord *Inner = FindKnown(Known, Staged, Children[0]);
    if (!Inner)
      return false;

    DeclarationTraits Traits;
    Traits.Read = &ReadOptional;
    Traits.Write = &WriteOptional;
    Traits.Representation = Inner->Representation;
    Traits.IsNullable = true;
    Traits.Rank = Inner->Rank;
    Record = Declare(Type, PublicName, Traits);
    return true;
  }
  case TypeKind::Sequence:
  case TypeKind::Array:
  case TypeKind::Pair:
  case TypeKind::Tuple:
  case TypeKind::ArgumentPack:
  case TypeKind::ReturnPack:
    Record = Declare(Type, PublicName, AggregateTraits());
    return true;
  case TypeKind::Map:
    Record = Declare(Type, PublicName, MapTraits());
    return true;
  case TypeKind::Enumeration:
  case TypeKind::Class:
  case TypeKind::Converted:
    return false;
  case TypeKind::Pointer:
  case TypeKind::SharedOwnership:
  case TypeKind::BorrowedReference:
  case TypeKind::Fixed:
  case TypeKind::Unsupported:
    return false;
  }
  return false;
}

[[nodiscard]] StructuralDeclarationStatus
DeclareInto(const TypeGeneration &Known, const TypeDescriptor &Type,
            std::vector<TypeRecord> &Staged, TypeDescriptor &Blocking) {
  if (!Type.IsValid()) {
    Blocking = Type;
    return StructuralDeclarationStatus::UnsupportedDescriptor;
  }
  if (FindKnown(Known, Staged, Type))
    return StructuralDeclarationStatus::Declared;

  for (const TypeDescriptor &Child : Type.Children()) {
    const StructuralDeclarationStatus Status =
        DeclareInto(Known, Child, Staged, Blocking);
    if (Status != StructuralDeclarationStatus::Declared)
      return Status;
  }

  TypeRecord Record;
  if (!BuildRecord(Known, Staged, Type, Record) || !Record.IsComplete()) {
    Blocking = Type;
    return StructuralDeclarationStatus::UnavailableLeaf;
  }
  Staged.push_back(std::move(Record));
  return StructuralDeclarationStatus::Declared;
}

[[nodiscard]] StructuredDiagnostic UnavailableAt(const TypeDescriptor &Type) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = StructuredFailure::UnavailableType;
  Diagnostic.ExpectedType = StructuralPublicName(Type);
  return Diagnostic;
}

[[nodiscard]] StructuredDiagnostic InternalAt(const TypeDescriptor &Type) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = StructuredFailure::InternalFailure;
  Diagnostic.ExpectedType = StructuralPublicName(Type);
  return Diagnostic;
}

} // namespace

std::vector<TypeRecord> BuiltInScalarTypeRecords() {
  const auto Named = [](FixedTypeKey Key, const DeclarationTraits &Traits) {
    const TypeDescriptor Type = TypeDescriptor::ForFixed(Key);
    return Declare(Type, StructuralPublicName(Type), Traits);
  };

  std::vector<TypeRecord> Records;
  Records.reserve(4);
  Records.push_back(Named(FixedTypeKey::Float, SinglePrecisionTraits()));
  Records.push_back(Named(FixedTypeKey::StringView, TextTraits()));
  Records.push_back(Named(FixedTypeKey::CString, TextTraits()));
  Records.push_back(Named(FixedTypeKey::Null, NullTraits()));
  return Records;
}

std::shared_ptr<const TypeGeneration> BuiltInTypeGeneration() {
  static const std::shared_ptr<const TypeGeneration> Shared = [] {
    const std::shared_ptr<const TypeGeneration> Foundation =
        TypeGeneration::Foundation();
    if (!Foundation)
      return std::shared_ptr<const TypeGeneration>();
    TypeDeclarationStatus Status = TypeDeclarationStatus::Acceptable;
    std::shared_ptr<const TypeGeneration> Extended =
        TypeGeneration::Derive(*Foundation, BuiltInScalarTypeRecords(), Status);
    return Extended ? Extended : Foundation;
  }();
  return Shared;
}

TypeRecord
DeclareEnumerationTypeRecord(const StableTypeKey &Key, std::string PublicName,
                             std::optional<EnumerationDomain> Domain) {
  DeclarationTraits Traits;
  Traits.Read = &ReadEnumeration;
  Traits.Write = &WriteEnumeration;
  Traits.Representation = Domain && Domain->PublishesObjects
                              ? LuauRepresentation::Userdata
                              : LuauRepresentation::Number;
  Traits.Rank = ConversionRankCategory::Exact;
  Traits.Mapping = ValueKind::Integer;
  TypeRecord Record = Declare(TypeDescriptor::ForEnumeration(Key),
                              std::move(PublicName), Traits);
  Record.Enumeration = std::move(Domain);
  return Record;
}

TypeRecord DeclareClassTypeRecord(const StableTypeKey &Key,
                                  std::string PublicName) {
  DeclarationTraits Traits;
  Traits.Read = &ReadClassHandle;
  Traits.Write = &WriteClassHandle;
  Traits.Representation = LuauRepresentation::Userdata;
  Traits.Rank = ConversionRankCategory::Exact;
  return Declare(TypeDescriptor::ForClass(Key), std::move(PublicName), Traits);
}

namespace {

[[nodiscard]] StructuredReadResult
RejectConvertedRead(ConversionScope &Scope, const TypeRecord &Record, int) {
  return StructuredReadResult::Reject(
      Scope.Reject(StructuredFailure::UnavailableConversion, Record));
}

[[nodiscard]] StructuredWriteResult
RejectConvertedWrite(ConversionScope &Scope, const TypeRecord &Record,
                     const StructuredValue &) {
  return StructuredWriteResult::Reject(
      Scope.Reject(StructuredFailure::UnavailableConversion, Record));
}

} // namespace

TypeRecord DeclareConvertedTypeRecord(const StableTypeKey &Key,
                                      std::string PublicName) {
  DeclarationTraits Traits;
  Traits.Read = &RejectConvertedRead;
  Traits.Write = &RejectConvertedWrite;
  Traits.Representation = LuauRepresentation::Table;
  Traits.Rank = ConversionRankCategory::User;
  return Declare(TypeDescriptor::ForConverted(Key), std::move(PublicName),
                 Traits);
}

StructuralDeclarationStatus
DeclareStructuralTypes(const TypeGeneration &Known, const TypeDescriptor &Type,
                       std::vector<TypeRecord> &Declared,
                       TypeDescriptor &Blocking) {
  std::vector<TypeRecord> Staged = Declared;
  const StructuralDeclarationStatus Status =
      DeclareInto(Known, Type, Staged, Blocking);
  if (Status == StructuralDeclarationStatus::Declared)
    Declared = std::move(Staged);
  return Status;
}

StructuredReadResult ReadThroughRecord(ConversionScope &Scope,
                                       const TypeRecord &Record,
                                       int StackIndex) {
  if (Record.StructuredRead)
    return Record.StructuredRead(Scope, Record, StackIndex);

  lua_State *State = Scope.State();
  if (!Record.Read || !State)
    return StructuredReadResult::Reject(Internal(Scope, Record));

  if (lua_type(State, StackIndex) == LUA_TNONE)
    return StructuredReadResult::Reject(
        Scope.Reject(StructuredFailure::MissingElement, Record));

  const ArgumentReadResult Read = Record.Read(State, StackIndex);
  if (Read.IsSuccess())
    return StructuredReadResult::Accept(
        StructuredValue::Scalar(*Read.ConvertedValue));
  return StructuredReadResult::Reject(
      StructuredDiagnosticFrom(Read, Record.PublicName, Scope.Path()));
}

StructuredWriteResult WriteThroughRecord(ConversionScope &Scope,
                                         const TypeRecord &Record,
                                         const StructuredValue &Source) {
  if (Record.StructuredWrite)
    return Record.StructuredWrite(Scope, Record, Source);

  lua_State *State = Scope.State();
  const Value *Scalar = Source.ScalarValue();
  if (!Record.Write || !State || !Scalar)
    return StructuredWriteResult::Reject(Internal(Scope, Record));

  if (Record.MaximumByteCount) {
    const auto *Text = std::get_if<std::string>(Scalar);
    if (Text && Text->size() > *Record.MaximumByteCount) {
      StructuredDiagnostic Diagnostic =
          Scope.Reject(StructuredFailure::StringTooLong, Record);
      Diagnostic.ReceivedCount = Text->size();
      Diagnostic.PermittedCount = *Record.MaximumByteCount;
      return StructuredWriteResult::Reject(std::move(Diagnostic));
    }
  }
  if (!Reserve(State, 1))
    return StructuredWriteResult::Reject(Unreserved(Scope, Record, 1));
  if (!Record.Write(State, *Scalar))
    return StructuredWriteResult::Reject(Internal(Scope, Record));
  return StructuredWriteResult::Accept(1);
}

StructuredReadResult ReadStructuredValue(const TypeGeneration &Types,
                                         lua_State *State, int StackIndex,
                                         const TypeDescriptor &Type) {
  const TypeRecord *Record = Types.Find(Type);
  if (!State || !Record || !Record->IsReadable)
    return StructuredReadResult::Reject(UnavailableAt(Type));

  try {
    ConversionScope Scope(Types, State);
    return ReadThroughRecord(Scope, *Record, StackIndex);
  } catch (...) {
    return StructuredReadResult::Reject(InternalAt(Type));
  }
}

StructuredReadResult ReadArgumentPack(const TypeGeneration &Types,
                                      lua_State *State, int FirstStackIndex,
                                      int SuppliedCount,
                                      std::size_t FirstArgumentPosition,
                                      const TypeDescriptor &PackType) {
  const TypeRecord *Record = Types.Find(PackType);
  if (!State || !Record || !Record->IsReadable ||
      PackType.Kind() != TypeKind::ArgumentPack) {
    StructuredDiagnostic Diagnostic = UnavailableAt(PackType);
    Diagnostic.Position = FirstArgumentPosition;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }
  if (SuppliedCount < 0) {
    StructuredDiagnostic Diagnostic = InternalAt(PackType);
    Diagnostic.Position = FirstArgumentPosition;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }

  try {
    ConversionScope Scope(Types, State);
    const std::size_t Supplied = static_cast<std::size_t>(SuppliedCount);

    std::size_t Permitted = 0;
    if (PermittedElementCount(*Record, Permitted) && Supplied != Permitted) {
      StructuredDiagnostic Diagnostic =
          CountMismatch(Scope, *Record, Supplied, Permitted);
      Diagnostic.Position = FirstArgumentPosition;
      return StructuredReadResult::Reject(std::move(Diagnostic));
    }

    const int FirstIndex = AbsoluteIndex(State, FirstStackIndex);
    std::vector<StructuredValue> Elements;
    Elements.reserve(Supplied);
    for (std::size_t Offset = 0; Offset < Supplied; ++Offset) {
      const TypeRecord *Element = ElementRecordOf(Scope, *Record, Offset);
      if (!Element) {
        StructuredDiagnostic Diagnostic =
            Scope.Reject(StructuredFailure::UnavailableType, *Record);
        Diagnostic.Position = FirstArgumentPosition + Offset;
        return StructuredReadResult::Reject(std::move(Diagnostic));
      }

      StructuredReadResult Staged = ReadThroughRecord(
          Scope, *Element, FirstIndex + static_cast<int>(Offset));
      if (!Staged.IsSuccess()) {
        Staged.Diagnostic.Position = FirstArgumentPosition + Offset;
        return Staged;
      }
      Elements.push_back(std::move(Staged.ConvertedValue));
    }
    return StructuredReadResult::Accept(
        StructuredValue::List(std::move(Elements)));
  } catch (...) {
    StructuredDiagnostic Diagnostic = InternalAt(PackType);
    Diagnostic.Position = FirstArgumentPosition;
    return StructuredReadResult::Reject(std::move(Diagnostic));
  }
}

StructuredWriteResult WriteStructuredValue(const TypeGeneration &Types,
                                           lua_State *State,
                                           const TypeDescriptor &Type,
                                           const StructuredValue &Source) {
  const TypeRecord *Record = Types.Find(Type);
  if (!State || !Record || !Record->IsWritable)
    return StructuredWriteResult::Reject(UnavailableAt(Type));

  const int EntryDepth = lua_gettop(State);
  try {
    ConversionScope Scope(Types, State);
    StructuredWriteResult Written = WriteThroughRecord(Scope, *Record, Source);
    if (!Written.IsSuccess())
      lua_settop(State, EntryDepth);
    return Written;
  } catch (...) {
    lua_settop(State, EntryDepth);
    return StructuredWriteResult::Reject(InternalAt(Type));
  }
}

StructuredWriteResult PublishReturnShape(const TypeGeneration &Types,
                                         lua_State *State,
                                         const TypeDescriptor &Type,
                                         const StructuredValue &Source) {
  if (Type.FixedKey() == FixedTypeKey::Void)
    return StructuredWriteResult::Accept(0);

  const TypeKind Kind = Type.Kind();
  const bool IsOrderedPack = Kind == TypeKind::Pair ||
                             Kind == TypeKind::Tuple ||
                             Kind == TypeKind::ReturnPack;
  if (!IsOrderedPack)
    return WriteStructuredValue(Types, State, Type, Source);

  const TypeRecord *Record = Types.Find(Type);
  if (!State || !Record || !Record->IsWritable)
    return StructuredWriteResult::Reject(UnavailableAt(Type));

  const int EntryDepth = lua_gettop(State);
  try {
    ConversionScope Scope(Types, State);
    if (Source.Kind() != StructuredKind::List)
      return StructuredWriteResult::Reject(Internal(Scope, *Record));

    const std::size_t Count = Source.Size();
    std::size_t Permitted = 0;
    if (PermittedElementCount(*Record, Permitted) && Count != Permitted)
      return StructuredWriteResult::Reject(
          CountMismatch(Scope, *Record, Count, Permitted));
    if (Count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      return StructuredWriteResult::Reject(CountMismatch(
          Scope, *Record, Count,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
    for (std::size_t Position = 0; Position < Count; ++Position) {
      if (!ElementRecordOf(Scope, *Record, Position))
        return StructuredWriteResult::Reject(
            Scope.Reject(StructuredFailure::UnavailableType, *Record));
    }
    if (!Reserve(State, static_cast<int>(Count) + 1))
      return StructuredWriteResult::Reject(
          Unreserved(Scope, *Record, Count + 1));

    for (std::size_t Position = 0; Position < Count; ++Position) {
      const TypeRecord *Element = ElementRecordOf(Scope, *Record, Position);
      const StructuredValue *Staged = Source.ElementAt(Position);
      if (!Element || !Staged) {
        lua_settop(State, EntryDepth);
        return StructuredWriteResult::Reject(Internal(Scope, *Record));
      }

      StructuredWriteResult Written =
          WriteThroughRecord(Scope, *Element, *Staged);
      if (!Written.IsSuccess() || Written.PublishedCount != 1) {
        lua_settop(State, EntryDepth);
        if (Written.IsSuccess())
          return StructuredWriteResult::Reject(Internal(Scope, *Record));
        Written.Diagnostic.Position = Position + 1;
        return Written;
      }
    }
    return StructuredWriteResult::Accept(static_cast<int>(Count));
  } catch (...) {
    lua_settop(State, EntryDepth);
    return StructuredWriteResult::Reject(InternalAt(Type));
  }
}

} // namespace Luna::Detail
