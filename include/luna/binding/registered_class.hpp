#pragma once

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/type/stable_type_key.hpp>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

template <class Type> struct RegisteredClassTrait : std::false_type {};

template <class Type>
concept RegisteredClassType =
    std::is_class_v<std::remove_cvref_t<Type>> &&
    RegisteredClassTrait<std::remove_cvref_t<Type>>::value;

namespace Detail {

template <class Type> [[nodiscard]] inline StableTypeKey &ClassKeySlotFor() {
  static StableTypeKey Recorded;
  return Recorded;
}

template <class Type> inline void RecordClassKey(const StableTypeKey &Key) {
  StableTypeKey &Recorded = ClassKeySlotFor<Type>();
  if (Recorded.IsEmpty())
    Recorded = Key;
}

template <class Type>
[[nodiscard]] inline const StableTypeKey &RecordedClassKey() {
  return ClassKeySlotFor<Type>();
}

using ClassKeyResolver = const StableTypeKey &(*)();

template <class Type>
[[nodiscard]] constexpr ClassKeyResolver ClassKeyResolverFor() noexcept {
  return +[]() -> const StableTypeKey & { return RecordedClassKey<Type>(); };
}

template <class Type> struct InstanceReturnTrait {
  static constexpr bool IsDeclared = false;
  static constexpr bool RequiresLifetime = false;
  using Native = void;
};

template <RegisteredClassType Type> struct InstanceReturnTrait<Type> {
  static constexpr bool IsDeclared = std::is_move_constructible_v<Type>;
  static constexpr bool RequiresLifetime = false;
  using Native = Type;
};

template <RegisteredClassType Type> struct InstanceReturnTrait<Type *> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresLifetime = true;
  using Native = Type;
};

template <RegisteredClassType Type>
struct InstanceReturnTrait<std::shared_ptr<Type>> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresLifetime = false;
  using Native = Type;
};

template <class Type>
inline constexpr bool IsInstanceReturnType =
    InstanceReturnTrait<Type>::IsDeclared;

} // namespace Detail

template <class Type> OwnedValue OwnedValue::Instance(Type Value) {
  static_assert(RegisteredClassType<Type>,
                "A manufactured Luna instance names a class that opted in with "
                "Luna::RegisteredClassTrait.");
  ClassAllocator Protocol = ClassAllocator::ForOwnedObject<Type>(
      Detail::ConstructedStoragePolicyName);
  auto Held = std::make_shared<Type>(std::move(Value));
  ClassAllocator::ConstructOperation Build = [Held](void *Storage) {
    static_cast<void>(new (Storage) Type(std::move(*Held)));
    return AllocatorStepResult::Done();
  };
  return OwnedValue::PendingInstance(
      Detail::RecordedClassKey<Type>(),
      Detail::CreatedClassInstance(std::move(Protocol), std::move(Build)));
}

template <class Type>
OwnedValue OwnedValue::Instance(std::shared_ptr<Type> Shared) {
  static_assert(RegisteredClassType<Type>,
                "A manufactured Luna instance names a class that opted in with "
                "Luna::RegisteredClassTrait.");
  Type *const Object = Shared.get();
  if (Object == nullptr)
    return OwnedValue::PendingInstance(Detail::RecordedClassKey<Type>(),
                                       Detail::ConstructedInstance());
  return OwnedValue::PendingInstance(
      Detail::RecordedClassKey<Type>(),
      Detail::SharedClassInstance(
          static_cast<void *>(Object),
          std::static_pointer_cast<void>(std::move(Shared))));
}

template <class Type>
OwnedValue OwnedValue::Instance(Type *Borrowed, OwnershipPolicy Declared) {
  static_assert(RegisteredClassType<Type>,
                "A manufactured Luna instance names a class that opted in with "
                "Luna::RegisteredClassTrait.");
  return OwnedValue::PendingInstance(
      Detail::RecordedClassKey<Type>(),
      Detail::BorrowedClassInstance(static_cast<void *>(Borrowed),
                                    Declared.Lifetime()));
}

} // namespace Luna
