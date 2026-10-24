#pragma once

// luna : foundation types.
//
// Nothing in this header (or any other public header) names a Luau type. The VM
// stays entirely inside src/, so a consumer includes <luna/luna.hpp> and links
// luna::luna without an include path or a compile definition for Luau.

// The Windows headers (and some SDKs) define names that collide with
// this API. Consumers should not have to #undef them by hand; here the
// headers defend themselves so the prologue can be deleted.
#pragma push_macro("RegisterClass")
#pragma push_macro("Register")
#pragma push_macro("min")
#pragma push_macro("max")
#undef RegisterClass
#undef Register
#undef min
#undef max

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luna {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// Dense indices into the State's append-only model. Not content hashes: they are
// only meaningful inside one State, which is all a binding ever needs.
using TypeId = std::uint32_t;
using SymbolId = std::uint32_t;

inline constexpr TypeId NoType = 0;

// An optional stable string that identifies a class across runs. Used for the
// symbol palette, diagnostics and generated artifacts. It is never hashed into
// an identity.
class StableTypeKey {
public:
  constexpr explicit StableTypeKey(std::string_view Value) noexcept
      : Text(Value) {}

  [[nodiscard]] constexpr std::string_view Value() const noexcept {
    return Text;
  }

private:
  std::string_view Text;
};

// ---------------------------------------------------------------------------
// Diagnostics and results
// ---------------------------------------------------------------------------

enum class ErrorCategory : std::uint8_t {
  None = 0,
  Compile,
  Runtime,
  Interrupted,
  Registration,
};

class ErrorDiagnostic {
public:
  ErrorDiagnostic() = default;

  ErrorDiagnostic(ErrorCategory Category, std::string Text)
      : Category_(Category), Text_(std::move(Text)) {}

  [[nodiscard]] ErrorCategory Category() const noexcept { return Category_; }
  [[nodiscard]] const std::string &Message() const noexcept { return Text_; }

private:
  ErrorCategory Category_ = ErrorCategory::None;
  std::string Text_;
};

// Owned result of a fallible operation. Always cheap to return by value and
// never throws; a failure carries a message the host can display verbatim.
class RegistrationResult {
public:
  RegistrationResult() = default;
  explicit RegistrationResult(ErrorDiagnostic Failed)
      : Failure(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept { return !Failure.has_value(); }
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return Failure ? &*Failure : nullptr;
  }

private:
  std::optional<ErrorDiagnostic> Failure;
};

class ExecutionResult {
public:
  ExecutionResult() = default;
  explicit ExecutionResult(ErrorDiagnostic Failed)
      : Failure(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept { return !Failure.has_value(); }
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return Failure ? &*Failure : nullptr;
  }
  [[nodiscard]] bool IsInterrupted() const noexcept {
    return Failure && Failure->Category() == ErrorCategory::Interrupted;
  }

private:
  std::optional<ErrorDiagnostic> Failure;
};

// Builder-local diagnostic accumulator. Registration reports the first problem
// it finds and keeps going only to finish the builder chain.
class Diag {
public:
  [[nodiscard]] bool Ok() const noexcept { return Message_.empty(); }
  [[nodiscard]] const std::string &Text() const noexcept { return Message_; }

  Diag &Fail(std::string Text) {
    if (Message_.empty())
      Message_ = std::move(Text);
    return *this;
  }

  template <class... Parts> Diag &FailJoin(Parts &&...Chunks) {
    if (Message_.empty()) {
      (Message_.append(std::forward<Parts>(Chunks)), ...);
    }
    return *this;
  }

private:
  std::string Message_;
};

// ---------------------------------------------------------------------------
// Reflection vocabulary
// ---------------------------------------------------------------------------

enum class SymbolKind : std::uint8_t {
  None = 0,
  Function,
  Method,
  StaticMethod,
  Factory,
  Constructor,
  Property,
  Field,
  Class,
  Enumeration,
  Enumerator,
  EnumeratorAlias,
  Constant,
  Namespace,
  OverloadSet,
};

// One entry of the palette a host uses for autocomplete. Names are owned so a
// range stays readable after the State that produced it is gone.
class Symbol {
public:
  Symbol() = default;
  Symbol(std::string Name, std::string Qualified, SymbolKind Kind)
      : Name_(std::move(Name)), Qualified_(std::move(Qualified)), Kind_(Kind) {}

  [[nodiscard]] const std::string &Name() const noexcept { return Name_; }
  [[nodiscard]] const std::string &QualifiedName() const noexcept {
    return Qualified_;
  }
  [[nodiscard]] SymbolKind Kind() const noexcept { return Kind_; }

private:
  std::string Name_;
  std::string Qualified_;
  SymbolKind Kind_ = SymbolKind::None;
};

// A borrow of an immutable symbol list. Holds the list alive, so a consumer may
// keep a range across further registration or past the State's destruction.
class SymbolRange {
public:
  SymbolRange() = default;
  SymbolRange(std::shared_ptr<const std::vector<Symbol>> Owned,
              const Symbol *First, std::size_t Count) noexcept
      : Owned_(std::move(Owned)), Data(First), Count_(Count) {}

  [[nodiscard]] std::size_t Size() const noexcept { return Count_; }
  [[nodiscard]] bool Empty() const noexcept { return Count_ == 0; }

  [[nodiscard]] const Symbol &At(std::size_t Index) const noexcept {
    static const Symbol Empty;
    return Index < Count_ ? Data[Index] : Empty;
  }

  [[nodiscard]] const Symbol *begin() const noexcept { return Data; }
  [[nodiscard]] const Symbol *end() const noexcept { return Data + Count_; }

private:
  std::shared_ptr<const std::vector<Symbol>> Owned_;
  const Symbol *Data = nullptr;
  std::size_t Count_ = 0;
};

// ---------------------------------------------------------------------------
// Ownership
// ---------------------------------------------------------------------------

// Invalidation token for native objects the host lends to scripts. Copying a
// handle shares one control block; Invalidate() makes every copy refuse access,
// which is what a rejoin or a DataModel rebuild needs.
class LifetimeHandle {
public:
  struct Control {
    std::atomic<bool> Alive{true};
  };

  LifetimeHandle() : Shared(std::make_shared<Control>()) {}

  [[nodiscard]] bool Alive() const noexcept {
    return Shared && Shared->Alive.load(std::memory_order_acquire);
  }

  void Invalidate() noexcept {
    if (Shared)
      Shared->Alive.store(false, std::memory_order_release);
  }

  [[nodiscard]] const std::shared_ptr<Control> &ControlBlock() const noexcept {
    return Shared;
  }

private:
  std::shared_ptr<Control> Shared;
};

class OwnershipPolicy {
public:
  enum class Kind : std::uint8_t { LuaOwned = 0, Borrowed, Shared };

  static OwnershipPolicy LuaOwned() noexcept {
    OwnershipPolicy Policy;
    Policy.Mode = Kind::LuaOwned;
    return Policy;
  }

  static OwnershipPolicy Borrowed(LifetimeHandle Handle) noexcept {
    OwnershipPolicy Policy;
    Policy.Mode = Kind::Borrowed;
    Policy.Lifetime = std::move(Handle);
    return Policy;
  }

  static OwnershipPolicy Shared() noexcept {
    OwnershipPolicy Policy;
    Policy.Mode = Kind::Shared;
    return Policy;
  }

  [[nodiscard]] Kind GetKind() const noexcept { return Mode; }
  [[nodiscard]] const LifetimeHandle &GetLifetime() const noexcept {
    return Lifetime;
  }

private:
  Kind Mode = Kind::LuaOwned;
  LifetimeHandle Lifetime;
};

// ---------------------------------------------------------------------------
// Class operators
// ---------------------------------------------------------------------------

enum class ClassOperator : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  Power,
  UnaryMinus,
  Equal,
  Less,
  LessOrEqual,
  Concatenate,
  Length,
  ToText,
  Call,
  Iterate,
  Index,    // reserved
  NewIndex, // reserved
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class State;
class BindingRegistry;
class NamespaceBuilder;
class Chunk;
class ReflectionSnapshot;
class OwnedValue;
class ValuePack;
class ArgumentPack;

template <class Signature> class Delegate;
template <class T> class ClassBuilder;
template <class T> class EnumBuilder;

namespace detail {
class Model;
class Plan;
struct Target;
struct ClassInfo;

// Process-wide unique per-type token that works with /GR- (no RTTI). The
// address of a template static is unique per instantiation within a binary.
template <class T> inline constexpr unsigned char TypeToken = 0;

// Compile-time "this class was registered" opt-in. The consumer specialises it
// exactly as it did for Luna::RegisteredClassTrait.
template <class T> struct RegisteredClassTrait : std::false_type {};

// Compile-time "this enum is bound" opt-in.
template <class E> struct RegisteredEnumTrait : std::false_type {};

// The metamethod a class operator binds to.
enum class OperatorSlot : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  Power,
  Negate,
  Equal,
  Less,
  LessOrEqual,
  Concat,
  Len,
  ToString,
  Call,
  Iterate,
  Index,
  NewIndex,
};
} // namespace detail

// Public alias, so a host can specialise the trait without naming detail::.
template <class T>
struct RegisteredClassTrait : detail::RegisteredClassTrait<T> {};

} // namespace luna

#pragma pop_macro("max")
#pragma pop_macro("min")
#pragma pop_macro("Register")
#pragma pop_macro("RegisterClass")
