#pragma once

// Marshalling between C++ types and the VM stack.

#include <luna/fwd.hpp>
#include <luna/value.hpp>

#include <lua.h>
#include <lualib.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luna {
namespace detail {
inline constexpr std::uint32_t kObjectMagic = 0x4C554E41u;

enum class ObjectTag : std::uint8_t {
  Borrowed = 0,
  Shared = 1,
  Owned = 2,
};

struct Object {
  std::uint32_t Magic;
  TypeId ClassId;
  std::uint8_t Tag;
  std::uint8_t Padding[3];
  void *Pointer;
  void (*Destroy)(void *);
  void *Control;
  std::shared_ptr<void> *Shared;
};

inline thread_local bool SuspensionRequested = false;
inline thread_local double SuspensionDelay = 0.0;

// Outlives the VM, so a handle can tell whether its state is still there.
struct Liveness {
  std::atomic<bool> Alive{true};
};

std::shared_ptr<Liveness> LivenessOf(void *VM) noexcept;

// A non-null key parks the chunk until State::Wake(Key), which is what
// Signal::Wait uses.
inline thread_local const void *SuspensionKey = nullptr;

// A request that was made and will not be honoured must not outlive the call.
inline void ClearSuspensionRequest() noexcept {
  SuspensionRequested = false;
  SuspensionDelay = 0.0;
  SuspensionKey = nullptr;
}

bool CompleteSuspension(lua_State *L);

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

template <class T> struct IsMap : std::false_type {};
template <class K, class V, class C, class A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {};
template <class K, class V, class H, class E, class A>
struct IsMap<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template <class T> struct IsPair : std::false_type {};
template <class T, class U> struct IsPair<std::pair<T, U>> : std::true_type {};

template <class T, class = void> struct IsTupleLike : std::false_type {};
template <class T>
struct IsTupleLike<T, std::void_t<decltype(std::tuple_size<T>::value)>>
    : std::true_type {};

template <class T> struct IsStringLike : std::false_type {};
template <> struct IsStringLike<std::string> : std::true_type {};
template <> struct IsStringLike<std::string_view> : std::true_type {};

template <class T, bool = IsSharedPtr<T>::value> struct SharedOr {
  using Type = T;
};
template <class T> struct SharedOr<T, true> {
  using Type = typename SharedInner<T>::Type;
};

template <class A> struct ClassSubject {
  using Bare = UnrefT<A>;
  using Ptr = std::remove_cv_t<std::remove_pointer_t<Bare>>;
  using Type = std::conditional_t<std::is_pointer_v<Bare>, Ptr,
                                  typename SharedOr<Bare>::Type>;
  static constexpr bool Is = RegisteredClassTrait<Type>::value;
};

template <class A> inline constexpr bool ClassV = ClassSubject<A>::Is;

TypeId ClassIdForToken(lua_State *L, const void *Token) noexcept;

// Containers carry the class of their element, not of the parameter.
template <class E> TypeId ElementClassId(lua_State *L) noexcept {
  using Subject = typename ClassSubject<E>::Type;
  if constexpr (RegisteredClassTrait<Subject>::value)
    return ClassIdForToken(L, static_cast<const void *>(&TypeToken<Subject>));
  else
    return NoType;
}

template <class T, class = void> struct CustomCodec {
  static constexpr bool Enabled = false;
};

// Raised while marshalling a return value, and turned into a Lua error by the
// caller.
class MarshalError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

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

inline int PinTop(lua_State *L) {
  const int Ref = lua_ref(L, -1);
  lua_pop(L, 1);
  return Ref;
}

inline void Unpin(lua_State *L, int Ref) noexcept {
  lua_unref(L, Ref);
}

inline void PushClosure(lua_State *L, lua_CFunction Fn, const char *Name,
                        int Upvalues) {
  lua_pushcclosurek(L, Fn, Name, Upvalues, nullptr);
}

bool ReadUserdataPointer(lua_State *L, int Index, TypeId ClassId, void **Out,
                         std::string &Why);

bool ReadBasePointer(lua_State *L, int Index, TypeId ClassId,
                     void **Out) noexcept;
// False for a borrowed object whose owner has been invalidated.
bool IsObjectUsable(const Object *Instance) noexcept;
bool StoreSharedPointer(lua_State *L, int Index, TypeId ClassId,
                        std::shared_ptr<void> &Out, std::string &Why);

inline bool ReadInstancePointer(lua_State *L, int Index, TypeId ClassId,
                                void **Out) noexcept {
  if (lua_type(L, Index) != LUA_TUSERDATA)
    return false;

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic)
    return false;
  // A class-typed position whose class never registered must not accept every
  // instance: the pointer is cast to the parameter's type.
  if (ClassId == NoType)
    return false;
  if (Instance->ClassId != ClassId)
    return ReadBasePointer(L, Index, ClassId, Out);

  if (!IsObjectUsable(Instance))
    return false;

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

template <class A>
inline constexpr bool SlotIsPointer =
    std::is_lvalue_reference_v<A> && ClassV<A>;

template <class A>
using SlotType =
    std::conditional_t<SlotIsPointer<A>, void *,
                       std::remove_cv_t<std::remove_reference_t<A>>>;

template <class A, class S> decltype(auto) PassSlot(S &Stored) {
  if constexpr (SlotIsPointer<A>)
    return *static_cast<std::remove_reference_t<A> *>(Stored);
  else if constexpr (std::is_reference_v<A>)
    return static_cast<A>(Stored);
  else
    return std::move(Stored);
}

template <class A>
bool ReadValue(lua_State *L, int Index, SlotType<A> &Out, TypeId ClassId,
               ArgError &Error);

// Whether Value can become an A without loss. A 64-bit maximum is not a double,
// so its conversion rounds up to a power of two that is itself out of range,
// and the bound has to be exclusive there.
template <class A> bool NumberFits(double Value) {
  if constexpr (std::is_integral_v<A>) {
    if (std::floor(Value) != Value)
      return false;
    if (Value < static_cast<double>(std::numeric_limits<A>::min()))
      return false;
    if constexpr (sizeof(A) >= 8)
      return Value < static_cast<double>(std::numeric_limits<A>::max());
    else
      return Value <= static_cast<double>(std::numeric_limits<A>::max());
  } else {
    (void)Value;
    return true;
  }
}

template <class A>
bool ReadNumber(lua_State *L, int Index, A &Out, ArgError &Error) {
  int IsNumber = 0;
  const double Value = lua_tonumberx(L, Index, &IsNumber);
  if (!IsNumber) {
    Error.Set(std::string("expected a number, got ") + LuaTypeName(L, Index));
    return false;
  }
  if constexpr (std::is_integral_v<A>) {
    if (!NumberFits<A>(Value)) {
      Error.Set(
          "expected an integer, got a non-integral or out-of-range value");
      return false;
    }
  }
  Out = static_cast<A>(Value);
  return true;
}

template <class X>
bool ReadSequence(lua_State *L, int Index, std::vector<X> &Out, TypeId ClassId,
                  ArgError &Error) {
  (void)ClassId;
  if (lua_type(L, Index) != LUA_TTABLE) {
    Error.Set(std::string("expected a table, got ") + LuaTypeName(L, Index));
    return false;
  }
  const int Length = static_cast<int>(lua_objlen(L, Index));
  const TypeId ElementId = ElementClassId<X>(L);
  Out.clear();
  Out.reserve(static_cast<std::size_t>(Length));
  for (int Element = 1; Element <= Length; ++Element) {
    lua_rawgeti(L, Index, Element);
    SlotType<X> Item{};
    const int Top = lua_gettop(L);
    const bool Ok = ReadValue<X>(L, Top, Item, ElementId, Error);
    lua_pop(L, 1);
    if (!Ok)
      return false;
    Out.push_back(static_cast<X>(std::move(Item)));
  }
  return true;
}

template <class M>
bool ReadMap(lua_State *L, int Index, M &Out, TypeId ClassId, ArgError &Error) {
  (void)ClassId;
  if (lua_type(L, Index) != LUA_TTABLE) {
    Error.Set(std::string("expected a table, got ") + LuaTypeName(L, Index));
    return false;
  }

  using K = typename M::key_type;
  using V = typename M::mapped_type;
  const TypeId KeyId = ElementClassId<K>(L);
  const TypeId ValueId = ElementClassId<V>(L);
  Out.clear();
  lua_pushnil(L);
  while (lua_next(L, Index) != 0) {
    const int ValueIndex = lua_gettop(L);
    // The key is read from a copy: reading a number as a string converts it in
    // place, and lua_next has to be given back the key it handed out.
    lua_pushvalue(L, ValueIndex - 1);
    const int KeyIndex = lua_gettop(L);
    SlotType<K> Key{};
    SlotType<V> Value{};
    const bool Ok = ReadValue<K>(L, KeyIndex, Key, KeyId, Error) &&
                    ReadValue<V>(L, ValueIndex, Value, ValueId, Error);

    lua_pop(L, 2);
    if (!Ok) {
      lua_pop(L, 1);
      return false;
    }

    Out[static_cast<K>(std::move(Key))] = static_cast<V>(std::move(Value));
  }
  return true;
}

template <class T>
bool ReadTupleLike(lua_State *L, int Index, T &Out, TypeId ClassId,
                   ArgError &Error) {
  (void)ClassId;
  constexpr std::size_t Arity = std::tuple_size_v<T>;
  if (lua_type(L, Index) != LUA_TTABLE) {
    Error.Set(std::string("expected a table, got ") + LuaTypeName(L, Index));
    return false;
  }
  if (static_cast<std::size_t>(lua_objlen(L, Index)) < Arity) {
    Error.Set("expected a table with " + std::to_string(Arity) + " elements");
    return false;
  }

  bool Ok = true;
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    auto ReadOne = [&]<std::size_t N>(std::integral_constant<std::size_t, N>) {
      if (!Ok)
        return;
      lua_rawgeti(L, Index, static_cast<int>(N) + 1);
      using E = std::remove_cv_t<
          std::remove_reference_t<decltype(std::get<N>(std::declval<T &>()))>>;
      SlotType<E> Item{};
      const int Top = lua_gettop(L);
      Ok = ReadValue<E>(L, Top, Item, ElementClassId<E>(L), Error);
      lua_pop(L, 1);
      if (Ok)
        std::get<N>(Out) = static_cast<E>(std::move(Item));
    };
    (ReadOne(std::integral_constant<std::size_t, I>{}), ...);
  }(std::make_index_sequence<Arity>{});
  return Ok;
}

template <class T> struct UnsupportedParameterType;
template <class T> struct UnsupportedReturnType;
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
  } else if constexpr (IsMap<B>::value) {
    return ReadMap(L, Index, Out, ClassId, Error);
  } else if constexpr (IsTupleLike<B>::value) {
    return ReadTupleLike(L, Index, Out, ClassId, Error);
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
      return ReadUserdataPointer(L, Index, ClassId, &Pointer, Error.Text);
    }
    if constexpr (std::is_pointer_v<S>)
      Out = static_cast<S>(Pointer);
    else
      Out = *static_cast<Subject *>(Pointer);
    return true;
  } else {
    UnsupportedParameterType<UnrefT<A>> Unsupported;
    (void)Unsupported;
    static_assert(
        sizeof(A) == 0,
        "luna: no codec for this parameter type. Specialise "
        "luna::detail::RegisteredClassTrait for a class, or use a supported "
        "scalar. Structural types are refused on purpose.");
    return false;
  }
}

template <class A>
bool ReadSlot(lua_State *L, int Index, SlotType<A> &Out, TypeId ClassId,
              ArgError &Error) {
  if constexpr (SlotIsPointer<A>) {
    if (ReadInstancePointer(L, Index, ClassId, &Out))
      return true;
    return ReadUserdataPointer(L, Index, ClassId, &Out, Error.Text);
  } else {
    return ReadValue<A>(L, Index, Out, ClassId, Error);
  }
}

template <class A> bool AcceptValue(lua_State *L, int Index, TypeId ClassId);

template <class X>
bool AcceptElement(lua_State *L, int TableIndex, int Position) {
  lua_rawgeti(L, TableIndex, Position);
  const bool Ok = AcceptValue<X>(L, lua_gettop(L), ElementClassId<X>(L));
  lua_pop(L, 1);
  return Ok;
}

// The test an overload set uses to pick a candidate. It is strict and it never
// touches the stack: a number is not a string and a string is not a number, so
// a candidate that is turned down cannot change the arguments the next one
// sees.
template <class A> bool AcceptValue(lua_State *L, int Index, TypeId ClassId) {
  using B = UnrefT<A>;
  if constexpr (CustomCodec<B>::Enabled) {
    if constexpr (requires { CustomCodec<B>::Accepts(L, Index); }) {
      return CustomCodec<B>::Accepts(L, Index);
    } else {
      B Probe{};
      ArgError Ignored;
      return CustomCodec<B>::Read(L, Index, Probe, Ignored);
    }
  } else if constexpr (IsArgumentPackV<B> || IsOwnedValueV<B> || IsValueV<B>) {
    return true;
  } else if constexpr (IsOptional<B>::value) {
    if (lua_isnoneornil(L, Index))
      return true;
    return AcceptValue<typename B::value_type>(L, Index, ClassId);
  } else if constexpr (std::is_same_v<B, bool>) {
    return lua_type(L, Index) == LUA_TBOOLEAN;
  } else if constexpr (IsStringLike<B>::value) {
    return lua_type(L, Index) == LUA_TSTRING;
  } else if constexpr (std::is_enum_v<B>) {
    return lua_type(L, Index) == LUA_TNUMBER &&
           NumberFits<std::underlying_type_t<B>>(lua_tonumber(L, Index));
  } else if constexpr (std::is_arithmetic_v<B>) {
    return lua_type(L, Index) == LUA_TNUMBER &&
           NumberFits<B>(lua_tonumber(L, Index));
  } else if constexpr (IsVector<B>::value) {
    if (lua_type(L, Index) != LUA_TTABLE)
      return false;
    const int Length = static_cast<int>(lua_objlen(L, Index));
    for (int Element = 1; Element <= Length; ++Element) {
      if (!AcceptElement<typename B::value_type>(L, Index, Element))
        return false;
    }
    return true;
  } else if constexpr (IsMap<B>::value) {
    if (lua_type(L, Index) != LUA_TTABLE)
      return false;
    const TypeId KeyId = ElementClassId<typename B::key_type>(L);
    const TypeId ValueId = ElementClassId<typename B::mapped_type>(L);
    lua_pushnil(L);
    while (lua_next(L, Index) != 0) {
      const int ValueIndex = lua_gettop(L);
      const bool Ok =
          AcceptValue<typename B::key_type>(L, ValueIndex - 1, KeyId) &&
          AcceptValue<typename B::mapped_type>(L, ValueIndex, ValueId);
      if (!Ok) {
        lua_pop(L, 2);
        return false;
      }
      lua_pop(L, 1);
    }
    return true;
  } else if constexpr (IsTupleLike<B>::value) {
    constexpr std::size_t Arity = std::tuple_size_v<B>;
    if (lua_type(L, Index) != LUA_TTABLE ||
        static_cast<std::size_t>(lua_objlen(L, Index)) < Arity)
      return false;
    bool Ok = true;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((Ok = Ok && AcceptElement<std::tuple_element_t<I, B>>(
                       L, Index, static_cast<int>(I) + 1)),
       ...);
    }(std::make_index_sequence<Arity>{});
    return Ok;
  } else if constexpr (IsSharedPtr<B>::value && ClassV<B>) {
    std::shared_ptr<void> Probe;
    std::string Ignored;
    return StoreSharedPointer(L, Index, ClassId, Probe, Ignored);
  } else if constexpr (ClassV<A>) {
    void *Pointer = nullptr;
    if (ReadInstancePointer(L, Index, ClassId, &Pointer))
      return true;
    std::string Ignored;
    return ReadUserdataPointer(L, Index, ClassId, &Pointer, Ignored);
  } else {
    return false;
  }
}

template <class A> bool AcceptSlot(lua_State *L, int Index, TypeId ClassId) {
  if constexpr (SlotIsPointer<A>) {
    void *Pointer = nullptr;
    return ReadInstancePointer(L, Index, ClassId, &Pointer);
  } else {
    return AcceptValue<A>(L, Index, ClassId);
  }
}

template <class T> void WriteOne(lua_State *L, const T &Value);

template <class B, class Ret> int PushClassResult(lua_State *L, Ret &&Value) {
  using Subject = typename ClassSubject<B>::Type;
  const void *Token = static_cast<const void *>(&TypeToken<Subject>);
  bool Ok = false;
  if constexpr (IsSharedPtr<B>::value) {
    if (!Value) {
      lua_pushnil(L);
      return 1;
    }
    Ok = PushSharedInstance(
        L, Token, std::static_pointer_cast<void>(std::forward<Ret>(Value)));
  } else if constexpr (std::is_pointer_v<B>) {
    // A null pointer means "no value", which is how an index lookup says
    // absent.
    if (!Value) {
      lua_pushnil(L);
      return 1;
    }
    Ok = PushBorrowedInstance(L, Token, const_cast<Subject *>(Value), nullptr);
  } else {
    // Owned until the userdata holds it, because making the userdata can throw.
    std::unique_ptr<Subject> Copy(new Subject(std::forward<Ret>(Value)));
    Ok = PushOwnedInstance(L, Token, Copy.get(), [](void *Pointer) {
      delete static_cast<Subject *>(Pointer);
    });
    if (Ok)
      Copy.release();
  }

  if (!Ok)
    throw MarshalError("the returned class is not registered");
  return 1;
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
  } else if constexpr (IsOptional<B>::value) {
    // An absent element inside a container is a nil slot.
    if (!Value) {
      lua_pushnil(L);
      return 1;
    }
    return WriteValue(L, *Value, NoType);
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
  } else if constexpr (IsVector<B>::value) {
    using E = typename B::value_type;
    static_assert(!IsValuePackV<E>,
                  "luna: a container element must produce exactly one value");
    lua_createtable(L, static_cast<int>(Value.size()), 0);
    int Element = 0;
    for (const auto &Item : Value) {
      WriteOne(L, Item);
      lua_rawseti(L, -2, ++Element);
    }
    return 1;
  } else if constexpr (IsMap<B>::value) {
    using V = typename B::mapped_type;
    static_assert(!IsValuePackV<V>,
                  "luna: a container element must produce exactly one value");
    lua_createtable(L, 0, static_cast<int>(Value.size()));
    for (const auto &Entry : Value) {
      WriteOne(L, Entry.first);
      WriteOne(L, Entry.second);
      lua_settable(L, -3);
    }
    return 1;
  } else if constexpr (IsTupleLike<B>::value) {
    constexpr std::size_t Arity = std::tuple_size_v<B>;
    lua_createtable(L, static_cast<int>(Arity), 0);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((WriteOne(L, std::get<I>(Value)),
        lua_rawseti(L, -2, static_cast<int>(I) + 1)),
       ...);
    }(std::make_index_sequence<Arity>{});
    return 1;
  } else if constexpr (ClassV<B>) {
    return PushClassResult<B>(L, Value);
  } else {
    (void)ClassId;
    UnsupportedReturnType<B> Unsupported;
    (void)Unsupported;
    static_assert(sizeof(B) == 0,
                  "luna: no codec for this return type. Return a scalar, a "
                  "string, a registered class, std::optional, std::vector, "
                  "std::map, std::pair or std::tuple, or specialise "
                  "luna::detail::CustomCodec.");
    return 0;
  }
}

template <class T> void WriteOne(lua_State *L, const T &Value) {
  if (WriteValue(L, Value, NoType) == 0)
    lua_pushnil(L);
}

template <class Tuple, std::size_t... I>
int WriteTuple(lua_State *L, const Tuple &Values, std::index_sequence<I...>) {
  int Pushed = 0;
  ((Pushed += WriteValue(L, std::get<I>(Values), NoType)), ...);
  return Pushed;
}

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
  } else if constexpr (IsTupleLike<B>::value) {
    return WriteTuple(L, Value,
                      std::make_index_sequence<std::tuple_size_v<B>>{});
  } else if constexpr (std::is_lvalue_reference_v<R> && ClassV<B> &&
                       !IsSharedPtr<B>::value && !std::is_pointer_v<B>) {
    // A reference names an object the host already owns, so the script gets
    // that object, not a copy it could write to without effect.
    using Subject = typename ClassSubject<B>::Type;
    if (!PushBorrowedInstance(L, static_cast<const void *>(&TypeToken<Subject>),
                              const_cast<Subject *>(std::addressof(Value)),
                              nullptr))
      throw MarshalError("the returned class is not registered");
    return 1;
  } else if constexpr (ClassV<B> && !std::is_reference_v<R> &&
                       !IsSharedPtr<B>::value && !std::is_pointer_v<B>) {
    // A prvalue is moved into the script's copy, so a move-only class works.
    return PushClassResult<B>(L, std::move(Value));
  } else {
    return WriteValue(L, Value, NoType);
  }
}

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
