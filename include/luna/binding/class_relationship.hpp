#pragma once

// Explicit relationships between registered classes: base edges and safe
// downcast policies.
//
// A relationship is described, never discovered. `Base` states one edge from
// the class being registered to a class already identified by its own validated
// stable key, and `Cast` states the one non-mutating policy a downcast to this
// class is permitted through. Both are captured where the consumer's types are
// still complete, so the declared C++ facts - whether the named class really is
// a base, whether it is publicly reachable, and how a pointer is adjusted - are
// recorded here rather than guessed by a backend that never sees either type.

// clang-format off
#include <luna/type/stable_type_key.hpp>

#include <string>
#include <string_view>
#include <type_traits>
// clang-format on

namespace Luna {

namespace Detail {

// The pointer adjustment of one relationship edge. It is a pure function of the
// pointer it receives and never throws, so it is usable from the validated
// access gate.
using ClassPointerAdjustment = void *(*)(void *);

// The non-mutating compatibility probe of one safe downcast: the adjusted
// read-only pointer when the object really is a value of the target class, and
// null when it is not. It never mutates, allocates, or throws, so an object it
// rejects never reaches a committing conversion or a native target.
using ClassCompatibilityProbe = const void *(*)(const void *);

// The reflected policy identity of a downcast that decides compatibility with
// runtime type assistance. The assistance stays internal: no persistent
// identity derives from a runtime type name or address.
inline constexpr std::string_view RuntimeTypeCastPolicyName =
    "luna.runtimetype";

// One declared base edge of the class being registered.
struct BaseRequest final {
  StableTypeKey Base;

  // The named class really is a base of the class being registered.
  bool DeclaresBase = false;

  // The base is reachable through one unambiguous public path in C++ itself.
  bool IsAccessible = false;

  ClassPointerAdjustment Upcast = nullptr;
};

// One declared safe downcast to the class being registered, from the class the
// source key names.
struct CastRequest final {
  StableTypeKey Source;
  std::string Policy;

  bool DeclaresBase = false;
  bool IsAccessible = false;
  bool UsesRuntimeTypeAssistance = false;

  ClassCompatibilityProbe Compatible = nullptr;
  ClassPointerAdjustment Downcast = nullptr;
};

// True when a pointer to `Source` can be adjusted to `Target` without runtime
// assistance. A virtual base is excluded, because only runtime assistance can
// adjust away from one.
template <class Target, class Source>
concept StaticallyDowncastable =
    requires(Source *Object) { static_cast<Target *>(Object); };

template <class Derived, class Base>
[[nodiscard]] BaseRequest MakeBaseRequest(const StableTypeKey &Key) {
  static_assert(std::is_class_v<Base>,
                "Luna base registration requires a class or struct type.");
  static_assert(sizeof(Base) > 0,
                "Luna base registration requires a complete base type.");

  BaseRequest Request;
  Request.Base = Key;
  Request.DeclaresBase =
      std::is_base_of_v<Base, Derived> && !std::is_same_v<Base, Derived>;
  Request.IsAccessible =
      Request.DeclaresBase && std::is_convertible_v<Derived *, Base *>;
  if constexpr (std::is_base_of_v<Base, Derived> &&
                !std::is_same_v<Base, Derived> &&
                std::is_convertible_v<Derived *, Base *>) {
    Request.Upcast = [](void *Object) -> void * {
      return static_cast<Base *>(static_cast<Derived *>(Object));
    };
  }
  return Request;
}

template <class Target, class Source>
[[nodiscard]] const void *ProbeThroughRuntimeType(const void *Object) {
  if constexpr (std::is_polymorphic_v<Source>) {
    return dynamic_cast<const Target *>(static_cast<const Source *>(Object));
  } else {
    static_cast<void>(Object);
    return nullptr;
  }
}

template <class Target, class Source>
[[nodiscard]] void *AdjustThroughRuntimeType(void *Object) {
  if constexpr (std::is_polymorphic_v<Source>) {
    return dynamic_cast<Target *>(static_cast<Source *>(Object));
  } else {
    static_cast<void>(Object);
    return nullptr;
  }
}

template <class Target, class Source, class Check>
[[nodiscard]] const void *ProbeThroughDeclaredCheck(const void *Object) {
  // The declared check is a consumer predicate reached from the access gate, so
  // an escaping exception is an incompatible object rather than a terminated
  // process.
  try {
    const Source *Received = static_cast<const Source *>(Object);
    Check Predicate{};
    if (!static_cast<bool>(Predicate(*Received)))
      return nullptr;
    if constexpr (StaticallyDowncastable<Target, Source>)
      return static_cast<const Target *>(Received);
    else
      return nullptr;
  } catch (...) {
    return nullptr;
  }
}

template <class Target, class Source>
[[nodiscard]] void *AdjustStatically(void *Object) {
  if constexpr (StaticallyDowncastable<Target, Source>) {
    return static_cast<Target *>(static_cast<Source *>(Object));
  } else {
    static_cast<void>(Object);
    return nullptr;
  }
}

template <class Target, class Source>
[[nodiscard]] CastRequest MakeRuntimeTypeCastRequest(const StableTypeKey &Key) {
  static_assert(std::is_class_v<Source>,
                "Luna cast registration requires a class or struct type.");
  static_assert(sizeof(Source) > 0,
                "Luna cast registration requires a complete source type.");

  CastRequest Request;
  Request.Source = Key;
  Request.Policy = std::string(RuntimeTypeCastPolicyName);
  Request.DeclaresBase =
      std::is_base_of_v<Source, Target> && !std::is_same_v<Source, Target>;
  Request.IsAccessible =
      Request.DeclaresBase && std::is_convertible_v<Target *, Source *>;
  Request.UsesRuntimeTypeAssistance = true;
  if constexpr (std::is_polymorphic_v<Source>) {
    Request.Compatible = &ProbeThroughRuntimeType<Target, Source>;
    Request.Downcast = &AdjustThroughRuntimeType<Target, Source>;
  }
  return Request;
}

template <class Target, class Source, class Check>
[[nodiscard]] CastRequest MakeCheckedCastRequest(const StableTypeKey &Key,
                                                 std::string_view Identity) {
  static_assert(std::is_class_v<Source>,
                "Luna cast registration requires a class or struct type.");
  static_assert(
      std::is_empty_v<Check> && std::is_default_constructible_v<Check>,
      "Luna requires a stateless compatibility check, such as a capture-free "
      "lambda, so the check carries no state of its own across the member "
      "boundary.");

  CastRequest Request;
  Request.Source = Key;
  Request.Policy = std::string(Identity);
  Request.DeclaresBase =
      std::is_base_of_v<Source, Target> && !std::is_same_v<Source, Target>;
  Request.IsAccessible =
      Request.DeclaresBase && std::is_convertible_v<Target *, Source *>;
  if constexpr (StaticallyDowncastable<Target, Source>) {
    Request.Compatible = &ProbeThroughDeclaredCheck<Target, Source, Check>;
    Request.Downcast = &AdjustStatically<Target, Source>;
  }
  return Request;
}

} // namespace Detail

} // namespace Luna
