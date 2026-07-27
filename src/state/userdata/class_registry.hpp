#pragma once

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

struct RegisteredClassDeclaration final {
  std::string Segment;
  SymbolKind Kind = SymbolKind::Property;
  SymbolId Declaration;
};

struct RegisteredClass final {
  StateIdentity Origin;
  SymbolId ClassSymbol;
  TypeId Type;
  StableTypeKey Key;
  std::string QualifiedName;
  MetatableId Metatable;

  ClassPolicy Policy;

  std::uint64_t LifecycleGeneration = 0;

  const void *Table = nullptr;
  int Reference = 0;

  std::uint64_t MetatableCreations = 0;

  std::vector<RegisteredMember> Members;

  std::vector<RegisteredClassDeclaration> Declarations;

  std::vector<RegisteredOperator> Operators;

  [[nodiscard]] const RegisteredMember *
  FindMember(std::string_view Segment) const noexcept;

  [[nodiscard]] const RegisteredOperator *
  FindOperator(ClassOperator Selected) const noexcept;

  [[nodiscard]] bool IsComplete() const noexcept;
};

struct ClassBaseView final {
  TypeId Base;
  std::string QualifiedName;
  bool IsDirect = false;
  bool IsAccessible = true;
};

struct ClassCastView final {
  TypeId Source;
  std::string QualifiedName;
  std::string Policy;
  bool UsesRuntimeTypeAssistance = false;
};

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
  [[nodiscard]] MetatableId AllocateMetatableIdentity() noexcept;

  void Record(RegisteredClass Registered);

  [[nodiscard]] const RegisteredClass *
  Find(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] const RegisteredClass *Find(const TypeId &Type) const noexcept;
  [[nodiscard]] const RegisteredClass *
  FindBySymbol(const SymbolId &ClassSymbol) const noexcept;

  [[nodiscard]] RegisteredClass *FindForUpdate(const TypeId &Type) noexcept;

  [[nodiscard]] RegisteredClass *
  FindForUpdate(const SymbolId &ClassSymbol) noexcept;

  [[nodiscard]] std::span<const RegisteredClass> Registered() const noexcept {
    return Records;
  }

  [[nodiscard]] const ClassRelationships &Relationships() const noexcept {
    return Graph;
  }

  [[nodiscard]] ClassRelationships &RelationshipsForUpdate() noexcept {
    return Graph;
  }

  [[nodiscard]] std::vector<ClassBaseView> BasesOf(const TypeId &Type) const;
  [[nodiscard]] std::vector<ClassCastView> CastsOf(const TypeId &Type) const;
  [[nodiscard]] std::vector<ClassInheritedMemberView>
  InheritedMembersOf(const TypeId &Type) const;

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }

  [[nodiscard]] std::uint64_t IssuedMetatableIdentities() const noexcept {
    return NextMetatableValue;
  }

  [[nodiscard]] static bool Matches(const RegisteredClass &Registered,
                                    const StateIdentity &Origin,
                                    const TypeId &Type,
                                    const SymbolId &ClassSymbol) noexcept;

  [[nodiscard]] static bool
  IsCurrent(const RegisteredClass &Registered,
            std::uint64_t LifecycleGeneration) noexcept;

private:
  std::vector<RegisteredClass> Records;
  ClassRelationships Graph;
  std::uint64_t NextMetatableValue = 0;
};

} // namespace Luna::Detail
