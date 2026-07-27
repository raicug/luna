#pragma once

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/storage.hpp"

#include <vector>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] TypeDescriptor CanonicalReturnType(const ReturnMetadata &Return);

[[nodiscard]] std::vector<TypeDescriptor>
PublishedReturnTypes(const TypeDescriptor &ReturnType);

[[nodiscard]] ReturnShape ReflectedReturnShapeOf(const ReturnMetadata &Return);

[[nodiscard]] std::vector<ReflectionReturnFields>
MakeReflectedReturnFields(const ReturnMetadata &Return);

} // namespace Luna::Detail
