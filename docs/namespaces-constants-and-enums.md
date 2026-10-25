# Namespaces, constants and enums

A namespace is a Luau table that groups registrations. Namespaces nest, and every builder in a chain stages into **one pending batch** that publishes when any builder in the chain commits.

## Namespaces

```cpp
luna::NamespaceBuilder studio = registry.RegisterNamespace("Studio");
luna::NamespaceBuilder text = studio.RegisterNamespace("Text");   // Studio.Text

text.RegisterFunction("Trim", [](std::string_view s) { return std::string(s); });
text.RegisterConstant("Limit", 80);
text.Commit();
```

`RegisterNamespace` accepts a dotted path (`"Studio.Text"`). Every missing table on the way is created. A segment that already holds something other than a table is a refusal, found before anything is linked:

```text
cannot register 'blocked.fn': 'blocked' is already bound to a value that is not a table
```

### NamespaceBuilder

| Method | Stages |
| --- | --- |
| `RegisterFunction(name, callable)` | A function |
| `RegisterConstant(name, OwnedValue)` | An immutable value |
| `RegisterNamespace(name)` | A child namespace sharing this batch |
| `RegisterClass<T>(name, key)` | A class; returns a `ClassBuilder<T>` |
| `RegisterEnum<E>(name, key)` | An enum; returns an `EnumBuilder<E>` |
| `RegisterValue(name, factory, policy)` | One published instance |
| `Commit()` | Validates and publishes the whole batch |

`BindingRegistry` offers the same registrations except constants: it stages and commits a function or a value in one call, and returns a builder for a namespace, a class or an enum. Put a constant in a namespace.

### One batch, one commit

```cpp
studio.RegisterFunction("Open", [] {});
luna::ClassBuilder<Sprite> sprites = studio.RegisterClass<Sprite>("Sprite", key);
sprites.Factory("new", [] { return Sprite{}; });
sprites.Commit();        // publishes Open *and* Sprite
```

A builder's `Commit()` takes everything staged in its chain, including what other builders in the chain staged. That is what lets a function and the class it takes register together, and it is why a function over a class that is not yet registered is refused unless the class is in the same batch (see [Classes and userdata](classes-and-userdata.md#registration-order)).

## Constants

```cpp
studio.RegisterConstant("Version", "0.1.0");
studio.RegisterConstant("Pi", 3.14159);
studio.RegisterConstant("Origin", luna::OwnedValue::Table().SetField("x", 0).SetField("y", 0));
```

A constant is any `OwnedValue`: a number, boolean, string, or a table built at registration. It is published once, at commit.

## Published values

`RegisterValue` names one *instance* of a registered class, so `Studio.Camera` reaches it without a call.

```cpp
static Camera camera;
luna::LifetimeHandle lifetime;

studio.RegisterValue("Camera", [] { return &camera; },
                     luna::OwnershipPolicy::Borrowed(lifetime));
```

The factory runs once, at commit, and its result is published at the name.

| `OwnershipPolicy` | Meaning |
| --- | --- |
| `LuaOwned()` (default) | The factory returns the object by value; the script owns it |
| `Borrowed(handle)` | The factory returns `T *`; the instance stops working when `handle.Invalidate()` is called |
| `Shared()` | The factory returns `std::shared_ptr<T>` |

A factory that returns nothing (a null pointer, an empty `OwnedValue`) is refused with `produced no value`, and the whole commit is rolled back.

## Enums

```cpp
enum class Channel : int { Debug = 10, Info = 20, Warning = 30 };

luna::EnumBuilder<Channel> channels =
    studio.RegisterEnum<Channel>("Channel", luna::StableTypeKey("app.Channel"));
channels.Value("Debug", Channel::Debug)
    .Value("Info", Channel::Info)
    .Value("Warning", Channel::Warning)
    .Alias("Default", "Info");
channels.Commit();
```

```lua
print(Studio.Channel.Info, Studio.Channel.Default)   --> 20 20
```

An enumerator is a plain number by default. An alias names an existing enumerator (or another alias) and is validated: an alias to something that is not there is refused with `aliases 'X', which is not one of its values`.

### Enumerator objects

`AsObjects()` publishes each enumerator as one interned table instead of a number:

```cpp
channels.AsObjects();
```

```lua
local info = Studio.Channel.Info
print(info.Name, info.Value, info.EnumName)   --> Info   20   Studio.Channel
print(info == Studio.Channel.Info)             --> true
print(info == Studio.Channel.Warning)          --> false
```

Because each enumerator is created once, identity comparison works.

### Enum parameters and returns

A C++ enum used as a parameter is read from a number, and as a return it is written as a number. luna does not check that the number is one of the registered enumerators.

### Locked

Enum tables and enumerator objects are read-only to scripts: `Studio.Channel.Info = 5` and `Studio.Channel.Info.Value = 9` are both errors.

## The `StableTypeKey`

Classes and enums take a `luna::StableTypeKey`, a string that names the type independently of the C++ type's name or address:

```cpp
luna::StableTypeKey("app.Sprite")
```

It is stored with the class for tooling that wants an identity that survives a rename or a rebuild. If you do not need one, make it the same as the class name; an empty key defaults to the name.

---

[← Previous: Values and conversion](values-and-conversion.md) · [Documentation index](README.md) · [Next: Classes and userdata →](classes-and-userdata.md)
