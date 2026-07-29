#pragma once

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

[[nodiscard]] std::string StructuralPublicName(const TypeDescriptor &Type);

[[nodiscard]] std::vector<TypeRecord> BuiltInScalarTypeRecords();

[[nodiscard]] std::shared_ptr<const TypeGeneration> BuiltInTypeGeneration();

[[nodiscard]] TypeRecord
DeclareEnumerationTypeRecord(const StableTypeKey &Key, std::string PublicName,
                             std::optional<EnumerationDomain> Domain = {});

[[nodiscard]] TypeRecord DeclareClassTypeRecord(const StableTypeKey &Key,
                                                std::string PublicName);

// A user-defined leaf converted through `Luna::TypeConverter<T>`. The actual
// stack conversion happens inside the declaring member's own
// `MemberConvertedReadOperation`/`MemberConvertedWriteOperation` closures,
// which already know the concrete C++ type; this record exists so the leaf
// has a complete, idempotently redeclarable `TypeRecord` for identity and
// availability checks, but its own Read/Write are never actually reached by
// a member access.
[[nodiscard]] TypeRecord DeclareConvertedTypeRecord(const StableTypeKey &Key,
                                                    std::string PublicName);

enum class StructuralDeclarationStatus {
  Declared,
  UnsupportedDescriptor,
  UnavailableLeaf
};

[[nodiscard]] std::string_view
StructuralDeclarationStatusText(StructuralDeclarationStatus Status) noexcept;

[[nodiscard]] StructuralDeclarationStatus
DeclareStructuralTypes(const TypeGeneration &Known, const TypeDescriptor &Type,
                       std::vector<TypeRecord> &Declared,
                       TypeDescriptor &Blocking);

[[nodiscard]] StructuredReadResult ReadThroughRecord(ConversionScope &Scope,
                                                     const TypeRecord &Record,
                                                     int StackIndex);

[[nodiscard]] StructuredWriteResult
WriteThroughRecord(ConversionScope &Scope, const TypeRecord &Record,
                   const StructuredValue &Source);

[[nodiscard]] StructuredReadResult
ReadStructuredValue(const TypeGeneration &Types, lua_State *State,
                    int StackIndex, const TypeDescriptor &Type);

[[nodiscard]] StructuredReadResult
ReadArgumentPack(const TypeGeneration &Types, lua_State *State,
                 int FirstStackIndex, int SuppliedCount,
                 std::size_t FirstArgumentPosition,
                 const TypeDescriptor &PackType);

[[nodiscard]] StructuredWriteResult
WriteStructuredValue(const TypeGeneration &Types, lua_State *State,
                     const TypeDescriptor &Type, const StructuredValue &Source);

[[nodiscard]] StructuredWriteResult
PublishReturnShape(const TypeGeneration &Types, lua_State *State,
                   const TypeDescriptor &Type, const StructuredValue &Source);

} // namespace Luna::Detail
