#pragma once

// Construction candidates as ordinary callable candidates.
//
// A constructor, a factory, and a singleton accessor are not a separate
// invocation pipeline. Each one is described by exactly the canonical callable
// metadata every other declaration uses - one immutable parameter descriptor
// per declared parameter, required, optional, defaulted, or variadic - and each
// one is invoked through exactly the same erased adapter. The only thing they
// add is the return shape: instead of a scalar or a pack, they publish one
// value of the registered class, and they carry the ownership statement that
// decides how that value is owned.
//
// What a candidate declares decides its ownership result, and Luna never
// guesses it:
//
//   * a constructor states Lua ownership and hands Luna the storage protocol
//     plus the one construction step that builds the object from the converted
//     arguments, so allocation, construction, ownership, and publication are
//     all milestones of one gate;
//   * a factory returning the class by value states Lua ownership and moves the
//     produced object into the storage that protocol allocates;
//   * a factory or accessor returning `std::shared_ptr<T>` states shared
//     ownership and retains exactly one reference;
//   * a singleton accessor returning `T&` or `T*` states borrowed ownership,
//     which is the singleton default, and therefore carries one explicit
//     lifetime.
//
// An explicit `OwnershipPolicy` that contradicts the declared result is
// recorded as the declaration's first deterministic refusal, so it fails the
// whole transaction rather than being silently reinterpreted.

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

// One staged construction candidate of one class: the erased candidate, the
// reflected symbol kind it declares, the ownership result it publishes, the
// canonical identity of the allocator policy behind it, and the first
// deterministic refusal the declaration recorded, if any.
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

// One erased construction candidate: the ordinary callable adapter over a
// target whose result is one staged object of the registered class `Class`.
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

// The storage protocol of one class Luna creates values of. It is built once
// per candidate, captures nothing, and is retained by every value created
// through it until that value's cleanup completes.
template <class Type>
[[nodiscard]] ClassAllocator ConstructedStorageProtocolFor() {
  return ClassAllocator::ForOwnedObject<Type>(ConstructedStoragePolicyName);
}

// The one storage protocol every candidate that creates a value of one
// registered class allocates from and releases through.
//
// A class selects it once, and every creating candidate of that class reads the
// selection where it creates its object rather than where it was declared. That
// is what makes the selection order-independent: a consumer may state the
// protocol before or after declaring its constructors and factories, and the
// whole class still creates its values through exactly one protocol. An
// undeclared selection is a class that never stated one, and its candidates
// create their objects through Luna's own protocol of the class instead.
using StorageSelection = std::shared_ptr<ClassAllocator>;

[[nodiscard]] inline StorageSelection MakeStorageSelection() {
  return std::make_shared<ClassAllocator>();
}

// The protocol one candidate creates its object through: the class's selection
// when it states one, and otherwise the ordinary protocol that candidate was
// built with.
[[nodiscard]] inline ClassAllocator
SelectedStorageProtocol(const StorageSelection &Selected,
                        const ClassAllocator &Ordinary) {
  if (Selected && Selected->IsDeclared())
    return *Selected;
  return Ordinary;
}

// One construction step over arguments the call already converted. The captured
// payload is held indirectly so the step stays copyable even when the class or
// one of its arguments is move-only.
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

// One constructor candidate of `Type`, built over its declared parameter list.
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

// The ownership result one declared factory return type states.
template <class Type, class Produced> struct FactoryResultTrait {
  static constexpr bool IsSupported = false;
};

template <class Type> struct FactoryResultTrait<Type, Type> {
  static constexpr bool IsSupported = true;
  static constexpr ConstructionOwnership Result =
      ConstructionOwnership::LuaOwned;
  static constexpr std::string_view Policy = ConstructedStoragePolicyName;

  // The produced object is moved into the storage the protocol allocates, so
  // the value Luna publishes is the one Luna also destroys and deallocates.
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

// The ownership result one declared singleton accessor return type states.
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

// The refusal an explicit policy earns when it contradicts what the declaration
// states, or nothing when the two agree.
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

// One factory candidate, built over the declared parameter list of its target.
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

// One singleton accessor candidate. Borrowed ownership is the default, so the
// declared result of `T&` or `T*` carries the explicit lifetime the policy
// states; a shared accessor states shared ownership explicitly.
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
