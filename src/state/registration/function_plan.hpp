#pragma once

// Function declarations of one builder plan.
//
// A scoped function is one reflected callable candidate plus one Luna-owned
// closure at an exact canonical path. It carries no schema of its own: the
// canonical descriptor of a scoped function is built by exactly the same
// `MakeFunctionPlanEntry` a root-scope `Register` uses, with the parent scope
// identity of its namespace, so both spellings share one canonical plan entry,
// one validation path, one installation path, and one rollback journal.
//
// The staged callable is held through a shared owner because a builder plan is
// read immutably while it is submitted, and the erased callable is moved into
// its canonical plan entry exactly once at that point.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>

#include "state/reflection/storage.hpp"

#include <memory>
#include <string>
#include <vector>
// clang-format on

namespace Luna::Detail {

// One staged function of a builder plan: the validated identifier segment the
// consumer asked for, the canonical qualified name it resolves to, and the
// erased callable the plan will hand to its transaction.
struct StagedFunction final {
  std::string Segment;
  std::string QualifiedName;
  std::shared_ptr<ErasedCallableDescriptor> Callable;

  // The declared documentation surface of this candidate. It travels with the
  // declaration rather than with the canonical descriptor, because the
  // descriptor of a scoped function is byte-for-byte the one a root-scope
  // request produces.
  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

} // namespace Luna::Detail
