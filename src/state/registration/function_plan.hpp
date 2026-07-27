#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>

#include "state/reflection/storage.hpp"

#include <memory>
#include <string>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct StagedFunction final {
  std::string Segment;
  std::string QualifiedName;
  std::shared_ptr<ErasedCallableDescriptor> Callable;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

} // namespace Luna::Detail
