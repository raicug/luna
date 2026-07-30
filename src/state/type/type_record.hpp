#pragma once

// clang-format off
#include <luna/binding/delegate.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/conversion_outcome.hpp"
#include "state/type/structured_conversion.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

enum class LuauRepresentation {
  None,
  Nil,
  Boolean,
  Number,
  String,
  Table,
  Userdata,
  Function
};

[[nodiscard]] std::string_view
LuauRepresentationText(LuauRepresentation Representation) noexcept;

enum class ConversionRankCategory { Exact, SafeBuiltIn, User };

[[nodiscard]] std::string_view
ConversionRankCategoryText(ConversionRankCategory Rank) noexcept;

using TypeReadFunction = ArgumentReadResult (*)(lua_State *State,
                                                int StackIndex);

using TypeWriteFunction = bool (*)(lua_State *State, const Value &Source);

struct EnumerationDomain final {
  std::vector<std::int64_t> Values;

  // The canonical enumerator name of each value, in the same order. Only an
  // enumeration that publishes objects needs them, since an enumerator
  // object names itself; a numeric enumeration leaves them empty.
  std::vector<std::string> Names;

  bool IsBitflags = false;

  // Set when each enumerator reaches a script as one interned enumerator
  // object rather than as its bare number.
  bool PublishesObjects = false;

  std::int64_t SupportedBits = 0;

  [[nodiscard]] bool Accepts(std::int64_t Candidate) const noexcept;

  [[nodiscard]] std::string_view NameOf(std::int64_t Candidate) const noexcept;

  [[nodiscard]] friend bool
  operator==(const EnumerationDomain &Left,
             const EnumerationDomain &Right) = default;
};

struct TypeRecord final {
  TypeId Identity;
  TypeDescriptor Descriptor;

  std::string PublicName;

  LuauRepresentation Representation = LuauRepresentation::None;

  bool IsNullable = false;

  bool IsReadable = false;
  bool IsWritable = false;

  std::vector<TypeId> NestedTypes;

  ConversionRankCategory Rank = ConversionRankCategory::Exact;

  std::optional<ValueKind> ValueRepresentation;

  std::optional<std::size_t> MaximumByteCount;

  std::optional<EnumerationDomain> Enumeration;

  TypeReadFunction Read = nullptr;
  TypeWriteFunction Write = nullptr;

  StructuredReadFunction StructuredRead = nullptr;
  StructuredWriteFunction StructuredWrite = nullptr;

  [[nodiscard]] bool IsStructural() const noexcept;

  [[nodiscard]] bool IsVoid() const noexcept;

  [[nodiscard]] bool IsComplete() const;
};

[[nodiscard]] bool HasSameConverters(const TypeRecord &Left,
                                     const TypeRecord &Right) noexcept;

[[nodiscard]] bool HasSameDeclaration(const TypeRecord &Left,
                                      const TypeRecord &Right);

[[nodiscard]] bool TypeRecordPrecedes(const TypeRecord &Left,
                                      const TypeRecord &Right);

[[nodiscard]] TypeDescriptor CanonicalValueType(ValueKind Kind) noexcept;

// The canonical type of one delegate parameter: a callable whose first child
// is its result type and whose remaining children are its parameter types.
[[nodiscard]] TypeDescriptor
CanonicalDelegateType(const DelegateShape &Declared);

[[nodiscard]] bool IsCanonicalDelegateType(const TypeDescriptor &Type) noexcept;

[[nodiscard]] std::string CanonicalTypeText(const TypeDescriptor &Type);

} // namespace Luna::Detail
