#pragma once

// The registration half of one declared return shape.
//
// A callable produces zero values, exactly one value, or ordered multiple
// values, and every one of those shapes is one canonical type here:
//
//   * `void` and a suppressed return are `luna.void`, and publish nothing.
//   * One supported scalar is that scalar's canonical type.
//   * A returned `std::pair` or `std::tuple` is one canonical return pack whose
//     ordered children are the declared element types.
//   * A returned `Luna::ReturnPack` is the canonical owning value pack: its
//     element count and element types are decided by the invocation, so the
//     signature fixes neither.
//
// Availability follows the same split. A pack publishes one value per ordered
// element rather than one aggregate, so what registration must find in the
// captured type generation is every element type, not the pack itself; a
// dynamic pack names its element types only at publication and is validated
// there against the same generation the invocation captured.

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/storage.hpp"

#include <vector>
// clang-format on

namespace Luna::Detail {

// The canonical type of one declared return shape.
[[nodiscard]] TypeDescriptor CanonicalReturnType(const ReturnMetadata &Return);

// Every canonical type one return shape publishes: a scalar names itself, an
// ordered pack names its element types, and `void` or a dynamic pack names
// none.
[[nodiscard]] std::vector<TypeDescriptor>
PublishedReturnTypes(const TypeDescriptor &ReturnType);

// The reflected return shape: zero values, one scalar value, or ordered
// multiple values.
[[nodiscard]] ReturnShape ReflectedReturnShapeOf(const ReturnMetadata &Return);

// The reflected returned values of one shape, in return order. A dynamic pack
// declares no element records, because its elements are decided per call.
[[nodiscard]] std::vector<ReflectionReturnFields>
MakeReflectedReturnFields(const ReturnMetadata &Return);

} // namespace Luna::Detail
