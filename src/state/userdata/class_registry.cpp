// clang-format off
#include "state/userdata/class_registry.hpp"

#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

bool RegisteredClass::IsComplete() const noexcept {
  return Origin.IsValid() && ClassSymbol.IsValid() && Type.IsValid() &&
         Key.IsValid() && !QualifiedName.empty() && Metatable.IsValid() &&
         Policy.ByteCount != 0 && Policy.Alignment != 0;
}

const RegisteredMember *
RegisteredClass::FindMember(std::string_view Segment) const noexcept {
  for (const RegisteredMember &Declared : Members) {
    if (Declared.Segment == Segment)
      return &Declared;
  }
  return nullptr;
}

const RegisteredOperator *
RegisteredClass::FindOperator(ClassOperator Selected) const noexcept {
  for (const RegisteredOperator &Declared : Operators) {
    if (Declared.Selected == Selected)
      return &Declared;
  }
  return nullptr;
}

std::vector<ClassBaseView> ClassRegistry::BasesOf(const TypeId &Type) const {
  std::vector<ClassBaseView> Enumerated;
  for (const TypeId &Base : Graph.AccessibleBases(Type)) {
    const RegisteredClass *Declared = Find(Base);
    if (Declared == nullptr)
      continue;
    ClassBaseView View;
    View.Base = Base;
    View.QualifiedName = Declared->QualifiedName;
    View.IsDirect = Graph.IsDirectBase(Type, Base);
    Enumerated.push_back(std::move(View));
  }
  std::sort(Enumerated.begin(), Enumerated.end(),
            [](const ClassBaseView &Left, const ClassBaseView &Right) {
              return Left.QualifiedName < Right.QualifiedName;
            });
  return Enumerated;
}

std::vector<ClassCastView> ClassRegistry::CastsOf(const TypeId &Type) const {
  std::vector<ClassCastView> Enumerated;
  if (!Type.IsValid())
    return Enumerated;
  for (const ClassCastEdge &Edge : Graph.CastEdges()) {
    if (!(Edge.Target == Type))
      continue;
    const RegisteredClass *Declared = Find(Edge.Source);
    if (Declared == nullptr)
      continue;
    ClassCastView View;
    View.Source = Edge.Source;
    View.QualifiedName = Declared->QualifiedName;
    View.Policy = Edge.Policy;
    View.UsesRuntimeTypeAssistance = Edge.UsesRuntimeTypeAssistance;
    Enumerated.push_back(std::move(View));
  }
  std::sort(Enumerated.begin(), Enumerated.end(),
            [](const ClassCastView &Left, const ClassCastView &Right) {
              if (Left.QualifiedName != Right.QualifiedName)
                return Left.QualifiedName < Right.QualifiedName;
              return Left.Policy < Right.Policy;
            });
  return Enumerated;
}

std::vector<ClassInheritedMemberView>
ClassRegistry::InheritedMembersOf(const TypeId &Type) const {
  std::vector<ClassInheritedMemberView> Enumerated;
  const RegisteredClass *Derived = Find(Type);
  if (Derived == nullptr)
    return Enumerated;

  for (const ClassBaseView &Base : BasesOf(Type)) {
    const RegisteredClass *Declaring = Find(Base.Base);
    if (Declaring == nullptr)
      continue;
    for (const RegisteredClassDeclaration &Member : Declaring->Declarations) {
      // A name the derived class declares itself is owned by that declaration,
      // so it is never reported as inherited.
      bool DeclaredHere = false;
      for (const RegisteredClassDeclaration &Own : Derived->Declarations)
        DeclaredHere = DeclaredHere || Own.Segment == Member.Segment;
      if (DeclaredHere)
        continue;
      ClassInheritedMemberView View;
      View.Segment = Member.Segment;
      View.Kind = Member.Kind;
      View.DeclaringClass = Declaring->Type;
      View.DeclaringClassName = Declaring->QualifiedName;
      View.Declaration = Member.Declaration;
      Enumerated.push_back(std::move(View));
    }
  }

  // One name owned by more than one declaring class is ambiguous. Several
  // overload candidates owned by the same class remain one inherited overload
  // set and are not an ambiguity by themselves.
  for (ClassInheritedMemberView &View : Enumerated) {
    std::vector<TypeId> Owners;
    for (const ClassInheritedMemberView &Other : Enumerated) {
      if (Other.Segment != View.Segment)
        continue;
      bool Seen = false;
      for (const TypeId &Owner : Owners)
        Seen = Seen || Owner == Other.DeclaringClass;
      if (!Seen)
        Owners.push_back(Other.DeclaringClass);
    }
    View.IsAmbiguous = Owners.size() > 1;
  }

  std::sort(Enumerated.begin(), Enumerated.end(),
            [](const ClassInheritedMemberView &Left,
               const ClassInheritedMemberView &Right) {
              if (Left.Segment != Right.Segment)
                return Left.Segment < Right.Segment;
              if (Left.DeclaringClassName != Right.DeclaringClassName)
                return Left.DeclaringClassName < Right.DeclaringClassName;
              if (Left.Kind != Right.Kind)
                return Left.Kind < Right.Kind;
              return Left.Declaration < Right.Declaration;
            });
  return Enumerated;
}

MetatableId ClassRegistry::AllocateMetatableIdentity() noexcept {
  return MetatableId::FromValue(++NextMetatableValue);
}

void ClassRegistry::Record(RegisteredClass Registered) {
  for (RegisteredClass &Existing : Records) {
    if (Existing.QualifiedName == Registered.QualifiedName) {
      Existing = std::move(Registered);
      return;
    }
  }
  Records.push_back(std::move(Registered));
}

const RegisteredClass *
ClassRegistry::Find(std::string_view QualifiedName) const noexcept {
  for (const RegisteredClass &Existing : Records) {
    if (Existing.QualifiedName == QualifiedName)
      return &Existing;
  }
  return nullptr;
}

const RegisteredClass *ClassRegistry::Find(const TypeId &Type) const noexcept {
  if (!Type.IsValid())
    return nullptr;
  for (const RegisteredClass &Existing : Records) {
    if (Existing.Type == Type)
      return &Existing;
  }
  return nullptr;
}

const RegisteredClass *
ClassRegistry::FindBySymbol(const SymbolId &ClassSymbol) const noexcept {
  if (!ClassSymbol.IsValid())
    return nullptr;
  for (const RegisteredClass &Existing : Records) {
    if (Existing.ClassSymbol == ClassSymbol)
      return &Existing;
  }
  return nullptr;
}

RegisteredClass *ClassRegistry::FindForUpdate(const TypeId &Type) noexcept {
  if (!Type.IsValid())
    return nullptr;
  for (RegisteredClass &Existing : Records) {
    if (Existing.Type == Type)
      return &Existing;
  }
  return nullptr;
}

RegisteredClass *
ClassRegistry::FindForUpdate(const SymbolId &ClassSymbol) noexcept {
  if (!ClassSymbol.IsValid())
    return nullptr;
  for (RegisteredClass &Existing : Records) {
    if (Existing.ClassSymbol == ClassSymbol)
      return &Existing;
  }
  return nullptr;
}

bool ClassRegistry::Matches(const RegisteredClass &Registered,
                            const StateIdentity &Origin, const TypeId &Type,
                            const SymbolId &ClassSymbol) noexcept {
  return Registered.Origin == Origin && Registered.Type == Type &&
         Registered.ClassSymbol == ClassSymbol &&
         Registered.Metatable.IsValid();
}

bool ClassRegistry::IsCurrent(const RegisteredClass &Registered,
                              std::uint64_t LifecycleGeneration) noexcept {
  return Registered.LifecycleGeneration == LifecycleGeneration;
}

} // namespace Luna::Detail
