#pragma once

// The planned built-in and structural canonical types. Everything the
// foundation could not describe lives here: `float`, `std::string_view`, C
// strings, null, `std::optional`, sequence containers, fixed arrays,
// associative maps, pairs, tuples, enumerations, registered classes, argument
// packs, and return packs.
//
// Every structural type is an ordinary `TypeRecord` in an ordinary type
// generation. Its converters recurse through the same registry, which is what
// makes one nested failure report the callable or member, the one-based
// argument or return position, and the complete element or key path -
// `argument 2[4].Key` - and lets a writer stage a whole aggregate before
// publishing it, so a rejected conversion exposes neither a partial table nor a
// partial native value.
//
// No arbitrary bound is introduced. A tuple, pack, sequence, or map accepts any
// element count, nesting has no configured depth, and the only Luna-owned size
// policy is the inherited foundation string limit. The one remaining bound is
// the virtual-machine stack capacity a publication must actually reserve, and a
// failure to reserve it reports the count it needed.

// clang-format off
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Canonical descriptor constructors. They only name the canonical model, so
// every caller builds the same descriptor for the same shape.
[[nodiscard]] TypeDescriptor OptionalTypeOf(TypeDescriptor Inner);
[[nodiscard]] TypeDescriptor SequenceTypeOf(TypeDescriptor Element);
[[nodiscard]] TypeDescriptor FixedArrayTypeOf(TypeDescriptor Element,
                                              std::size_t Extent);
[[nodiscard]] TypeDescriptor MapTypeOf(TypeDescriptor Key,
                                       TypeDescriptor Mapped);
[[nodiscard]] TypeDescriptor PairTypeOf(TypeDescriptor First,
                                        TypeDescriptor Second);
[[nodiscard]] TypeDescriptor TupleTypeOf(std::vector<TypeDescriptor> Elements);
[[nodiscard]] TypeDescriptor
ArgumentPackTypeOf(std::vector<TypeDescriptor> Elements);
[[nodiscard]] TypeDescriptor
ReturnPackTypeOf(std::vector<TypeDescriptor> Elements);

// Public name of one canonical type, composed from its own structure. An
// enumeration or class leaf is named by its validated stable key unless its own
// milestone declared a name for it.
[[nodiscard]] std::string StructuralPublicName(const TypeDescriptor &Type);

// The planned built-in scalars beyond the foundation five: `float`,
// `std::string_view`, a C string, and null.
[[nodiscard]] std::vector<TypeRecord> BuiltInScalarTypeRecords();

// The foundation extended by the planned built-in scalars. Conversion call
// sites that are not inside one captured invocation read it here.
[[nodiscard]] std::shared_ptr<const TypeGeneration> BuiltInTypeGeneration();

// One registered enumeration. Its converter accepts the integral domain the
// foundation integer does, narrowed to the declared enumerator domain when the
// registration supplies one: a value outside the declared enumerators, or a
// bitflag value carrying an unsupported bit, is rejected whole instead of being
// narrowed or truncated. An enumeration declared without a domain accepts the
// whole integral domain, which is what a nested aggregate of the conversion
// milestone relies on.
[[nodiscard]] TypeRecord
DeclareEnumerationTypeRecord(const StableTypeKey &Key, std::string PublicName,
                             std::optional<EnumerationDomain> Domain = {});

// One registered class. Its canonical descriptor, identity, nested types, and
// userdata representation are complete here. Reading one produces a validated
// native handle: the value's layout, origin State, metatable identity,
// lifetime, dynamic type, and view permission are all checked, in that order,
// before any native pointer exists. Writing one - exposing a native object as a
// value - arrives with the ownership half of the userdata milestone.
[[nodiscard]] TypeRecord DeclareClassTypeRecord(const StableTypeKey &Key,
                                                std::string PublicName);

enum class StructuralDeclarationStatus {
  Declared,
  // The descriptor is not a valid canonical type.
  UnsupportedDescriptor,
  // One leaf has no declaration in this registry: an unregistered class or
  // enumeration, or a constructor whose own milestone owns it.
  UnavailableLeaf
};

[[nodiscard]] std::string_view
StructuralDeclarationStatusText(StructuralDeclarationStatus Status) noexcept;

// Declares every record `Type` needs that `Known` does not already describe,
// children before parents, appending them to `Declared`. Records already in
// `Declared` count as known, so a caller can pre-declare its enumeration and
// class leaves and then let one call complete the aggregates built on them. On
// refusal `Blocking` names the descriptor that could not be declared and
// `Declared` is left untouched.
[[nodiscard]] StructuralDeclarationStatus
DeclareStructuralTypes(const TypeGeneration &Known, const TypeDescriptor &Type,
                       std::vector<TypeRecord> &Declared,
                       TypeDescriptor &Blocking);

// Runs the committing reader of one record, whether it is scalar or structural.
// A nested foundation leaf keeps the exact foundation rejection wording.
[[nodiscard]] StructuredReadResult ReadThroughRecord(ConversionScope &Scope,
                                                     const TypeRecord &Record,
                                                     int StackIndex);

// Runs the committing writer of one record. Every resource the publication
// needs is reserved and every nested element is validated first.
[[nodiscard]] StructuredWriteResult
WriteThroughRecord(ConversionScope &Scope, const TypeRecord &Record,
                   const StructuredValue &Source);

// Reads one value of the canonical type `Type` at `StackIndex`.
[[nodiscard]] StructuredReadResult
ReadStructuredValue(const TypeGeneration &Types, lua_State *State,
                    int StackIndex, const TypeDescriptor &Type);

// Reads one argument pack from consecutive call positions. A pack with one
// child accepts any supplied count and converts each position as that child; a
// pack with several children is positional and requires exactly that many. The
// first deterministic failure carries the one-based call argument position and
// the nested path inside that argument.
[[nodiscard]] StructuredReadResult
ReadArgumentPack(const TypeGeneration &Types, lua_State *State,
                 int FirstStackIndex, int SuppliedCount,
                 std::size_t FirstArgumentPosition,
                 const TypeDescriptor &PackType);

// Publishes one staged value of the canonical type `Type` as exactly one Luau
// value.
[[nodiscard]] StructuredWriteResult
WriteStructuredValue(const TypeGeneration &Types, lua_State *State,
                     const TypeDescriptor &Type, const StructuredValue &Source);

// Publishes one return shape: `void` produces zero values, a scalar or one
// aggregate table produces one, and a root pair, tuple, or return pack produces
// its ordered elements. Every element is staged and validated and all stack
// capacity is reserved before the first value is published, so a failure
// publishes nothing.
[[nodiscard]] StructuredWriteResult
PublishReturnShape(const TypeGeneration &Types, lua_State *State,
                   const TypeDescriptor &Type, const StructuredValue &Source);

} // namespace Luna::Detail
