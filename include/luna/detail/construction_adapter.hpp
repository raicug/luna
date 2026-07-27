#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna::Detail {

struct ConstructionRequest final {
  std::optional<ErasedCallableDescriptor> Callable;
  SymbolKind Kind = SymbolKind::Constructor;
  ConstructionOwnership Ownership = ConstructionOwnership::LuaOwned;
  std::string AllocatorPolicy;
  std::string Refusal;

  ConstructionRequest() = default;
  ConstructionRequest(const ConstructionRequest &) = delete;
  ConstructionRequest &operator=(const ConstructionRequest &) = delete;
  ConstructionRequest(ConstructionRequest &&) noexcept = default;
  ConstructionRequest &operator=(ConstructionRequest &&) noexcept = default;
  ~ConstructionRequest() = default;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable.has_value() && Callable->HasTarget();
  }
};

template <class... Parameters, class Target>
[[nodiscard]] ErasedCallableDescriptor
MakeConstructionDescriptor(const StableTypeKey &Class, Target &&Producer) {
  using Signature = ConstructedInstance(Parameters...);
  using Stored = std::decay_t<Target>;
  using Adapter = CallableAdapter<Signature, Stored>;

  CallableMetadata Metadata =
      DescriptorMetadata<Signature>::CreateForInstance(Class);
  return ErasedCallableDescriptor(std::move(Metadata),
                                  Adapter(std::forward<Target>(Producer)));
}

template <class Type>
[[nodiscard]] ClassAllocator ConstructedStorageProtocolFor() {
  return ClassAllocator::ForOwnedObject<Type>(ConstructedStoragePolicyName);
}

using StorageSelection = std::shared_ptr<ClassAllocator>;

[[nodiscard]] inline StorageSelection MakeStorageSelection() {
  return std::make_shared<ClassAllocator>();
}

[[nodiscard]] inline ClassAllocator
SelectedStorageProtocol(const StorageSelection &Selected,
                        const ClassAllocator &Ordinary) {
  if (Selected && Selected->IsDeclared())
    return *Selected;
  return Ordinary;
}

template <class Type, class... Values>
[[nodiscard]] ClassAllocator::ConstructOperation
MakeConstructionStep(Values &&...Supplied) {
  using Payload = std::tuple<std::decay_t<Values>...>;
  auto Held = std::make_shared<Payload>(std::forward<Values>(Supplied)...);
  return [Held](void *Storage) {
    const auto Build = [Storage](auto &&...Arguments) {
      static_cast<void>(new (Storage) Type(std::move(Arguments)...));
    };
    std::apply(Build, *Held);
    return AllocatorStepResult::Done();
  };
}

template <class Type, class... Arguments>
struct ConstructorCandidateBuilder final {
  [[nodiscard]] static ConstructionRequest Build(const StableTypeKey &Class,
                                                 StorageSelection Selected) {
    static_assert(
        (SupportedParameter<Arguments> && ...),
        "A Luna constructor declares only supported parameter types.");
    static_assert(std::is_constructible_v<Type, Arguments...>,
                  "A Luna constructor requires the registered class to be "
                  "constructible from its declared parameters.");

    ClassAllocator Ordinary = ConstructedStorageProtocolFor<Type>();
    auto Target = [Selected,
                   Ordinary](Arguments... Supplied) -> ConstructedInstance {
      ClassAllocator::ConstructOperation Step =
          MakeConstructionStep<Type, Arguments...>(
              std::forward<Arguments>(Supplied)...);
      ClassAllocator Protocol = SelectedStorageProtocol(Selected, Ordinary);
      return CreatedClassInstance(std::move(Protocol), std::move(Step));
    };

    ConstructionRequest Request;
    Request.Kind = SymbolKind::Constructor;
    Request.Ownership = ConstructionOwnership::LuaOwned;
    Request.AllocatorPolicy = std::string(ConstructedStoragePolicyName);
    Request.Callable.emplace(
        MakeConstructionDescriptor<Arguments...>(Class, std::move(Target)));
    return Request;
  }
};

template <class Type, class... Arguments>
[[nodiscard]] ConstructionRequest
MakeConstructorRequest(const StableTypeKey &Class, StorageSelection Selected) {
  using Builder = ConstructorCandidateBuilder<Type, Arguments...>;
  return Builder::Build(Class, std::move(Selected));
}

template <class Type, class Produced> struct FactoryResultTrait {
  static constexpr bool IsSupported = false;
};

template <class Type> struct FactoryResultTrait<Type, Type> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result =
      ConstructionOwnership::LuaOwned;
  static constexpr std::string_view Policy = ConstructedStoragePolicyName;

  [[nodiscard]] static ConstructedInstance Adopt(const ClassAllocator &Protocol,
                                                 Type Source) {
    static_assert(std::is_move_constructible_v<Type>,
                  "A Luna factory returning the class by value requires a "
                  "move-constructible class type.");

    ClassAllocator::ConstructOperation Step =
        MakeConstructionStep<Type, Type>(std::move(Source));
    return CreatedClassInstance(Protocol, std::move(Step));
  }
};

template <class Type> struct FactoryResultTrait<Type, std::shared_ptr<Type>> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result = ConstructionOwnership::Shared;
  static constexpr std::string_view Policy = AdoptedStoragePolicyName;

  [[nodiscard]] static ConstructedInstance Adopt(const ClassAllocator &Protocol,
                                                 std::shared_ptr<Type> Source) {
    static_cast<void>(Protocol);
    Type *const Object = Source.get();
    std::shared_ptr<void> Owned =
        std::static_pointer_cast<void>(std::move(Source));
    return SharedClassInstance(Object, std::move(Owned));
  }
};

template <class Type, class Accessed> struct SingletonResultTrait {
  static constexpr bool IsSupported = false;
};

template <class Type> struct SingletonResultTrait<Type, Type &> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result =
      ConstructionOwnership::Borrowed;

  [[nodiscard]] static void *Address(Type &Source) noexcept { return &Source; }
};

template <class Type> struct SingletonResultTrait<Type, Type *> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result =
      ConstructionOwnership::Borrowed;

  [[nodiscard]] static void *Address(Type *Source) noexcept { return Source; }
};

template <class Type> struct SingletonResultTrait<Type, std::shared_ptr<Type>> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result = ConstructionOwnership::Shared;
};

[[nodiscard]] inline std::string
ClassifyOwnershipPolicy(const OwnershipPolicy &Policy,
                        ConstructionOwnership Declared) {
  if (Policy.Ownership() != Declared)
    return "the explicit ownership policy states " +
           std::string(ConstructionOwnershipText(Policy.Ownership())) +
           " ownership but the declared result states " +
           std::string(ConstructionOwnershipText(Declared)) + " ownership.";
  if (!Policy.IsCoherent())
    return "the explicit ownership policy is not coherent: a borrowed result "
           "requires one declared lifetime and no other result may declare "
           "one.";
  return std::string();
}

template <class Type, class Produced, class... Parameters>
struct FactoryCandidateBuilder final {
  template <class Target>
  [[nodiscard]] static ConstructionRequest Build(const StableTypeKey &Class,
                                                 Target Producer,
                                                 StorageSelection Selected) {
    using Trait = FactoryResultTrait<Type, Produced>;
    static_assert(Trait::IsSupported,
                  "A Luna factory returns the registered class by value or as "
                  "std::shared_ptr<T>.");
    static_assert((SupportedParameter<Parameters> && ...),
                  "A Luna factory declares only supported parameter types.");

    ClassAllocator Ordinary = ConstructedStorageProtocolFor<Type>();
    auto Adapted = [Producer, Selected, Ordinary](
                       Parameters... Supplied) mutable -> ConstructedInstance {
      Produced Result = Producer(std::forward<Parameters>(Supplied)...);
      const ClassAllocator Protocol =
          SelectedStorageProtocol(Selected, Ordinary);
      return Trait::Adopt(Protocol, std::move(Result));
    };

    ConstructionRequest Request;
    Request.Kind = SymbolKind::Factory;
    Request.Ownership = Trait::Result;
    Request.AllocatorPolicy = std::string(Trait::Policy);
    Request.Callable.emplace(
        MakeConstructionDescriptor<Parameters...>(Class, std::move(Adapted)));
    return Request;
  }
};

template <class Type, class Signature> struct FactoryRequestBuilder;

template <class Type, class Produced, class... Parameters>
struct FactoryRequestBuilder<Type, Produced(Parameters...)> final {
  template <class Target>
  [[nodiscard]] static ConstructionRequest Build(const StableTypeKey &Class,
                                                 Target Producer,
                                                 StorageSelection Selected) {
    using Builder = FactoryCandidateBuilder<Type, Produced, Parameters...>;
    return Builder::Build(Class, std::move(Producer), std::move(Selected));
  }
};

template <class Type, class Target>
[[nodiscard]] ConstructionRequest
MakeFactoryRequest(const StableTypeKey &Class, Target Producer,
                   StorageSelection Selected) {
  using Signature = typename CallableSignature<std::decay_t<Target>>::Type;
  using Builder = FactoryRequestBuilder<Type, Signature>;
  return Builder::Build(Class, std::move(Producer), std::move(Selected));
}

template <class Type, class Accessed, class... Parameters>
struct SingletonCandidateBuilder final {
  template <class Target>
  [[nodiscard]] static ConstructionRequest
  Build(const StableTypeKey &Class, Target Accessor, OwnershipPolicy Policy) {
    using Trait = SingletonResultTrait<Type, Accessed>;
    static_assert(Trait::IsSupported,
                  "A Luna singleton accessor returns the registered class as "
                  "T&, T*, or std::shared_ptr<T>.");
    static_assert((SupportedParameter<Parameters> && ...),
                  "A Luna singleton accessor declares only supported parameter "
                  "types.");
    constexpr ConstructionOwnership Declared = Trait::Result;

    auto Adapted = [Accessor, Policy](
                       Parameters... Supplied) mutable -> ConstructedInstance {
      if constexpr (Declared == ConstructionOwnership::Shared) {
        std::shared_ptr<Type> Held =
            Accessor(std::forward<Parameters>(Supplied)...);
        Type *const Object = Held.get();
        std::shared_ptr<void> Owned =
            std::static_pointer_cast<void>(std::move(Held));
        return SharedClassInstance(Object, std::move(Owned));
      } else {
        Accessed Reached = Accessor(std::forward<Parameters>(Supplied)...);
        void *const Object = Trait::Address(Reached);
        const LifetimeHandle &Borrowed = Policy.Lifetime();
        return BorrowedClassInstance(Object, Borrowed);
      }
    };

    ConstructionRequest Request;
    Request.Kind = SymbolKind::Factory;
    Request.Ownership = Declared;
    Request.AllocatorPolicy = std::string(AdoptedStoragePolicyName);
    Request.Refusal = ClassifyOwnershipPolicy(Policy, Declared);
    Request.Callable.emplace(
        MakeConstructionDescriptor<Parameters...>(Class, std::move(Adapted)));
    return Request;
  }
};

template <class Type, class Signature> struct SingletonRequestBuilder;

template <class Type, class Accessed, class... Parameters>
struct SingletonRequestBuilder<Type, Accessed(Parameters...)> final {
  template <class Target>
  [[nodiscard]] static ConstructionRequest
  Build(const StableTypeKey &Class, Target Accessor, OwnershipPolicy Policy) {
    using Builder = SingletonCandidateBuilder<Type, Accessed, Parameters...>;
    return Builder::Build(Class, std::move(Accessor), std::move(Policy));
  }
};

template <class Type, class Target>
[[nodiscard]] ConstructionRequest
MakeSingletonRequest(const StableTypeKey &Class, Target Accessor,
                     OwnershipPolicy Policy) {
  using Signature = typename CallableSignature<std::decay_t<Target>>::Type;
  using Builder = SingletonRequestBuilder<Type, Signature>;
  return Builder::Build(Class, std::move(Accessor), std::move(Policy));
}

} // namespace Luna::Detail
