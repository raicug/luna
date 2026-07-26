#pragma once

// The registered classes of one logical State.
//
// Each registered native type owns exactly one canonical `TypeId`, one class
// symbol, and one cached metatable identity per logical State. This registry is
// where that per-State half lives: publication records it, and every later
// exposure, access validation, and cleanup step resolves it from here instead
// of deriving it again.
//
// The registry is written only by publication, so a rolled-back attempt leaves
// no registered class behind. Metatable identities are allocated in canonical
// order from a state-local counter that never restarts, so an identity is never
// recycled inside one State and never leaks a virtual-machine or native
// address. The virtual-machine metatable itself is created lazily by the
// exposure path and retained here, which is why the table slot is part of the
// record rather than of the plan.

// clang-format off
#include <luna/binding/class_builder.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/class_operators.hpp"
#include "state/userdata/class_relationships.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/member_access.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// One declaration owned by a registered class. Typed accessors additionally
// live in `RegisteredClass::Members`; this compact declaration list includes
// methods as well so inherited views never clone or lose their original symbol.
struct RegisteredClassDeclaration final {
  std::string Segment;
  SymbolKind Kind = SymbolKind::Property;
  SymbolId Declaration;
};

// One registered class of one logical State.
struct RegisteredClass final {
  StateIdentity Origin;
  SymbolId ClassSymbol;
  TypeId Type;
  StableTypeKey Key;
  std::string QualifiedName;
  MetatableId Metatable;

  // The declared C++ shape the consumer's translation unit captured. Luna keeps
  // it because the backend never sees the consumer's type.
  ClassPolicy Policy;

  std::uint64_t LifecycleGeneration = 0;

  // The Luna-owned metatable of this class and the protected reference that
  // keeps it alive. Both stay empty until the first exposure creates the table,
  // so registration never installs a virtual-machine value for a class that no
  // value was ever created of.
  const void *Table = nullptr;
  int Reference = 0;

  // How many times the virtual-machine metatable of this class was created. It
  // is one for every class one value was ever exposed of, whichever exposure
  // path created the first value, because the table is retained here for the
  // life of the State rather than made again per value.
  std::uint64_t MetatableCreations = 0;

  // The published properties and fields of this class, in canonical declaration
  // order. Each one holds the two generated descriptors its declaration
  // produced; none of them holds a native object or a cached value.
  std::vector<RegisteredMember> Members;

  // All declarations owned by this class that can appear through inheritance.
  // Every entry retains the original declaration identity; no derived class
  // receives a cloned record.
  std::vector<RegisteredClassDeclaration> Declarations;

  // The published operators of this class. Each one names the Luna-owned member
  // segment its candidate is published under rather than holding the candidate,
  // so a later registration can move every record without invalidating an
  // installed metamethod.
  std::vector<RegisteredOperator> Operators;

  // The member one name resolves to, or null when this class declares none.
  [[nodiscard]] const RegisteredMember *
  FindMember(std::string_view Segment) const noexcept;

  // The operator one selection resolves to, or null when this class declares
  // none.
  [[nodiscard]] const RegisteredOperator *
  FindOperator(ClassOperator Selected) const noexcept;

  [[nodiscard]] bool IsComplete() const noexcept;
};

// One accessible base of a registered class, as canonical enumeration reports
// it.
struct ClassBaseView final {
  TypeId Base;
  std::string QualifiedName;
  bool IsDirect = false;
  bool IsAccessible = true;
};

// One registered safe downcast into a class.
struct ClassCastView final {
  TypeId Source;
  std::string QualifiedName;
  std::string Policy;
  bool UsesRuntimeTypeAssistance = false;
};

// One member a class inherits rather than declares. It retains the declaring
// class and the original member's own symbol identity instead of copying the
// declaration, and it reports an inherited name reachable from more than one
// base as ambiguous instead of selecting one of them.
struct ClassInheritedMemberView final {
  std::string Segment;
  SymbolKind Kind = SymbolKind::Property;
  TypeId DeclaringClass;
  std::string DeclaringClassName;
  SymbolId Declaration;
  bool IsAmbiguous = false;
};

class ClassRegistry final {
public:
  // The next state-local metatable identity. It is monotonic and never zero, so
  // a default-constructed identity always means "no metatable".
  [[nodiscard]] MetatableId AllocateMetatableIdentity() noexcept;

  // Publication is the only caller: a class becomes registered only once its
  // transaction published it. Recording the same qualified name twice replaces
  // the record, which is what a later compatible replacement will need.
  void Record(RegisteredClass Registered);

  [[nodiscard]] const RegisteredClass *
  Find(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] const RegisteredClass *Find(const TypeId &Type) const noexcept;
  [[nodiscard]] const RegisteredClass *
  FindBySymbol(const SymbolId &ClassSymbol) const noexcept;

  // The mutable record of one registered class, for the exposure path that
  // creates and retains the class metatable lazily.
  [[nodiscard]] RegisteredClass *FindForUpdate(const TypeId &Type) noexcept;

  // The mutable record of one registered class named by its class symbol, for
  // publication attaching the members that class declared.
  [[nodiscard]] RegisteredClass *
  FindForUpdate(const SymbolId &ClassSymbol) noexcept;

  [[nodiscard]] std::span<const RegisteredClass> Registered() const noexcept {
    return Records;
  }

  // The explicit relationship graph of this State. It is written only by
  // publication and read by every access that adjusts a receiver or an
  // argument.
  [[nodiscard]] const ClassRelationships &Relationships() const noexcept {
    return Graph;
  }

  [[nodiscard]] ClassRelationships &RelationshipsForUpdate() noexcept {
    return Graph;
  }

  // Canonical enumeration of the class surface one registered class inherits or
  // declares as a relationship. Every list is ordered by canonical qualified
  // name and member name, never by the order the declarations were made in.
  [[nodiscard]] std::vector<ClassBaseView> BasesOf(const TypeId &Type) const;
  [[nodiscard]] std::vector<ClassCastView> CastsOf(const TypeId &Type) const;
  [[nodiscard]] std::vector<ClassInheritedMemberView>
  InheritedMembersOf(const TypeId &Type) const;

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }

  [[nodiscard]] std::uint64_t IssuedMetatableIdentities() const noexcept {
    return NextMetatableValue;
  }

  // True when `Registered` describes exactly the requested registered class of
  // this State.
  [[nodiscard]] static bool Matches(const RegisteredClass &Registered,
                                    const StateIdentity &Origin,
                                    const TypeId &Type,
                                    const SymbolId &ClassSymbol) noexcept;

  // True when the registration belongs to the lifecycle generation the caller
  // captured. A class published by a replaced generation is stale.
  [[nodiscard]] static bool
  IsCurrent(const RegisteredClass &Registered,
            std::uint64_t LifecycleGeneration) noexcept;

private:
  std::vector<RegisteredClass> Records;
  ClassRelationships Graph;
  std::uint64_t NextMetatableValue = 0;
};

} // namespace Luna::Detail
