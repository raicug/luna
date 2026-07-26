#pragma once

// One immutable canonical type record. A record is what the registry knows
// about exactly one normalized type: its stable `TypeId`, its complete
// canonical descriptor, the public name diagnostics use, the Luau
// representation it reads and writes, whether it accepts nil, which directions
// it supports, the nested types it is built from, its conversion rank category,
// and the committing reader and writer themselves.
//
// A record is a value: it owns its strings and its nested identities, holds no
// virtual-machine resource, and can therefore be copied into an immutable type
// generation, captured by one invocation, and outlive the attempt that declared
// it. Converters are named through an opaque virtual-machine forward
// declaration, so no Luau type reaches this header.

// clang-format off
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

// How one canonical type is represented on the Luau side. `None` belongs to
// `void`, which is neither readable nor writable as a value.
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

// Conversion rank category of one type. The foundation types are all exact
// conversions; safe built-in and user categories arrive with the wider
// converters and only ever participate as ordered Pareto dimensions.
enum class ConversionRankCategory { Exact, SafeBuiltIn, User };

[[nodiscard]] std::string_view
ConversionRankCategoryText(ConversionRankCategory Rank) noexcept;

// The committing reader of one type: it inspects the value at a stack position
// and either converts it or reports the first deterministic rejection.
using TypeReadFunction = ArgumentReadResult (*)(lua_State *State,
                                                int StackIndex);

// The committing writer of one type: it publishes exactly one value and reports
// whether publication succeeded.
using TypeWriteFunction = bool (*)(lua_State *State, const Value &Source);

// The declared enumerator domain of one registered enumeration. It is part of
// the canonical type declaration, so the committing converters can reject a
// value the enumeration never declared instead of narrowing or truncating it,
// and two declarations of one enumeration that disagree about their domain are
// an incompatible duplicate rather than a silent overwrite.
struct EnumerationDomain final {
  // Canonical enumerator values in ascending order, without duplicates. Aliases
  // name an existing canonical value and therefore add nothing here.
  std::vector<std::int64_t> Values;

  // Bitflag behavior is never inferred: it is declared explicitly.
  bool IsBitflags = false;

  // The declared supported-bit mask, meaningful only for a bitflag enumeration.
  // Every accepted value is a subset of it; a value carrying any other bit is
  // rejected whole.
  std::int64_t SupportedBits = 0;

  // The value is one the enumeration declared. A bitflag enumeration also
  // accepts any combination of its declared supported bits, including none.
  [[nodiscard]] bool Accepts(std::int64_t Candidate) const noexcept;

  [[nodiscard]] friend bool
  operator==(const EnumerationDomain &Left,
             const EnumerationDomain &Right) = default;
};

struct TypeRecord final {
  TypeId Identity;
  TypeDescriptor Descriptor;

  // Public name of the type. Diagnostics use it verbatim, so the foundation
  // names remain exactly what the foundation reported.
  std::string PublicName;

  LuauRepresentation Representation = LuauRepresentation::None;

  // The type accepts nil as one of its own values.
  bool IsNullable = false;

  // Supported conversion directions.
  bool IsReadable = false;
  bool IsWritable = false;

  // Ordered nested types this type is built from. A foundation leaf has none.
  std::vector<TypeId> NestedTypes;

  ConversionRankCategory Rank = ConversionRankCategory::Exact;

  // The foundation value kind this type maps to, when it has one. `void` and
  // the structural types of later milestones have none.
  std::optional<ValueKind> ValueRepresentation;

  // Explicit Luna-owned size policy of the type, in bytes, when it has one.
  std::optional<std::size_t> MaximumByteCount;

  // The declared enumerator domain, present only for a registered enumeration
  // whose enumerator list is known. An enumeration declared without one accepts
  // the whole integral domain its representation describes.
  std::optional<EnumerationDomain> Enumeration;

  TypeReadFunction Read = nullptr;
  TypeWriteFunction Write = nullptr;

  // The recursive converters of one structural type. A type supplies either the
  // scalar pair above or this pair, never a mixture: a scalar foundation leaf
  // moves one `Value`, while an aggregate moves one complete `StructuredValue`
  // and can report the nested path of its first deterministic failure.
  StructuredReadFunction StructuredRead = nullptr;
  StructuredWriteFunction StructuredWrite = nullptr;

  // The type converts through the recursive structural converters.
  [[nodiscard]] bool IsStructural() const noexcept;

  // A record describes the canonical void type.
  [[nodiscard]] bool IsVoid() const noexcept;

  // A record is complete when it identifies one valid canonical descriptor,
  // names itself, and supplies a converter for every direction it claims.
  [[nodiscard]] bool IsComplete() const;
};

// Two declarations of one canonical type supply the same converters.
[[nodiscard]] bool HasSameConverters(const TypeRecord &Left,
                                     const TypeRecord &Right) noexcept;

// Two declarations of one canonical type agree on everything a consumer can
// observe: identity, descriptor, public name, representation, nullability,
// directions, nested types, rank, value mapping, size policy, and converters.
[[nodiscard]] bool HasSameDeclaration(const TypeRecord &Left,
                                      const TypeRecord &Right);

// Canonical order of two type records: descriptor first, identity last. It
// never depends on declaration order.
[[nodiscard]] bool TypeRecordPrecedes(const TypeRecord &Left,
                                      const TypeRecord &Right);

// Canonical mapping from one foundation value kind to its canonical type. Every
// caller that needs this mapping - the descriptor plan, registration
// validation, and the migrated converters - reads it here.
[[nodiscard]] TypeDescriptor CanonicalValueType(ValueKind Kind) noexcept;

// Canonical text of one type, used only in diagnostics.
[[nodiscard]] std::string CanonicalTypeText(const TypeDescriptor &Type);

} // namespace Luna::Detail
