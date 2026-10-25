# Classes and userdata

`RegisterClass<T>` exposes a C++ class as typed userdata. A class is described to luna; it is never discovered.

## Making a type bindable

One line, at global scope, before the type is used:

```cpp
template <> struct luna::detail::RegisteredClassTrait<Sprite> : std::true_type {};
```

This is what lets a parameter or return of type `Sprite`, `Sprite &`, `Sprite *` or `std::shared_ptr<Sprite>` compile. Without it the signature is refused with a `static_assert`.

## A class

```cpp
luna::ClassBuilder<Sprite> sprites = studio.RegisterClass<Sprite>(
    "Sprite", luna::StableTypeKey("app.Sprite"));

sprites.Factory("new", [](double w, double h) { return Sprite{w, h}; })
    .StaticMethod("Unit", [] { return Sprite{1, 1}; })
    .Method("Area", &Sprite::Area)
    .Property("Width", [](const Sprite &s) { return s.Width; },
              [](Sprite &s, double v) { s.Width = v; })
    .Property("Height", [](const Sprite &s) { return s.Height; })
    .Field("Name", &Sprite::Name)
    .Operator(luna::ClassOperator::ToText, &Sprite::ToText)
    .Constant("Max", 4096);
const luna::RegistrationResult result = sprites.Commit();
```

```lua
local s = Studio.Sprite.new(3, 4)
s.Width = 7
print(s:Area(), s.Height, tostring(s), Studio.Sprite.Max)
```

### ClassBuilder

| Method | Meaning |
| --- | --- |
| `Factory(name, callable)` | A function on the class table that returns an instance (`Sprite.new(...)`) |
| `StaticMethod(name, callable)` | A function on the class table that takes no receiver |
| `Method(name, callable)` | A function called with `:`. The **first parameter is the receiver** and is not counted in the script's arguments. A member-function pointer works. |
| `Property(name, getter)` | Read-only |
| `Property(name, getter, setter)` | Read-write |
| `Property(name, getter, setter, PropertyPolicy::Notify(hook))` | Read-write, with a hook run after each successful write |
| `Field(name, &T::member)` | Read-write access to a data member, by value |
| `Operator(op, callable)` | A metamethod |
| `Constant(name, OwnedValue)` | An immutable value on the class table |
| `BaseOf<B>(baseName)` | Inherit from an already-registered base |
| `BorrowedFrom(handle)` | Tie borrowed instances to a `LifetimeHandle` |
| `Commit()` | Publish |

Everything is staged and validated before anything is linked. Methods may repeat a name (an overload set); a property or field may not share a name with a method or with another property or field, and a static function may not share one with a constant. These are refused at commit with a message that names the member.

### Write hooks

```cpp
sprites.Property("Width", getter, setter,
                 luna::PropertyPolicy::Notify(
                     [](Sprite &self, std::string_view name) { redraw(self); }));
```

The hook runs after the setter succeeded, with the receiver and the property name. A setter that throws does not run it.

## Operators

| `ClassOperator` | Luau metamethod | Callable shape |
| --- | --- | --- |
| `Add`, `Subtract`, `Multiply`, `Divide`, `Modulo`, `Power` | `__add` ... `__pow` | `(const T &, const T &) -> R` |
| `UnaryMinus` | `__unm` | `(const T &) -> R` |
| `Equal`, `Less`, `LessOrEqual` | `__eq`, `__lt`, `__le` | `(const T &, const T &) -> bool` |
| `Concatenate` | `__concat` | `(const T &, ...) -> R` |
| `Length` | `__len` | `(const T &) -> number` |
| `ToText` | `__tostring` | `(const T &) -> std::string` |
| `Call` | `__call` | `(const T &, ...) -> R` |
| `Iterate` | `__iter` | `(const T &, int control) -> value`; `nil` ends the loop |
| `Index` | `__index` fallback | `(const T &, key) -> value`, for keys that are not members |
| `NewIndex` | `__newindex` fallback | `(T &, key, value)` |

`Index` and `NewIndex` only answer keys that are **not** members: a declared property still wins.

### Iteration

`Iterate` makes the class usable in a generic `for`. The step function receives a zero-based control index and returns the value, or `nil` to stop:

```cpp
bags.Operator(luna::ClassOperator::Iterate,
              [](const Bag &self, int control) -> std::optional<double> {
                if (control >= self.Count()) return std::nullopt;
                return self.At(control);
              });
```

```lua
for index, value in bag do print(index, value) end   -- 1, a / 2, b / ...
```

The loop's first variable counts from 1.

## Registration order

A callable whose signature mentions a class must be registered **after** that class, or in the **same commit**:

```cpp
registry.RegisterFunction("early", [](Sprite &s) { return s.Width; });
// refused: 'early' takes a class that is not registered; register the class first,
// or in the same commit
```

The reason is that a parameter check is resolved to the class's id when the callable is linked. luna refuses the registration rather than guess, because an unresolved class would accept any instance. The same rule covers classes inside containers: `std::vector<Sprite>` counts.

At runtime a class-typed position that somehow has no class refuses every instance.

## Ownership

An instance is one of three things, chosen by how it entered Lua:

| Model | Created by | Meaning |
| --- | --- | --- |
| **Lua-owned** | A callable returning `T` by value, `Factory`, `OwnedValue::Instance<T>(value)` | The script owns a copy. It is destroyed with the userdata. |
| **Shared** | A callable returning `std::shared_ptr<T>` | The script holds a reference to a shared object. |
| **Borrowed** | A callable returning `T *` or `T &`, or `RegisterValue` with `Borrowed(handle)` | The script holds a pointer the host owns. |

A borrowed pointer is the risky one: the host may destroy the object while a script still holds it. Tie it to a handle:

```cpp
luna::LifetimeHandle lifetime;
sprites.BorrowedFrom(lifetime);

sprites.Factory("pick", []() -> Sprite * { return &globalSprite; });
// ...
lifetime.Invalidate();   // before destroying globalSprite
```

After `Invalidate()`, a script that kept the instance gets `the instance is no longer available` on any read, write or call. Without `BorrowedFrom`, a borrowed instance is never invalidated, so use a handle whenever the pointer's lifetime is not the state's.

`RegisterValue(..., Borrowed(handle))` attaches the handle to that one published instance instead.

A `T &` return is borrowed too: the script gets the host's object, and a write reaches it. (A reference return used to be copied; it is not any more.)

## Inheritance

```cpp
struct Entity { double Id = 0; double Describe() const; };
struct Player : Entity { double Score = 0; };

luna::ClassBuilder<Entity> entities = studio.RegisterClass<Entity>("Entity", luna::StableTypeKey("app.Entity"));
entities.Method("Describe", &Entity::Describe).Field("Id", &Entity::Id);
entities.Commit();

luna::ClassBuilder<Player> players = studio.RegisterClass<Player>("Player", luna::StableTypeKey("app.Player"));
players.Factory("new", [] { return Player{}; }).Field("Score", &Player::Score);
players.BaseOf<Entity>("Studio.Entity");
players.Commit();
```

- The base must already be registered, and is named by its **registered** name (`"Studio.Entity"`). A base declared later in the same commit is refused, as is a base whose C++ type does not match `B`.
- The derived class gets the base's methods, properties and fields, and its own definitions win.
- A derived instance can be passed wherever the base is expected. luna adjusts the pointer by the base's offset, so a base that is not at offset zero (multiple inheritance) works.
- Every instance answers `obj:IsA("Name")`, which walks the base chain. A bare name or the registered name both match.
- Operators are inherited too, except `__index`, `__newindex` and `__type`.
- Members added to a base *after* a derived class is committed do not reach the derived class.

## Re-registering a class

Registering the same name for the **same C++ type** again extends the class: new members are added, and existing operators are kept. Registering a **different** type under a name that is taken is refused (`already registered for a different type`). A second *name* for the same type becomes an alias of the first.

## What a script can and cannot change

A script cannot rewrite what the host registered:

- `getmetatable(instance)` returns the string `The metatable is locked`;
- the class table (`Studio.Sprite`) and every table behind it are read-only: `Studio.Sprite.new = nil` and `Studio.Sprite.extra = 1` are errors;
- enum tables and enumerators are read-only.

The host can still extend a class in a later commit. luna lifts the lock while it writes and puts it back afterwards.

## Missing members

Reading a member a class does not have gives `nil`. With `StateOptions::StrictMembers` it raises `Wdith is not a valid member of Sprite`, so a misspelt name fails where it is written. Writing to something that is not a writable member is always an error.

---

[← Previous: Namespaces, constants and enums](namespaces-constants-and-enums.md) · [Documentation index](README.md) · [Next: Tasks and scheduling →](tasks-and-scheduling.md)
