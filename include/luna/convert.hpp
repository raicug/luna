#pragma once

// luna : argument and return codecs.
//
// One customisation point per type. A codec is a compile-time plan, so the thunk
// generated for a signature inlines the whole read/write path instead of walking
// a run-time descriptor.
//
// Boundary note: this header includes the Luau C API, because a template that
// reads a value must be able to call lua_tonumberx in the consumer's translation
// unit. "Inline codecs" and "no Luau header in any public include" are mutually
// exclusive; Luna chose the second and paid for it with six descriptor/adapter
// headers and a 40 KB trampoline. luna takes the first: Luau's C API is
// reachable through this header, and CMake propagates it as a SYSTEM include.
// No consumer writes a Luau type or calls a stack function.

#include <luna/fwd.hpp>
#include <luna/value.hpp>

#include <lua.h>
#include <lualib.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace luna {
namespace detail {

// ---------------------------------------------------------------------------
// The instance block
// ---------------------------------------------------------------------------
//
// Every bound instance is one userdata holding this, so a read costs a tag
// check and a pointer chase rather than an allocation. It is `detail`, but its
// layout is written down here because the hot read path is inline: a template
// that reads an instance argument must be able to inspect it without a call.

inline constexpr std::uint32_t kObjectMagic = 0x4C554E41u; // "LUNA"

enum class ObjectTag : std::uint8_t {
  Borrowed = 0, // a pointer the host owns; may be invalidated
  Shared = 1,   // a shared_ptr the block owns
  Owned = 2,    // a copy the block owns
};

struct Object {
  std::uint32_t Magic;
  TypeId ClassId;
  std::uint8_t Tag;
  std::uint8_t Padding[3];
  void *Pointer;                   // the subject, always valid when Alive
  void (*Destroy)(void *);         // owned only
  void *Control;                   // borrowed: LifetimeHandle::Control*
  std::shared_ptr<void> *Shared;   // shared only
};

// ---------------------------------------------------------------------------
// Suspension
// ---------------------------------------------------------------------------
//
// A native callable asks to suspend by setting these, then returns; the
// trampoline turns that into lua_yield. They are inline so the test is a
// thread-local load and a branch, not a call across a translation unit.

inline thread_local bool SuspensionRequested = false;
inline thread_local double SuspensionDelay = 0.0;

// Declared here, defined in src/state.cpp: records the due time against the
// running task. Only reached when SuspensionRequested is set.
bool CompleteSuspension(lua_State *L);

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

template <class T> struct Unref {
  using Type = std::remove_cv_t<std::remove_reference_t<T>>;
};
template <class T> using UnrefT = typename Unref<T>::Type;

template <class T> struct IsSharedPtr : std::false_type {};
template <class T> struct IsSharedPtr<std::shared_ptr<T>> : std::true_type {};

template <class T> struct SharedInner;
template <class T> struct SharedInner<std::shared_ptr<T>> {
  using Type = std::remove_cv_t<T>;
};

template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};

template <class T> struct IsVector : std::false_type {};
template <class T> struct IsVector<std::vector<T>> : std::true_type {};

template <class T> struct IsPair : std::false_type {};
template <class T, class U> struct IsPair<std::pair<T, U>> : std::true_type {};

template <class T> struct IsStringLike : std::false_type {};
template <> struct IsStringLike<std::string> : std::true_type {};
template <> struct IsStringLike<std::string_view> : std::true_type {};

// Lazy shared_ptr unwrapping: std::conditional_t evaluates both branches, so
// SharedInner must not be instantiated for a type that is not a shared_ptr.
template <class T, bool = IsSharedPtr<T>::value> struct SharedOr {
  using Type = T;
};
template <class T> struct SharedOr<T, true> {
  using Type = typename SharedInner<T>::Type;
};

// The class a signature position refers to, in any spelling.
template <class A> struct ClassSubject {
  using Bare = UnrefT<A>;
  using Ptr = std::remove_cv_t<std::remove_pointer_t<Bare>>;
  using Type = std::conditional_t<std::is_pointer_v<Bare>, Ptr,
                                  typename SharedOr<Bare>::Type>;
  static constexpr bool Is = RegisteredClassTrait<Type>::value;
};

template <class A> inline constexpr bool ClassV = ClassSubject<A>::Is;

// A type outside this header enables a codec by specialising this.
template <class T, class = void> struct CustomCodec {
  static constexpr bool Enabled = false;
};

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Describes a mismatch. Messages do not name the argument; the caller prefixes
// the position, so the success path builds no string.
struct ArgError {
  std::string Text;

  [[nodiscard]] bool Failed() const noexcept { return !Text.empty(); }
  void Set(std::string Message) {
    if (Text.empty())
      Text = std::move(Message);
  }
};

inline const char *LuaTypeName(lua_State *L, int Index) {
  return lua_typename(L, lua_type(L, Index));
}

// This Luau fork exposes lua_ref/lua_unref instead of luaL_ref/luaL_unref: there
// is no table argument (the registry is implicit) and lua_ref does not pop.
// These two restore the pop-and-pin shape the rest of the library assumes.
inline int PinTop(lua_State *L) {
  const int Ref = lua_ref(L, -1);
  lua_pop(L, 1);
  return Ref;
}

inline void Unpin(lua_State *L, int Ref) noexcept { lua_unref(L, Ref); }

// lua_pushcclosure in this fork carries a debug name, which is what makes a
// bound function show a useful name in a traceback.
inline void PushClosure(lua_State *L, lua_CFunction Fn, const char *Name,
                        int Upvalues) {
  lua_pushcclosurek(L, Fn, Name, Upvalues, nullptr);
}

// ---------------------------------------------------------------------------
// Out-of-line VM interaction, defined in src/
// ---------------------------------------------------------------------------

bool ReadUserdataPointer(lua_State *L, int Index, TypeId ClassId, void **Out,
                         std::string &Why);
bool StoreSharedPointer(lua_State *L, int Index, TypeId ClassId,
                        std::shared_ptr<void> &Out, std::string &Why);

// The hot form of ReadUserdataPointer: no strings, no out-of-line call. Returns
// false on any mismatch, and the caller then calls the diagnostic version to
// find out why.
inline bool ReadInstancePointer(lua_State *L, int Index, TypeId ClassId,
                                void **Out) noexcept {
  if (lua_type(L, Index) != LUA_TUSERDATA)
    return false;

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic)
    return false;
  if (ClassId != NoType && Instance->ClassId != ClassId)
    return false;

  if (static_cast<ObjectTag>(Instance->Tag) == ObjectTag::Borrowed &&
      Instance->Control) {
    auto *Control = static_cast<LifetimeHandle::Control *>(Instance->Control);
    if (!Control->Alive.load(std::memory_order_acquire))
      return false;
  }

  *Out = Instance->Pointer;
  return true;
}

bool PushBorrowedInstance(lua_State *L, const void *TypeToken, void *Object,
                          const void *LifetimeControl);
bool PushSharedInstance(lua_State *L, const void *TypeToken,
                        std::shared_ptr<void> Object);
bool PushOwnedInstance(lua_State *L, const void *TypeToken, void *Object,
                       void (*Destroy)(void *));

int PushOwnedValue(lua_State *L, const OwnedValue &Value);
int PushValuePack(lua_State *L, const ValuePack &Pack);

bool SetTableField(lua_State *L, int TableIndex, const char *Name);

// ---------------------------------------------------------------------------
// Argument storage
// ---------------------------------------------------------------------------
//
// A slot holds the C++ object the callable receives. A class reached through a
// reference binds straight to the userdata payload, so no copy happens on the
// hot path; everything else is stored by value.

template <class A>
inline constexpr bool SlotIsPointer = std::is_lvalue_reference_v<A> && ClassV<A>;

template <class A>
using SlotType = std::conditional_t<SlotIsPointer<A>, void *,
                                    std::remove_cv_t<std::remove_reference_t<A>>>;

// Presents a slot in the spelling the signature declared.
template <class A, class S> decltype(auto) PassSlot(S &Stored) {
  if constexpr (SlotIsPointer<A>)
    return *static_cast<std::remove_reference_t<A> *>(Stored);
  else if constexpr (std::is_reference_v<A>)
    return static_cast<A>(Stored);
  else
    return std::move(Stored);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

template <class A>
bool ReadValue(lua_State *L, int Index, SlotType<A> &Out, TypeId ClassId,
               ArgError &Error);

template <class A>
bool ReadNumber(lua_State *L, int Index, A &Out, ArgError &Error) {
  int IsNumber = 0;
  const double Value = lua_tonumberx(L, Index, &IsNumber);
  if (!IsNumber) {
    Error.Set(std::string("expected a number, got ") + LuaTypeName(L, Index));
    return false;
  }
  if constexpr (std::is_integral_v<A>) {
    const bool InRange =
        Value >= static_cast<double>(std::numeric_limits<A>::min()) &&
        Value <= static_cast<double>(std::numeric_limits<A>::max());
    if (!InRange || std::floor(Value) != Value) {
      Error.Set("expected an integer, got a non-integral or out-of-range value");
      return false;
    }
  }
  Out = static_cast<A>(Value);
  return true;
}

template <class X>
bool ReadSequence(lua_State *L, int Index, std::vector<X> &Out, TypeId ClassId,
                  ArgError &Error) {
  if (lua_type(L, Index) != LUA_TTABLE) {
    Error.Set(std::string("expected a table, got ") + LuaTypeName(L, Index));
    return false;
  }
  const int Length = static_cast<int>(lua_objlen(L, Index));
  Out.clear();
  Out.reserve(static_cast<std::size_t>(Length));
  for (int Element = 1; Element <= Length; ++Element) {
    lua_rawgeti(L, Index, Element);
    SlotType<X> Item{};
    const int Top = lua_gettop(L);
    const bool Ok = ReadValue<X>(L, Top, Item, ClassId, Error);
    lua_pop(L, 1);
    if (!Ok)
      return false;
    Out.push_back(static_cast<X>(std::move(Item)));
  }
  return true;
}

// Declared but never defined: instantiating it is a compile error whose text
// names the unsupported parameter, which is the useful half of the diagnostic.
template <class T> struct UnsupportedParameterType;

template <class A>
bool ReadValue(lua_State *L, int Index, SlotType<A> &Out, TypeId ClassId,
               ArgError &Error) {
  using B = UnrefT<A>;
  using S = SlotType<A>;

  if constexpr (CustomCodec<B>::Enabled) {
    return CustomCodec<B>::Read(L, Index, Out, Error);
  } else if constexpr (IsArgumentPackV<B>) {
    const int Top = lua_gettop(L);
    const int Count = Top >= Index ? Top - Index + 1 : 0;
    Out = ArgumentPack(static_cast<void *>(L), Index, Count);
    return true;
  } else if constexpr (IsOwnedValueV<B>) {
    Out = ArgumentPack(static_cast<void *>(L), Index, 1).At(0);
    return true;
  } else if constexpr (IsValueV<B>) {
    Out = ArgumentPack(static_cast<void *>(L), Index, 1).View(0);
    return true;
  } else if constexpr (IsOptional<B>::value) {
    using X = typename B::value_type;
    if (lua_isnoneornil(L, Index)) {
      Out.reset();
      return true;
    }
    SlotType<X> Item{};
    if (!ReadValue<X>(L, Index, Item, ClassId, Error))
      return false;
    Out = std::move(Item);
    return true;
  } else if constexpr (std::is_same_v<B, bool>) {
    if (lua_type(L, Index) != LUA_TBOOLEAN) {
      Error.Set(std::string("expected a boolean, got ") +
                LuaTypeName(L, Index));
      return false;
    }
    Out = lua_toboolean(L, Index) != 0;
    return true;
  } else if constexpr (IsStringLike<B>::value) {
    std::size_t Length = 0;
    const char *Text = lua_tolstring(L, Index, &Length);
    if (!Text) {
      Error.Set(std::string("expected a string, got ") + LuaTypeName(L, Index));
      return false;
    }
    if constexpr (std::is_same_v<B, std::string>)
      Out.assign(Text, Length);
    else
      Out = std::string_view(Text, Length);
    return true;
  } else if constexpr (std::is_enum_v<B>) {
    using Underlying = std::underlying_type_t<B>;
    Underlying Raw{};
    if (!ReadNumber<Underlying>(L, Index, Raw, Error))
      return false;
    Out = static_cast<B>(Raw);
    return true;
  } else if constexpr (std::is_arithmetic_v<B>) {
    return ReadNumber<B>(L, Index, Out, Error);
  } else if constexpr (IsVector<B>::value) {
    return ReadSequence<typename B::value_type>(L, Index, Out, ClassId, Error);
  } else if constexpr (IsSharedPtr<B>::value && ClassV<B>) {
    std::shared_ptr<void> Erased;
    if (!StoreSharedPointer(L, Index, ClassId, Erased, Error.Text))
      return false;
    Out = std::static_pointer_cast<typename SharedInner<B>::Type>(Erased);
    return true;
  } else if constexpr (ClassV<A>) {
    using Subject = typename ClassSubject<A>::Type;
    void *Pointer = nullptr;
    if (!ReadInstancePointer(L, Index, ClassId, &Pointer)) {
      // Only the diagnostic path builds a message, so a good call allocates
      // nothing and the failure explains itself.
      return ReadUserdataPointer(L, Index, ClassId, &Pointer, Error.Text);
    }
    if constexpr (std::is_pointer_v<S>)
      Out = static_cast<S>(Pointer);
    else
      Out = *static_cast<Subject *>(Pointer);
    return true;
  } else {
    // Naming the type is the whole diagnostic: an incomplete-type error above
    // the assert prints it, so a consumer sees which parameter is unsupported.
    UnsupportedParameterType<UnrefT<A>> Unsupported;
    (void)Unsupported;
    static_assert(sizeof(A) == 0,
                  "luna: no codec for this parameter type. Specialise "
                  "luna::RegisteredClassTrait for a class, or use a supported "
                  "scalar. Structural types are refused on purpose.");
    return false;
  }
}

template <class A>
bool ReadSlot(lua_State *L, int Index, SlotType<A> &Out, TypeId ClassId,
              ArgError &Error) {
  if constexpr (SlotIsPointer<A>) {
    // The hot case: an instance reached by reference binds straight to the
    // userdata payload, so nothing is copied and no message is built.
    if (ReadInstancePointer(L, Index, ClassId, &Out))
      return true;
    return ReadUserdataPointer(L, Index, ClassId, &Out, Error.Text);
  } else {
    return ReadValue<A>(L, Index, Out, ClassId, Error);
  }
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

template <class B, class Ret>
int PushClassResult(lua_State *L, Ret &&Value) {
  using Subject = typename ClassSubject<B>::Type;
  const void *Token = static_cast<const void *>(&TypeToken<Subject>);

  if constexpr (IsSharedPtr<B>::value) {
    const bool Ok = PushSharedInstance(
        L, Token, std::static_pointer_cast<void>(std::forward<Ret>(Value)));
    return Ok ? 1 : 0;
  } else if constexpr (std::is_pointer_v<B>) {
    const bool Ok = PushBorrowedInstance(L, Token, const_cast<Subject *>(Value),
                                        nullptr);
    return Ok ? 1 : 0;
  } else {
    // The VM takes a copy; the deleter is generated here so the backend never
    // needs to know how to destroy the type.
    auto *Copy = new Subject(std::forward<Ret>(Value));
    const bool Ok = PushOwnedInstance(
        L, Token, Copy,
        [](void *Pointer) { delete static_cast<Subject *>(Pointer); });
    if (!Ok)
      delete Copy;
    return Ok ? 1 : 0;
  }
}

template <class T>
int WriteValue(lua_State *L, const T &Value, TypeId ClassId) {
  using B = UnrefT<T>;
  if constexpr (CustomCodec<B>::Enabled) {
    return CustomCodec<B>::Write(L, Value);
  } else if constexpr (IsOwnedValueV<B>) {
    return PushOwnedValue(L, Value);
  } else if constexpr (IsValuePackV<B>) {
    return PushValuePack(L, Value);
  } else if constexpr (std::is_same_v<B, bool>) {
    lua_pushboolean(L, Value ? 1 : 0);
    return 1;
  } else if constexpr (IsStringLike<B>::value) {
    lua_pushlstring(L, Value.data(), Value.size());
    return 1;
  } else if constexpr (std::is_enum_v<B>) {
    lua_pushnumber(
        L, static_cast<double>(static_cast<std::underlying_type_t<B>>(Value)));
    return 1;
  } else if constexpr (std::is_arithmetic_v<B>) {
    lua_pushnumber(L, static_cast<double>(Value));
    return 1;
  } else if constexpr (ClassV<B>) {
    return PushClassResult<B>(L, Value);
  } else {
    (void)ClassId;
    lua_pushnil(L);
    return 1;
  }
}

template <class Tuple, std::size_t... I>
int WriteTuple(lua_State *L, const Tuple &Values, std::index_sequence<I...>) {
  int Pushed = 0;
  ((Pushed += WriteValue(L, std::get<I>(Values), NoType)), ...);
  return Pushed;
}

template <class R> struct RetArity {
  static constexpr std::size_t Value = 1;
};
template <> struct RetArity<void> {
  static constexpr std::size_t Value = 0;
};
template <class A, class B> struct RetArity<std::pair<A, B>> {
  static constexpr std::size_t Value = 2;
};
template <class... T> struct RetArity<std::tuple<T...>> {
  static constexpr std::size_t Value = sizeof...(T);
};
template <class T> struct RetArity<std::optional<T>> {
  static constexpr std::size_t Value = RetArity<T>::Value;
};

template <class R>
int WriteReturn(lua_State *L, R &&Value, const TypeId *Ids = nullptr) {
  using B = UnrefT<R>;
  (void)Ids;
  if constexpr (std::is_void_v<B>) {
    return 0;
  } else if constexpr (IsOptional<B>::value) {
    if (!Value)
      return 0;
    return WriteValue(L, *Value, NoType);
  } else if constexpr (IsPair<B>::value) {
    int Pushed = WriteValue(L, Value.first, NoType);
    Pushed += WriteValue(L, Value.second, NoType);
    return Pushed;
  } else if constexpr (requires { std::tuple_size<B>::value; }) {
    return WriteTuple(L, Value, std::make_index_sequence<std::tuple_size_v<B>>{});
  } else {
    return WriteValue(L, Value, NoType);
  }
}

// ---------------------------------------------------------------------------
// Invocation core
// ---------------------------------------------------------------------------
//
// One core serves every category. A method call and a plain call have the same
// stack layout -- the receiver, when there is one, is argument 1 and the rest
// follow -- so no per-category trampoline is needed.

template <class F, class Slots, class... A, std::size_t... I>
int CallAndWrite(lua_State *L, F &Fn, Slots &Stored,
                 std::type_identity<std::tuple<A...>>,
                 std::index_sequence<I...>) {
  using R = decltype(Fn(PassSlot<A>(std::get<I>(Stored))...));
  if constexpr (std::is_void_v<R>) {
    Fn(PassSlot<A>(std::get<I>(Stored))...);
    return 0;
  } else {
    return WriteReturn(L, Fn(PassSlot<A>(std::get<I>(Stored))...));
  }
}

// Returns the number of values pushed, or -1 with Error set. Reading stops at
// the first argument that does not convert, so the message names that one.
template <class R, class... A>
int InvokeCall(lua_State *L, std::function<R(A...)> &Fn, const TypeId *ArgIds,
               const TypeId *, const char *Name, std::string &Error) {
  using Args = std::tuple<A...>;
  using Slots = std::tuple<SlotType<A>...>;

  Slots Stored{};
  int Failed = 0;
  std::string Detail;

  auto ReadOne = [&]<std::size_t I>(std::integral_constant<std::size_t, I>) {
    if (Failed != 0)
      return;
    ArgError Local;
    if (!ReadSlot<std::tuple_element_t<I, Args>>(
            L, static_cast<int>(I) + 1, std::get<I>(Stored),
            ArgIds ? ArgIds[I] : NoType, Local)) {
      Failed = static_cast<int>(I) + 1;
      Detail = std::move(Local.Text);
    }
  };

  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (ReadOne(std::integral_constant<std::size_t, I>{}), ...);
  }(std::index_sequence_for<A...>{});

  if (Failed != 0) {
    Error = "argument ";
    Error += std::to_string(Failed);
    if (Name && *Name) {
      Error += " of ";
      Error += Name;
    }
    Error += ": ";
    Error += Detail;
    return -1;
  }

  return CallAndWrite(L, Fn, Stored, std::type_identity<std::tuple<A...>>{},
                      std::index_sequence_for<A...>{});
}

} // namespace detail
} // namespace luna
