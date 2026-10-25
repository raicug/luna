#pragma once

// Foundation types; no public header names a Luau type.

// Windows headers define these names, so the API defends itself here.
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
using TypeId = std::uint32_t;
using SymbolId = std::uint32_t;

inline constexpr TypeId NoType = 0;

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

  // Chunk and line are split out of Luau's "chunk:line: message" prefix.
  ErrorDiagnostic(ErrorCategory Category, std::string Text, std::string Chunk,
                  int Line, std::string Traceback)
      : Category_(Category), Text_(std::move(Text)), Chunk_(std::move(Chunk)),
        Line_(Line), Traceback_(std::move(Traceback)) {}

  [[nodiscard]] ErrorCategory Category() const noexcept { return Category_; }
  [[nodiscard]] const std::string &Message() const noexcept { return Text_; }
  [[nodiscard]] const std::string &Chunk() const noexcept { return Chunk_; }
  [[nodiscard]] int Line() const noexcept { return Line_; }
  [[nodiscard]] const std::string &Traceback() const noexcept {
    return Traceback_;
  }
  [[nodiscard]] bool HasLocation() const noexcept { return Line_ > 0; }

  // "chunk:line: message", or the message alone when it already carries it.
  [[nodiscard]] std::string Where() const {
    if (!HasLocation() || Chunk_.empty() || Text_.rfind(Chunk_ + ":", 0) == 0)
      return Text_;
    return Chunk_ + ":" + std::to_string(Line_) + ": " + Text_;
  }

private:
  ErrorCategory Category_ = ErrorCategory::None;
  std::string Text_;
  std::string Chunk_;
  int Line_ = 0;
  std::string Traceback_;
};

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

  // A success that is not completion: the script is parked waiting on a signal,
  // and only a later Tick finishes it.
  static ExecutionResult Pending() {
    ExecutionResult Result;
    Result.Parked = true;
    return Result;
  }
  [[nodiscard]] bool IsPending() const noexcept { return Parked; }

private:
  std::optional<ErrorDiagnostic> Failure;
  bool Parked = false;
};

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

class Symbol {
public:
  Symbol() = default;
  // Required is how many arguments cannot be omitted; Arity is -1 when unknown.
  Symbol(std::string Name, std::string Qualified, SymbolKind Kind,
         int Arity = -1, int Required = -1)
      : Name_(std::move(Name)), Qualified_(std::move(Qualified)), Kind_(Kind),
        Arity_(Arity), Required_(Required) {}

  [[nodiscard]] const std::string &Name() const noexcept { return Name_; }
  [[nodiscard]] const std::string &QualifiedName() const noexcept {
    return Qualified_;
  }
  [[nodiscard]] SymbolKind Kind() const noexcept { return Kind_; }
  [[nodiscard]] int Arity() const noexcept { return Arity_; }
  [[nodiscard]] int Required() const noexcept { return Required_; }

private:
  std::string Name_;
  std::string Qualified_;
  SymbolKind Kind_ = SymbolKind::None;
  int Arity_ = -1;
  int Required_ = -1;
};

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

enum class ClassOperator : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  Power,
  UnaryMinus,
  Negate = UnaryMinus,
  Equal,
  Less,
  LessOrEqual,
  Concatenate,
  Length,
  ToText,
  Call,
  Iterate,
  Index,
  NewIndex,
};

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

// Specialise this, at global scope, to make a class bindable.
template <class T> struct RegisteredClassTrait : std::false_type {};

namespace detail {
class Model;
class Plan;
struct Target;
struct ClassInfo;
struct Runtime;
struct Liveness;

template <class T> inline constexpr unsigned char TypeToken = 0;

template <class T> struct RegisteredClassTrait : std::false_type {};

// A class opts in through either name; the public one is the one to use.
template <class T>
inline constexpr bool IsClassType =
    RegisteredClassTrait<T>::value || ::luna::RegisteredClassTrait<T>::value;

template <class E> struct RegisteredEnumTrait : std::false_type {};

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

} // namespace luna

#pragma pop_macro("max")
#pragma pop_macro("min")
#pragma pop_macro("Register")
#pragma pop_macro("RegisterClass")
