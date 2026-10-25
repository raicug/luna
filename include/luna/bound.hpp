#pragma once

// Callables, builders and the registry: everything a host registers.

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
TypeId ClassIdForToken(lua_State *L, const void *Token) noexcept;

// Distance from a T to its B subobject, measured on real storage because an
// optimiser folds the null-pointer trick to zero.
template <class T, class B> std::ptrdiff_t BaseOffsetOf() {
  static const std::ptrdiff_t Offset = [] {
    struct Aligned {
      alignas(T) unsigned char Bytes[sizeof(T)];
    };
    static Aligned Storage{};
    T *Derived = reinterpret_cast<T *>(&Storage);
    B *Base = static_cast<B *>(Derived);
    return reinterpret_cast<char *>(Base) - reinterpret_cast<char *>(Derived);
  }();
  return Offset;
}

using InvokeThunk = int (*)(lua_State *, void *);
using AcceptThunk = bool (*)(lua_State *, const void *);

struct Target {
  void (*Push)(lua_State *, const void *) = nullptr;
  void *(*Context)(lua_State *, const void *) = nullptr;
  InvokeThunk Thunk = nullptr;
  AcceptThunk Accept = nullptr;
  std::shared_ptr<const void> Ctx;

  // Signature shape, for reflection and for generated definitions.
  int Arity = -1;
  int Required = -1;

  // Class tokens in the signature, so Commit can refuse a callable whose class
  // is not registered instead of leaving the parameter unchecked.
  std::vector<const void *> Needs;
  [[nodiscard]] bool Valid() const noexcept { return Push != nullptr; }

  [[nodiscard]] bool Direct() const noexcept {
    return Thunk != nullptr && Context != nullptr;
  }
};

// Must stay the first member of Callable: __index dispatches through it.
struct CallableHeader {
  InvokeThunk Thunk = nullptr;
  AcceptThunk Accept = nullptr;
};

template <class A> constexpr const void *TokenOf() {
  using Subject = typename ClassSubject<A>::Type;
  if constexpr (IsClassType<Subject>)
    return static_cast<const void *>(&TypeToken<Subject>);
  else
    return nullptr;
}

// Every registered class a parameter can reach, through containers as well.
template <class A> void AppendNeeds(std::vector<const void *> &Out) {
  using B = UnrefT<A>;
  if constexpr (CustomCodec<B>::Enabled) {
    return;
  } else if constexpr (IsOptional<B>::value || IsVector<B>::value) {
    AppendNeeds<typename B::value_type>(Out);
  } else if constexpr (IsMap<B>::value) {
    AppendNeeds<typename B::key_type>(Out);
    AppendNeeds<typename B::mapped_type>(Out);
  } else if constexpr (IsTupleLike<B>::value) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (AppendNeeds<std::tuple_element_t<I, B>>(Out), ...);
    }(std::make_index_sequence<std::tuple_size_v<B>>{});
  } else if constexpr (ClassV<A>) {
    Out.push_back(
        static_cast<const void *>(&TypeToken<typename ClassSubject<A>::Type>));
  }
}

template <class A>
inline constexpr bool OptionalParam =
    IsOptional<UnrefT<A>>::value || IsArgumentPackV<UnrefT<A>>;

template <class R, class... A> struct Callable : CallableHeader {
  static constexpr std::size_t Count = sizeof...(A);
  static constexpr std::size_t Required =
      ((OptionalParam<A> ? 0u : 1u) + ... + 0u);
  static constexpr bool Variadic = (IsArgumentPackV<UnrefT<A>> || ...);

  // Once a parameter may be left out, every one after it may be too.
  static constexpr bool OptionalIsTrailing = [] {
    bool Seen = false;
    bool Ok = true;
    ((Seen = Seen || OptionalParam<A>, Ok = Ok && (!Seen || OptionalParam<A>)),
     ...);
    (void)Seen;
    return Ok;
  }();
  static_assert(OptionalIsTrailing,
                "luna: a std::optional or ArgumentPack parameter must come "
                "after every required one");
  std::function<R(A...)> Fn;
  const void *Tokens[Count ? Count : 1]{};
  TypeId Ids[Count ? Count : 1]{};
  std::string Name;

  Callable() {
    Thunk = &Callable::Dispatch;
    Accept = &Callable::Accepts;
  }

  static void FillTokens(const void **Out) {
    std::size_t Slot = 0;
    ((Out[Slot++] = TokenOf<A>()), ...);
  }

  static void CollectNeeds(std::vector<const void *> &Out) {
    (AppendNeeds<A>(Out), ...);
  }

  void Resolve(lua_State *L) {
    for (std::size_t Index = 0; Index < Count; ++Index)
      Ids[Index] = Tokens[Index] ? ClassIdForToken(L, Tokens[Index]) : NoType;
  }

  static bool Accepts(lua_State *L, const void *Context) {
    auto *Self = static_cast<const Callable *>(Context);
    const int Top = lua_gettop(L);
    if (Top < static_cast<int>(Required))
      return false;
    if (Top > static_cast<int>(Count) && !Variadic)
      return false;
    return AcceptEach(L, Self->Ids, std::index_sequence_for<A...>{});
  }

  static int Dispatch(lua_State *L, void *Context) {
    auto *Self = static_cast<Callable *>(Context);
    std::string Error;
    int Pushed = -1;
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
      // A callable that asked to suspend and then failed must not leave the
      // request behind for whatever runs next.
      ClearSuspensionRequest();
      luaL_error(L, "%s", Error.c_str());
      return 0;
    }

    if (SuspensionRequested) {
      // Only a coroutine run by the scheduler can wait. Anywhere else (a
      // handler called from the host, a metamethod) the request is refused, not
      // dropped.
      if (!lua_isyieldable(L)) {
        ClearSuspensionRequest();
        luaL_error(L,
                   "%s: cannot wait here, because this call is not running "
                   "in a task that can yield",
                   Self->Name.c_str());
        return 0;
      }
      if (CompleteSuspension(L))
        return lua_yield(L, Pushed);
    }
    return Pushed;
  }

private:
  template <std::size_t... I>
  static bool AcceptEach(lua_State *L, const TypeId *Ids,
                         std::index_sequence<I...>) {
    bool Ok = true;
    ((Ok = Ok && (static_cast<int>(I) + 1 > lua_gettop(L) ||
                  AcceptSlot<std::tuple_element_t<I, std::tuple<A...>>>(
                      L, static_cast<int>(I) + 1, Ids[I]))),
     ...);
    return Ok;
  }

public:
  static int Call(lua_State *L) {
    return Dispatch(L, lua_touserdata(L, lua_upvalueindex(1)));
  }
};

template <class A> constexpr const void *TokenOf();

template <class C> void DestroyCallable(void *Memory) {
  static_cast<C *>(Memory)->~C();
}

template <class C> void PushCallable(lua_State *L, const void *Raw) {
  void *Memory = lua_newuserdatadtor(L, sizeof(C), &DestroyCallable<C>);
  auto *Ctx = new (Memory) C(*static_cast<const C *>(Raw));
  Ctx->Resolve(L);
  luna::detail::PushClosure(L, &C::Call, nullptr, 1);
}

template <class C> void *PushContext(lua_State *L, const void *Raw) {
  void *Memory = lua_newuserdatadtor(L, sizeof(C), &DestroyCallable<C>);
  auto *Ctx = new (Memory) C(*static_cast<const C *>(Raw));
  Ctx->Resolve(L);
  return Ctx;
}

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

template <class C, class R, class... A> struct Sig<R (C::*)(A...) noexcept> {
  using Fn = std::function<R(C &, A...)>;
  using Ctx = Callable<R, C &, A...>;
  static Fn Make(R (C::*Function)(A...) noexcept) {
    return [Function](C &Self, A... Args) -> R {
      return (Self.*Function)(std::forward<A>(Args)...);
    };
  }
};

template <class C, class R, class... A>
struct Sig<R (C::*)(A...) const noexcept> {
  using Fn = std::function<R(const C &, A...)>;
  using Ctx = Callable<R, const C &, A...>;
  static Fn Make(R (C::*Function)(A...) const noexcept) {
    return [Function](const C &Self, A... Args) -> R {
      return (Self.*Function)(std::forward<A>(Args)...);
    };
  }
};

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

template <class Erased> struct SetterSlot;
template <class R, class Self, class V, class... Rest>
struct SetterSlot<std::function<R(Self, V, Rest...)>> {
  using Type = V;
};

template <class F> Target MakeCall(F &&Callable, std::string Name) {
  using S = SigOf<F>;
  using C = typename S::Ctx;
  static_assert(!std::is_void_v<C>,
                "luna: the signature of this callable cannot be read. Pass a "
                "function pointer, a member function pointer, or a lambda or "
                "functor with a single non-template operator(); a generic "
                "lambda has none");
  auto Ctx = std::make_shared<C>();
  Ctx->Fn = S::Make(std::forward<F>(Callable));
  Ctx->Name = std::move(Name);
  C::FillTokens(Ctx->Tokens);
  Target Built;
  C::CollectNeeds(Built.Needs);
  Built.Ctx = Ctx;
  Built.Push = &PushCallable<C>;
  Built.Context = &PushContext<C>;
  Built.Thunk = &C::Dispatch;
  Built.Accept = &C::Accepts;
  Built.Arity = static_cast<int>(C::Count);
  Built.Required = static_cast<int>(C::Required);
  return Built;
}
} // namespace detail

class DelegateCallResult {
public:
  DelegateCallResult() = default;
  explicit DelegateCallResult(std::string Failed) : Error_(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept { return Error_.empty(); }
  [[nodiscard]] const std::string &Diagnostic() const noexcept {
    return Error_;
  }

private:
  std::string Error_;
};

template <class Signature> class Delegate;

template <class R, class... A> class Delegate<R(A...)> {
public:
  struct Handle {
    void *VM = nullptr;
    int Ref = 0;
    std::shared_ptr<detail::Liveness> Life;
    ~Handle() {
      if (VM && Ref && (!Life || Life->Alive.load(std::memory_order_acquire)))
        detail::Unpin(static_cast<lua_State *>(VM), Ref);
    }
  };
  Delegate() = default;
  [[nodiscard]] bool IsBound() const noexcept {
    return Shared && Shared->Ref != 0 && Shared->VM != nullptr &&
           (!Shared->Life ||
            Shared->Life->Alive.load(std::memory_order_acquire));
  }

  static Delegate Capture(void *VM, int StackIndex) {
    auto *L = static_cast<lua_State *>(VM);
    auto Owned = std::make_shared<Handle>();
    // L is the coroutine running the script, and it is collected once its task
    // ends. The handler may outlive that task, so it is anchored to the main
    // thread, which lives as long as the state.
    Owned->VM = static_cast<void *>(lua_mainthread(L));
    Owned->Life = luna::detail::LivenessOf(VM);
    lua_pushvalue(L, StackIndex);
    Owned->Ref = luna::detail::PinTop(L);
    Delegate Result;
    Result.Shared = std::move(Owned);
    return Result;
  }

  DelegateCallResult Invoke(A... Args) const;

  // Runs the function as a task of its own, so it may wait. Spawn runs it to
  // its first yield now, Defer starts it at the next tick, and SpawnAfter after
  // a delay. Each returns the task handle, or 0 if the delegate is not bound.
  std::uint64_t Spawn(A... Args) const {
    return Start(detail::StartMode::Now, 0.0, Args...);
  }
  std::uint64_t Defer(A... Args) const {
    return Start(detail::StartMode::Next, 0.0, Args...);
  }
  std::uint64_t SpawnAfter(double Seconds, A... Args) const {
    return Start(detail::StartMode::After, Seconds, Args...);
  }

private:
  std::uint64_t Start(detail::StartMode Mode, double Delay, A... Args) const {
    if (!IsBound())
      return 0;
    const int Ref = Shared->Ref;
    return detail::StartTask(
        static_cast<lua_State *>(Shared->VM), Mode, Delay, [&](lua_State *Co) {
          lua_checkstack(Co, static_cast<int>(sizeof...(A)) * 4 + 8);
          lua_rawgeti(Co, LUA_REGISTRYINDEX, Ref);
          int Pushed = 0;
          ((Pushed += detail::WriteValue(Co, Args, NoType)), ...);
          return Pushed;
        });
  }

  std::shared_ptr<Handle> Shared;
};

template <class R, class... A>
DelegateCallResult Delegate<R(A...)>::Invoke(A... Args) const {
  if (!IsBound())
    return DelegateCallResult("the handler is no longer available");

  auto *Main = static_cast<lua_State *>(Shared->VM);
  const int Base = lua_gettop(Main);
  // A thread of its own, anchored on the main stack until the call returns, so
  // the handler never runs on (or inside the continuation of) a task's
  // coroutine.
  lua_State *Thread = lua_newthread(Main);
  lua_checkstack(Thread, static_cast<int>(sizeof...(A)) * 4 + 8);
  lua_rawgeti(Thread, LUA_REGISTRYINDEX, Shared->Ref);
  int Pushed = 0;
  auto PushOne = [&](auto &&Value) {
    Pushed += detail::WriteValue(Thread, Value, NoType);
  };
  try {
    (PushOne(static_cast<A>(Args)), ...);
  } catch (const std::exception &Thrown) {
    lua_settop(Main, Base);
    return DelegateCallResult(Thrown.what());
  }
  const int Status = lua_pcall(Thread, Pushed, 0, 0);
  if (Status != 0) {
    const char *Message = lua_tostring(Thread, -1);
    std::string Text = Message ? Message : "handler raised an error";
    lua_settop(Main, Base);
    return DelegateCallResult(std::move(Text));
  }
  lua_settop(Main, Base);
  return DelegateCallResult();
}

namespace detail {
template <class T> struct IsDelegate : std::false_type {};
template <class T> struct IsDelegate<Delegate<T>> : std::true_type {};

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

  static bool Accepts(lua_State *L, int Index) {
    return lua_type(L, Index) == LUA_TFUNCTION;
  }

  static int Write(lua_State *L, const Delegate<R(A...)> &) {
    lua_pushnil(L);
    return 1;
  }
};
} // namespace detail

template <class Hook> struct NotifyPolicy {
  Hook Callback;
};

class PropertyPolicy {
public:
  template <class Hook>
  static NotifyPolicy<std::decay_t<Hook>> Notify(Hook &&Callback) {
    return NotifyPolicy<std::decay_t<Hook>>{std::forward<Hook>(Callback)};
  }
};

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
  std::string TypeName;
  const void *Token = nullptr;
  std::string BaseName;
  std::shared_ptr<LifetimeHandle::Control> Lifetime;
  const void *BaseToken = nullptr;
  std::ptrdiff_t BaseOffset = 0;
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

RegistrationResult Commit(Runtime &Core, Batch &&Pending);
} // namespace detail

class BindingRegistry {
public:
  BindingRegistry() = default;
  BindingRegistry(detail::Runtime *Core, std::shared_ptr<detail::Liveness> Life)
      : Runtime_(Core), Life_(std::move(Life)) {}

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

  template <class F>
  RegistrationResult
  RegisterValue(std::string_view Name, F &&Factory,
                OwnershipPolicy Policy = OwnershipPolicy::LuaOwned()) {
    detail::Batch Pending;
    detail::StagedValue Entry;
    Entry.Name = std::string(Name);
    Entry.Policy = std::move(Policy);
    Entry.Factory =
        detail::MakeCall(std::forward<F>(Factory), std::string(Name));
    Pending.Values.push_back(std::move(Entry));
    return CommitBatch(std::move(Pending));
  }

  RegistrationResult CommitBatch(detail::Batch &&Pending);
  [[nodiscard]] ReflectionSnapshot Reflection() const;
  // The State this registry belongs to, or null once it is gone.
  [[nodiscard]] State *Owner() const noexcept;

private:
  friend class NamespaceBuilder;
  template <class T> friend class ClassBuilder;
  template <class E> friend class EnumBuilder;
  // The runtime outlives a move of its State, and the token says if it is gone.
  detail::Runtime *Runtime_ = nullptr;
  std::shared_ptr<detail::Liveness> Life_;
};

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
    Child.Staged_ = Staged_;
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
  NamespaceBuilder &
  RegisterValue(std::string_view Name, F &&Factory,
                OwnershipPolicy Policy = OwnershipPolicy::LuaOwned()) {
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

  std::shared_ptr<detail::Batch> Staged_;
  BindingRegistry Registry_;
  std::string Path_;
};

inline NamespaceBuilder
BindingRegistry::RegisterNamespace(std::string_view Qualified) {
  return NamespaceBuilder(*this, std::string(Qualified));
}

template <class T> class ClassBuilder {
public:
  ClassBuilder() = default;
  ClassBuilder(BindingRegistry Registry, std::shared_ptr<detail::Batch> Shared,
               detail::StagedClass Staged)
      : Batch_(std::move(Shared)), Registry_(Registry),
        Staged_(std::move(Staged)) {
    if (!Batch_)
      Batch_ = std::make_shared<detail::Batch>();

    Staged_.Token = static_cast<const void *>(&detail::TypeToken<T>);
  }

  template <class F> ClassBuilder &Method(std::string_view Name, F &&Callable) {
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
    Staged.Getter =
        detail::MakeCall(std::forward<Getter>(Read), std::string(Name));
    Staged_.Properties.push_back(std::move(Staged));
    return *this;
  }

  template <class Getter, class Setter>
  ClassBuilder &Property(std::string_view Name, Getter &&Read, Setter &&Write) {
    detail::StagedProperty Staged;
    Staged.Name = std::string(Name);
    Staged.Getter =
        detail::MakeCall(std::forward<Getter>(Read), std::string(Name));
    Staged.Setter =
        detail::MakeCall(std::forward<Setter>(Write), std::string(Name));
    Staged.Writable = true;
    Staged_.Properties.push_back(std::move(Staged));
    return *this;
  }

  template <class Getter, class Setter, class Hook>
  ClassBuilder &Property(std::string_view Name, Getter &&Read, Setter &&Write,
                         NotifyPolicy<Hook> Policy) {
    const std::string PropertyName(Name);
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
    Staged.Getter =
        detail::MakeCall([Member](const T &Self) -> M { return Self.*Member; },
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

  // Instances of T that a bound function returns by pointer or reference are
  // borrowed. This ties them to Handle, so once it is invalidated a script that
  // still holds one gets "the instance is no longer available", not a dangling
  // read.
  ClassBuilder &BorrowedFrom(const LifetimeHandle &Handle) {
    Staged_.Lifetime = Handle.ControlBlock();
    return *this;
  }

  // What typeof reports for an instance, when it should not be the class name.
  ClassBuilder &TypeName(std::string_view Name) {
    Staged_.TypeName = std::string(Name);
    return *this;
  }

  // Inherits the base's methods, properties and fields; the base must be
  // registered first.
  template <class B> ClassBuilder &BaseOf(std::string_view BaseName) {
    static_assert(std::is_base_of_v<B, T>, "BaseOf requires a base of T");
    Staged_.BaseName = std::string(BaseName);
    Staged_.BaseToken = static_cast<const void *>(&detail::TypeToken<B>);
    Staged_.BaseOffset = detail::BaseOffsetOf<T, B>();
    return *this;
  }

  RegistrationResult Commit() {
    if (Staged_.StableKey.empty())
      Staged_.StableKey = Staged_.Name;

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
    Entry.Call = detail::MakeCall(std::forward<F>(Callable),
                                  Staged_.Name + "." + std::string(Name));
    Staged_.Calls.push_back(std::move(Entry));
  }

  static detail::OperatorSlot MapOperator(ClassOperator Operation) {
    using detail::OperatorSlot;
    switch (Operation) {
    case ClassOperator::Add:
      return OperatorSlot::Add;
    case ClassOperator::Subtract:
      return OperatorSlot::Subtract;
    case ClassOperator::Multiply:
      return OperatorSlot::Multiply;
    case ClassOperator::Divide:
      return OperatorSlot::Divide;
    case ClassOperator::Modulo:
      return OperatorSlot::Modulo;
    case ClassOperator::Power:
      return OperatorSlot::Power;
    case ClassOperator::UnaryMinus:
      return OperatorSlot::Negate;
    case ClassOperator::Equal:
      return OperatorSlot::Equal;
    case ClassOperator::Less:
      return OperatorSlot::Less;
    case ClassOperator::LessOrEqual:
      return OperatorSlot::LessOrEqual;
    case ClassOperator::Concatenate:
      return OperatorSlot::Concat;
    case ClassOperator::Length:
      return OperatorSlot::Len;
    case ClassOperator::ToText:
      return OperatorSlot::ToString;
    case ClassOperator::Call:
      return OperatorSlot::Call;
    case ClassOperator::Iterate:
      return OperatorSlot::Iterate;
    case ClassOperator::Index:
      return OperatorSlot::Index;
    case ClassOperator::NewIndex:
      return OperatorSlot::NewIndex;
    }
    return OperatorSlot::Add;
  }

  BindingRegistry Registry_;
  std::shared_ptr<detail::Batch> Batch_;
  detail::StagedClass Staged_;
};

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
