#pragma once

// luna : the binding surface.
//
// Registration is staged, never immediate. A builder appends plain data and
// Commit() validates it before anything is linked. Validation is a pure function
// over the staged batch, so a rejected commit has touched nothing and needs no
// undo log.

#include <luna/convert.hpp>
#include <luna/fwd.hpp>
#include <luna/value.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luna {

namespace detail {

// Resolves a per-type token to the class id this VM registered, or NoType.
TypeId ClassIdForToken(lua_State *L, const void *Token) noexcept;

// A staged call site. Push constructs a closure when the batch is linked;
// Context pushes the callable alone, with no closure around it, so a metamethod
// that finds one in a lookup table can invoke it directly. The callable itself
// is kept alive by Ctx.
using InvokeThunk = int (*)(lua_State *, void *);

struct Target {
  void (*Push)(lua_State *, const void *) = nullptr;
  void *(*Context)(lua_State *, const void *) = nullptr;
  InvokeThunk Thunk = nullptr;
  std::shared_ptr<const void> Ctx;

  [[nodiscard]] bool Valid() const noexcept { return Push != nullptr; }

  // True when this target can be invoked in place, without a closure and a VM
  // call. Every callable made by MakeCall can.
  [[nodiscard]] bool Direct() const noexcept {
    return Thunk != nullptr && Context != nullptr;
  }
};

// The first member of every erased callable, so a metamethod holding one can
// dispatch it after a single type check and one pointer read.
struct CallableHeader {
  InvokeThunk Thunk = nullptr;
};

// The token of a class-typed position, or nullptr for anything else.
template <class A> constexpr const void *TokenOf() {
  using Subject = typename ClassSubject<A>::Type;
  if constexpr (RegisteredClassTrait<Subject>::value)
    return static_cast<const void *>(&TypeToken<Subject>);
  else
    return nullptr;
}

// The erased callable plus the class id of every class-typed parameter. Ids are
// resolved once, at commit, so the call path never looks a type up.
template <class R, class... A> struct Callable {
  static constexpr std::size_t Count = sizeof...(A);

  // First member on purpose: __index reads it through CallableHeader.
  InvokeThunk Thunk = &Callable::Dispatch;

  std::function<R(A...)> Fn;
  const void *Tokens[Count ? Count : 1]{};
  TypeId Ids[Count ? Count : 1]{};
  std::string Name;

  // Runs where the parameter pack is still in scope: the token of every class
  // position is baked in at build time, never looked up at call time.
  static void FillTokens(const void **Out) {
    std::size_t Slot = 0;
    ((Out[Slot++] = TokenOf<A>()), ...);
  }

  void Resolve(lua_State *L) {
    for (std::size_t Index = 0; Index < Count; ++Index)
      Ids[Index] = Tokens[Index] ? ClassIdForToken(L, Tokens[Index]) : NoType;
  }

  // The whole invocation, with the arguments already sitting at 1..Count on the
  // stack. Both entry points below funnel through here, so there is one code
  // path whether the call came from the VM or from __index.
  static int Dispatch(lua_State *L, void *Context) {
    auto *Self = static_cast<Callable *>(Context);
    std::string Error;
    int Pushed = -1;

    // A host callable may throw -- std::stoi on a bad string, a container's
    // .at(), a user exception. Letting that unwind through the VM's C frames is
    // undefined behaviour, so it is translated here into a Lua error the script
    // can pcall. The suspension check stays outside the try block, because a
    // yield is not an exception and must not be caught as one.
    try {
      Pushed = InvokeCall<R, A...>(L, Self->Fn, Self->Ids, nullptr,
                                   Self->Name.c_str(), Error);
    } catch (const std::exception &Thrown) {
      Error = Self->Name + ": " + Thrown.what();
      Pushed = -1;
    } catch (...) {
      Error = Self->Name + ": the callable threw";
      Pushed = -1;
    }

    if (Pushed < 0) {
      // luaL_error is noreturn in this fork, so it is called rather than
      // returned.
      luaL_error(L, "%s", Error.c_str());
      return 0;
    }

    // A callable that asked to suspend yields here, after its return value is
    // already on the stack. That is what lets task.wait suspend the chunk
    // instead of sleeping a thread. The flag is an inline thread_local, so the
    // check is a load and a branch, not a call.
    if (SuspensionRequested && CompleteSuspension(L))
      return lua_yield(L, Pushed);
    return Pushed;
  }

  // The lua_CFunction form: the context is the closure's only upvalue.
  static int Call(lua_State *L) {
    return Dispatch(L, lua_touserdata(L, lua_upvalueindex(1)));
  }
};

// The token of a class-typed position, or nullptr for anything else.
template <class A> constexpr const void *TokenOf();

template <class C> void DestroyCallable(void *Memory) {
  static_cast<C *>(Memory)->~C();
}

// Copies the callable into a userdata owned by the closure it is pushed into.
template <class C> void PushCallable(lua_State *L, const void *Raw) {
  void *Memory = lua_newuserdatadtor(L, sizeof(C), &DestroyCallable<C>);
  auto *Ctx = new (Memory) C(*static_cast<const C *>(Raw));
  Ctx->Resolve(L);
  luna::detail::PushClosure(L, &C::Call, nullptr, 1);
}

// Copies the callable into a plain userdata, with no closure around it. Used by
// __index and __newindex, which invoke the thunk directly.
template <class C> void *PushContext(lua_State *L, const void *Raw) {
  void *Memory = lua_newuserdatadtor(L, sizeof(C), &DestroyCallable<C>);
  auto *Ctx = new (Memory) C(*static_cast<const C *>(Raw));
  Ctx->Resolve(L);
  return Ctx;
}

// ---------------------------------------------------------------------------
// Callable adaptation
// ---------------------------------------------------------------------------

// Primary: an unsupported callable. The specialisations below cover a function
// pointer, a member pointer, and any functor with an operator().
template <class F, class = void> struct Sig {
  using Fn = void;
  using Ctx = void;
};

template <class R, class... A> struct Sig<R (*)(A...)> {
  using Fn = std::function<R(A...)>;
  using Ctx = Callable<R, A...>;
  static Fn Make(R (*Function)(A...)) { return Fn(Function); }
};

template <class C, class R, class... A> struct Sig<R (C::*)(A...)> {
  using Fn = std::function<R(C &, A...)>;
  using Ctx = Callable<R, C &, A...>;
  static Fn Make(R (C::*Function)(A...)) {
    return [Function](C &Self, A... Args) -> R {
      return (Self.*Function)(std::forward<A>(Args)...);
    };
  }
};

template <class C, class R, class... A> struct Sig<R (C::*)(A...) const> {
  using Fn = std::function<R(const C &, A...)>;
  using Ctx = Callable<R, const C &, A...>;
  static Fn Make(R (C::*Function)(A...) const) {
    return [Function](const C &Self, A... Args) -> R {
      return (Self.*Function)(std::forward<A>(Args)...);
    };
  }
};

// Deduces a callable signature from a functor's operator(), dropping the
// implicit object parameter: a lambda passed as a plain function must not turn
// itself into the receiver.
template <class M> struct OperatorSig;

template <class R, class C, class... A> struct OperatorSig<R (C::*)(A...)> {
  using Fn = std::function<R(A...)>;
  using Ctx = Callable<R, A...>;
  template <class F> static Fn Make(F &&Callable) {
    return Fn(std::forward<F>(Callable));
  }
};

template <class R, class C, class... A>
struct OperatorSig<R (C::*)(A...) const> {
  using Fn = std::function<R(A...)>;
  using Ctx = Callable<R, A...>;
  template <class F> static Fn Make(F &&Callable) {
    return Fn(std::forward<F>(Callable));
  }
};

template <class F>
struct Sig<F, std::void_t<decltype(&F::operator())>>
    : OperatorSig<decltype(&F::operator())> {};

template <class F>
using SigOf = Sig<std::remove_cv_t<std::remove_reference_t<F>>>;

// The value a setter accepts: the second parameter of its erased form. Needed
// to build a monomorphic wrapper around a setter, because a generic lambda has
// no operator() to deduce a signature from.
template <class Erased> struct SetterSlot;
template <class R, class Self, class V, class... Rest>
struct SetterSlot<std::function<R(Self, V, Rest...)>> {
  using Type = V;
};

// Builds one staged target from any accepted callable.
template <class F>
Target MakeCall(F &&Callable, std::string Name) {
  using S = SigOf<F>;
  using C = typename S::Ctx;

  auto Ctx = std::make_shared<C>();
  Ctx->Fn = S::Make(std::forward<F>(Callable));
  Ctx->Name = std::move(Name);

  // Tokens for every parameter, in declaration order.
  C::FillTokens(Ctx->Tokens);

  Target Built;
  Built.Ctx = Ctx;
  Built.Push = &PushCallable<C>;
  Built.Context = &PushContext<C>;
  Built.Thunk = &C::Dispatch;
  return Built;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Delegate
// ---------------------------------------------------------------------------

class DelegateCallResult {
public:
  DelegateCallResult() = default;
  explicit DelegateCallResult(std::string Failed) : Error_(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept { return Error_.empty(); }
  [[nodiscard]] const std::string &Diagnostic() const noexcept { return Error_; }

private:
  std::string Error_;
};

// One subscribed Luau function, held by registry reference so it survives the
// call that produced it. Copies share the reference.
template <class Signature> class Delegate;

template <class R, class... A> class Delegate<R(A...)> {
public:
  struct Handle {
    void *VM = nullptr;
    int Ref = 0;
    ~Handle() {
      if (VM && Ref)
        detail::Unpin(static_cast<lua_State *>(VM), Ref);
    }
  };

  Delegate() = default;

  [[nodiscard]] bool IsBound() const noexcept {
    return Shared && Shared->Ref != 0 && Shared->VM != nullptr;
  }

  // Takes ownership of the value at StackIndex by registry reference, so the
  // handler outlives the call that produced it.
  static Delegate Capture(void *VM, int StackIndex) {
    auto *L = static_cast<lua_State *>(VM);
    auto Owned = std::make_shared<Handle>();
    Owned->VM = VM;
    lua_pushvalue(L, StackIndex);
    Owned->Ref = luna::detail::PinTop(L);

    Delegate Result;
    Result.Shared = std::move(Owned);
    return Result;
  }

  DelegateCallResult Invoke(A... Args) const;

private:
  std::shared_ptr<Handle> Shared;
};

template <class R, class... A>
DelegateCallResult Delegate<R(A...)>::Invoke(A... Args) const {
  if (!IsBound())
    return DelegateCallResult("the handler is no longer available");

  auto *L = static_cast<lua_State *>(Shared->VM);
  const int Base = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, Shared->Ref);

  int Pushed = 0;
  auto PushOne = [&](auto &&Value) {
    Pushed += detail::WriteValue(L, Value, NoType);
  };
  (PushOne(static_cast<A>(Args)), ...);

  const int Status = lua_pcall(L, Pushed, 0, 0);
  if (Status != 0) {
    const char *Message = lua_tostring(L, -1);
    std::string Text = Message ? Message : "handler raised an error";
    lua_settop(L, Base);
    return DelegateCallResult(std::move(Text));
  }
  lua_settop(L, Base);
  return DelegateCallResult();
}

namespace detail {
template <class T> struct IsDelegate : std::false_type {};
template <class T> struct IsDelegate<Delegate<T>> : std::true_type {};

// A delegate parameter captures the Luau function at the stack position it was
// passed at.
template <class R, class... A> struct CustomCodec<Delegate<R(A...)>, void> {
  static constexpr bool Enabled = true;

  static bool Read(lua_State *L, int Index, Delegate<R(A...)> &Out,
                   ArgError &Error) {
    if (lua_type(L, Index) != LUA_TFUNCTION) {
      Error.Set(std::string("expected a function, got ") +
                LuaTypeName(L, Index));
      return false;
    }
    Out = Delegate<R(A...)>::Capture(static_cast<void *>(L), Index);
    return true;
  }

  // A delegate is not a value a binding returns; publishing nil keeps a
  // signature that mentions it total.
  static int Write(lua_State *L, const Delegate<R(A...)> &) {
    lua_pushnil(L);
    return 1;
  }
};
} // namespace detail

// ---------------------------------------------------------------------------
// Property write hooks
// ---------------------------------------------------------------------------

template <class Hook> struct NotifyPolicy {
  Hook Callback;
};

class PropertyPolicy {
public:
  // Runs after a successful write, with the receiver and the property name.
  template <class Hook>
  static NotifyPolicy<std::decay_t<Hook>> Notify(Hook &&Callback) {
    return NotifyPolicy<std::decay_t<Hook>>{std::forward<Hook>(Callback)};
  }
};

// ---------------------------------------------------------------------------
// Staged data
// ---------------------------------------------------------------------------

namespace detail {

enum class Category : std::uint8_t {
  Function,
  Method,
  StaticMethod,
  Factory,
  Property,
  Field,
  Constant,
  Enumeration,
  Class,
  Value,
};

struct StagedCall {
  std::string Name;
  Target Call;
  Category Kind = Category::Function;
};

struct StagedProperty {
  std::string Name;
  Target Getter;
  Target Setter;
  bool Writable = false;
};

struct StagedField {
  std::string Name;
  Target Getter;
  Target Setter;
};

struct StagedOperator {
  OperatorSlot Slot = OperatorSlot::Add;
  Target Call;
};

struct StagedConstant {
  std::string Name;
  OwnedValue Value;
};

struct StagedEnumValue {
  std::string Name;
  std::int64_t Value = 0;
};

struct StagedEnum {
  std::string Name;
  std::string StableKey;
  std::vector<StagedEnumValue> Values;
  std::vector<std::pair<std::string, std::string>> Aliases;
  bool AsObjects = false;
};

struct StagedClass {
  std::string Name;
  std::string StableKey;
  const void *Token = nullptr;
  std::vector<StagedCall> Calls;
  std::vector<StagedProperty> Properties;
  std::vector<StagedField> Fields;
  std::vector<StagedOperator> Operators;
  std::vector<StagedConstant> Constants;
};

struct StagedValue {
  std::string Name;
  Target Factory;
  OwnershipPolicy Policy;
};

struct Batch {
  std::vector<StagedCall> Calls;
  std::vector<StagedClass> Classes;
  std::vector<StagedEnum> Enums;
  std::vector<StagedConstant> Constants;
  std::vector<StagedValue> Values;

  [[nodiscard]] bool Empty() const noexcept {
    return Calls.empty() && Classes.empty() && Enums.empty() &&
           Constants.empty() && Values.empty();
  }
};

// Validates a staged batch against the committed model and links it. Declared
// here, defined in src/registry.cpp.
RegistrationResult Commit(State &Owner, Batch &&Pending);

} // namespace detail

// ---------------------------------------------------------------------------
// BindingRegistry
// ---------------------------------------------------------------------------

class BindingRegistry {
public:
  BindingRegistry() = default;
  explicit BindingRegistry(State *Owner) : Owner_(Owner) {}

  template <class F>
  RegistrationResult Register(std::string_view Name, F &&Callable) {
    return RegisterFunction(Name, std::forward<F>(Callable));
  }

  template <class F>
  RegistrationResult RegisterFunction(std::string_view Name, F &&Callable) {
    detail::Batch Pending;
    detail::StagedCall Entry;
    Entry.Name = std::string(Name);
    Entry.Kind = detail::Category::Function;
    Entry.Call = detail::MakeCall(std::forward<F>(Callable), std::string(Name));
    Pending.Calls.push_back(std::move(Entry));
    return CommitBatch(std::move(Pending));
  }

  NamespaceBuilder RegisterNamespace(std::string_view Qualified);

  template <class T>
  ClassBuilder<T> RegisterClass(std::string_view Name, StableTypeKey Key) {
    detail::StagedClass Staged;
    Staged.Name = std::string(Name);
    Staged.StableKey = std::string(Key.Value());
    return ClassBuilder<T>(*this, std::make_shared<detail::Batch>(),
                           std::move(Staged));
  }

  template <class E>
  EnumBuilder<E> RegisterEnum(std::string_view Name, StableTypeKey Key) {
    detail::StagedEnum Staged;
    Staged.Name = std::string(Name);
    Staged.StableKey = std::string(Key.Value());
    return EnumBuilder<E>(*this, std::make_shared<detail::Batch>(),
                          std::move(Staged));
  }

  // Publishes an instance at a name. The factory runs once, at commit.
  template <class F>
  RegistrationResult RegisterValue(std::string_view Name, F &&Factory,
                                   OwnershipPolicy Policy =
                                       OwnershipPolicy::LuaOwned()) {
    detail::Batch Pending;
    detail::StagedValue Entry;
    Entry.Name = std::string(Name);
    Entry.Policy = std::move(Policy);
    Entry.Factory = detail::MakeCall(std::forward<F>(Factory), std::string(Name));
    Pending.Values.push_back(std::move(Entry));
    return CommitBatch(std::move(Pending));
  }

  RegistrationResult CommitBatch(detail::Batch &&Pending);

  [[nodiscard]] ReflectionSnapshot Reflection() const;

  [[nodiscard]] State *Owner() const noexcept { return Owner_; }

private:
  friend class NamespaceBuilder;
  template <class T> friend class ClassBuilder;
  template <class E> friend class EnumBuilder;

  State *Owner_ = nullptr;
};

// ---------------------------------------------------------------------------
// NamespaceBuilder
// ---------------------------------------------------------------------------

class NamespaceBuilder {
public:
  NamespaceBuilder() : Staged_(std::make_shared<detail::Batch>()) {}
  NamespaceBuilder(BindingRegistry Registry, std::string Path)
      : Staged_(std::make_shared<detail::Batch>()), Registry_(Registry),
        Path_(std::move(Path)) {}

  template <class F>
  NamespaceBuilder &RegisterFunction(std::string_view Name, F &&Callable) {
    detail::StagedCall Entry;
    Entry.Name = Qualify(Name);
    Entry.Kind = detail::Category::Function;
    Entry.Call = detail::MakeCall(std::forward<F>(Callable), Qualify(Name));
    Pending().Calls.push_back(std::move(Entry));
    return *this;
  }

  NamespaceBuilder &RegisterConstant(std::string_view Name, OwnedValue Value) {
    detail::StagedConstant Entry;
    Entry.Name = Qualify(Name);
    Entry.Value = std::move(Value);
    Pending().Constants.push_back(std::move(Entry));
    return *this;
  }

  NamespaceBuilder RegisterNamespace(std::string_view Name) {
    NamespaceBuilder Child(Registry_, Qualify(Name));
    Child.Staged_ = Staged_; // shared: the root commit publishes the child too
    return Child;
  }

  template <class T>
  ClassBuilder<T> RegisterClass(std::string_view Name, StableTypeKey Key) {
    detail::StagedClass Staged;
    Staged.Name = Qualify(Name);
    Staged.StableKey = std::string(Key.Value());
    return ClassBuilder<T>(Registry_, Staged_, std::move(Staged));
  }

  template <class E>
  EnumBuilder<E> RegisterEnum(std::string_view Name, StableTypeKey Key) {
    detail::StagedEnum Staged;
    Staged.Name = Qualify(Name);
    Staged.StableKey = std::string(Key.Value());
    return EnumBuilder<E>(Registry_, Staged_, std::move(Staged));
  }

  template <class F>
  NamespaceBuilder &RegisterValue(std::string_view Name, F &&Factory,
                                  OwnershipPolicy Policy =
                                      OwnershipPolicy::LuaOwned()) {
    detail::StagedValue Entry;
    Entry.Name = Qualify(Name);
    Entry.Policy = std::move(Policy);
    Entry.Factory = detail::MakeCall(std::forward<F>(Factory), Qualify(Name));
    Pending().Values.push_back(std::move(Entry));
    return *this;
  }

  RegistrationResult Commit() {
    detail::Batch Staged = std::move(*Staged_);
    *Staged_ = detail::Batch();
    return Registry_.CommitBatch(std::move(Staged));
  }

private:
  std::string Qualify(std::string_view Name) const {
    if (Path_.empty())
      return std::string(Name);
    std::string Qualified = Path_;
    Qualified += '.';
    Qualified += Name;
    return Qualified;
  }

  detail::Batch &Pending() { return *Staged_; }

  // Shared with every child builder, so RegisterNamespace("a").RegisterFunction
  // ("b", f) stages into the same batch the root commits: one Commit publishes
  // the whole subtree, and a child builder cannot be silently orphaned.
  std::shared_ptr<detail::Batch> Staged_;
  BindingRegistry Registry_;
  std::string Path_;
};

inline NamespaceBuilder BindingRegistry::RegisterNamespace(
    std::string_view Qualified) {
  return NamespaceBuilder(*this, std::string(Qualified));
}

// ---------------------------------------------------------------------------
// ClassBuilder
// ---------------------------------------------------------------------------

template <class T> class ClassBuilder {
public:
  ClassBuilder() = default;
  ClassBuilder(BindingRegistry Registry, std::shared_ptr<detail::Batch> Shared,
               detail::StagedClass Staged)
      : Batch_(std::move(Shared)), Registry_(Registry),
        Staged_(std::move(Staged)) {
    if (!Batch_)
      Batch_ = std::make_shared<detail::Batch>();
    // The token is how the model recognises this class from a value, so it must
    // be recorded even when the class is registered outside a namespace.
    Staged_.Token = static_cast<const void *>(&detail::TypeToken<T>);
  }

  // A method: a free function or lambda whose first parameter is the receiver,
  // or a member function pointer.
  template <class F>
  ClassBuilder &Method(std::string_view Name, F &&Callable) {
    AddCall(std::forward<F>(Callable), Name, detail::Category::Method);
    return *this;
  }

  template <class F>
  ClassBuilder &StaticMethod(std::string_view Name, F &&Callable) {
    AddCall(std::forward<F>(Callable), Name, detail::Category::StaticMethod);
    return *this;
  }

  template <class F>
  ClassBuilder &Factory(std::string_view Name, F &&Callable) {
    AddCall(std::forward<F>(Callable), Name, detail::Category::Factory);
    return *this;
  }

  template <class Getter>
  ClassBuilder &Property(std::string_view Name, Getter &&Read) {
    detail::StagedProperty Staged;
    Staged.Name = std::string(Name);
    Staged.Getter = detail::MakeCall(std::forward<Getter>(Read), std::string(Name));
    Staged_.Properties.push_back(std::move(Staged));
    return *this;
  }

  template <class Getter, class Setter>
  ClassBuilder &Property(std::string_view Name, Getter &&Read, Setter &&Write) {
    detail::StagedProperty Staged;
    Staged.Name = std::string(Name);
    Staged.Getter = detail::MakeCall(std::forward<Getter>(Read), std::string(Name));
    Staged.Setter = detail::MakeCall(std::forward<Setter>(Write), std::string(Name));
    Staged.Writable = true;
    Staged_.Properties.push_back(std::move(Staged));
    return *this;
  }

  // Property with a native write hook, run after a successful write. Removes the
  // need for a notifying builder wrapper at the call site.
  template <class Getter, class Setter, class Hook>
  ClassBuilder &Property(std::string_view Name, Getter &&Read, Setter &&Write,
                         NotifyPolicy<Hook> Policy) {
    const std::string PropertyName(Name);

    // Monomorphic on purpose: the wrapper is erased the same way a user
    // callable is, so it needs a deducible signature.
    using SetterFn = typename detail::Sig<std::decay_t<Setter>>::Fn;
    using Value = typename detail::SetterSlot<SetterFn>::Type;

    auto Notifying = [Inner = std::forward<Setter>(Write),
                      HookFn = std::move(Policy.Callback),
                      PropertyName](T &Self, Value Written) mutable {
      Inner(Self, std::forward<Value>(Written));
      HookFn(Self, std::string_view(PropertyName));
    };

    detail::StagedProperty Staged;
    Staged.Name = std::string(Name);
    Staged.Getter = detail::MakeCall(std::forward<Getter>(Read), PropertyName);
    Staged.Setter = detail::MakeCall(std::move(Notifying), PropertyName);
    Staged.Writable = true;
    Staged_.Properties.push_back(std::move(Staged));
    return *this;
  }

  template <class M> ClassBuilder &Field(std::string_view Name, M T::*Member) {
    detail::StagedField Staged;
    Staged.Name = std::string(Name);
    Staged.Getter = detail::MakeCall(
        [Member](const T &Self) -> M { return Self.*Member; },
        std::string(Name));
    Staged.Setter = detail::MakeCall(
        [Member](T &Self, M Value) { Self.*Member = std::move(Value); },
        std::string(Name));
    Staged_.Fields.push_back(std::move(Staged));
    return *this;
  }

  template <class F>
  ClassBuilder &Operator(ClassOperator Operation, F &&Callable) {
    detail::StagedOperator Staged;
    Staged.Slot = MapOperator(Operation);
    Staged.Call = detail::MakeCall(std::forward<F>(Callable),
                                   Staged_.Name + ".__operator");
    Staged_.Operators.push_back(std::move(Staged));
    return *this;
  }

  ClassBuilder &Constant(std::string_view Name, OwnedValue Value) {
    detail::StagedConstant Entry;
    Entry.Name = std::string(Name);
    Entry.Value = std::move(Value);
    Staged_.Constants.push_back(std::move(Entry));
    return *this;
  }

  RegistrationResult Commit() {
    if (Staged_.StableKey.empty())
      Staged_.StableKey = Staged_.Name;
    // Stages into the batch this builder shares with its namespace, so a class
    // committed inside a namespace publishes the namespace's own entries too.
    Batch_->Classes.push_back(std::move(Staged_));
    Staged_ = detail::StagedClass{};

    detail::Batch Published = std::move(*Batch_);
    *Batch_ = detail::Batch();
    return Registry_.CommitBatch(std::move(Published));
  }

private:
  template <class F>
  void AddCall(F &&Callable, std::string_view Name, detail::Category Kind) {
    detail::StagedCall Entry;
    Entry.Name = std::string(Name);
    Entry.Kind = Kind;
    Entry.Call = detail::MakeCall(std::forward<F>(Callable), Staged_.Name + "." +
                                                               std::string(Name));
    Staged_.Calls.push_back(std::move(Entry));
  }

  static detail::OperatorSlot MapOperator(ClassOperator Operation) {
    using detail::OperatorSlot;
    switch (Operation) {
    case ClassOperator::Add: return OperatorSlot::Add;
    case ClassOperator::Subtract: return OperatorSlot::Subtract;
    case ClassOperator::Multiply: return OperatorSlot::Multiply;
    case ClassOperator::Divide: return OperatorSlot::Divide;
    case ClassOperator::Modulo: return OperatorSlot::Modulo;
    case ClassOperator::Power: return OperatorSlot::Power;
    case ClassOperator::UnaryMinus: return OperatorSlot::Negate;
    case ClassOperator::Equal: return OperatorSlot::Equal;
    case ClassOperator::Less: return OperatorSlot::Less;
    case ClassOperator::LessOrEqual: return OperatorSlot::LessOrEqual;
    case ClassOperator::Concatenate: return OperatorSlot::Concat;
    case ClassOperator::Length: return OperatorSlot::Len;
    case ClassOperator::ToText: return OperatorSlot::ToString;
    case ClassOperator::Call: return OperatorSlot::Call;
    case ClassOperator::Iterate: return OperatorSlot::Iterate;
    case ClassOperator::Index: return OperatorSlot::Index;
    case ClassOperator::NewIndex: return OperatorSlot::NewIndex;
    }
    return OperatorSlot::Add;
  }

  BindingRegistry Registry_;
  std::shared_ptr<detail::Batch> Batch_;
  detail::StagedClass Staged_;
};

// ---------------------------------------------------------------------------
// EnumBuilder
// ---------------------------------------------------------------------------

template <class E> class EnumBuilder {
public:
  EnumBuilder() = default;
  EnumBuilder(BindingRegistry Registry, std::shared_ptr<detail::Batch> Shared,
              detail::StagedEnum Staged)
      : Batch_(std::move(Shared)), Registry_(Registry),
        Staged_(std::move(Staged)) {
    if (!Batch_)
      Batch_ = std::make_shared<detail::Batch>();
  }

  EnumBuilder &Value(std::string_view Name, E Value) {
    detail::StagedEnumValue Entry;
    Entry.Name = std::string(Name);
    Entry.Value = static_cast<std::int64_t>(
        static_cast<std::underlying_type_t<E>>(Value));
    Staged_.Values.push_back(std::move(Entry));
    return *this;
  }

  EnumBuilder &AsObjects(bool Enabled = true) {
    Staged_.AsObjects = Enabled;
    return *this;
  }

  EnumBuilder &Alias(std::string_view AliasName, std::string_view Existing) {
    Staged_.Aliases.emplace_back(std::string(AliasName), std::string(Existing));
    return *this;
  }

  RegistrationResult Commit() {
    if (Staged_.StableKey.empty())
      Staged_.StableKey = Staged_.Name;
    Batch_->Enums.push_back(std::move(Staged_));
    Staged_ = detail::StagedEnum{};

    detail::Batch Published = std::move(*Batch_);
    *Batch_ = detail::Batch();
    return Registry_.CommitBatch(std::move(Published));
  }

private:
  BindingRegistry Registry_;
  std::shared_ptr<detail::Batch> Batch_;
  detail::StagedEnum Staged_;
};

} // namespace luna

