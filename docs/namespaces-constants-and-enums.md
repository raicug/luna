# Namespaces, constants and enums

Registration is hierarchical. `BindingRegistry::RegisterNamespace` accepts one validated identifier segment and returns a `NamespaceBuilder` that owns a *pending plan*. Nested builders share that plan and stage under their own scope, so `Commit` submits the whole thing as one outermost transaction: either every declaration of the plan becomes visible, or none does.

```cpp
Luna::BindingRegistry Registry = State.Bindings();
Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

Studio.RegisterConstant("Version", "0.1.0")
    .RegisterConstant("MaxSprites", 512)
    .RegisterFunction("DescribeAccess", &DescribeAccess)
    .Documentation("The demo surface.")
    .Documentation("MaxSprites", "How many sprites the demo would draw.")
    .Attribute("stability", "experimental")
    .Example("HostLog(Studio.Version)");

Luna::NamespaceBuilder Text = Studio.RegisterNamespace("Text");
Text.RegisterConstant("Ellipsis", "...")
    .RegisterFunction("Shorten", Luna::WithDefaults(&Shorten, 8));

const Luna::RegistrationResult Result = Studio.Commit();
```

Destroying an uncommitted builder has no effect at all: no VM value, no reflection record, no dispatch target. That makes a failed setup path safe to abandon.

`QualifiedName()` reports the canonical dot-separated name Luna built, which is also the name reflection and generated artifacts use.

## What a namespace can hold

A `NamespaceBuilder` stages functions (`RegisterFunction`), constants (`RegisterConstant`), nested namespaces (`RegisterNamespace`), enums (`RegisterEnum`), classes (`RegisterClass`), and module loads (`RegisterModule`). Scoped functions accept exactly the callable forms root-scope registration accepts and are described by exactly the same canonical descriptor, so a scoped function and a root one differ only in their qualified name.

## Constants

`RegisterConstant` stages one immutable value. The value is normalized to its canonical type in your translation unit and converted through that type's registered writer at commit time, so the registry never needs your C++ type. The accepted inputs and their canonical types are listed in [values and validation](values-and-validation.md).

A constant whose canonical type is a user-defined leaf takes its `StableTypeKey` as a third argument, which is what keeps an enum constant typed as that enumeration instead of degrading into an untyped integer:

```cpp
Studio.RegisterConstant("DefaultChannel", Channel::Info, ChannelKey());
```

At root scope, `BindingRegistry::RegisterConstant` has the same two forms and commits immediately as its own transaction.

A class table holds constants too, through `ClassBuilder::Constant`, which takes the same two forms and stages into the class's own plan — see [class constants](classes-and-userdata.md#class-constants).

## Enumerations

`RegisterEnum<Enum>` takes a name segment and the enumeration's `StableTypeKey`, and returns an `EnumBuilder<Enum>`:

```cpp
enum class Channel : int { Debug = 10, Info = 20, Warning = 30, Error = 40 };

Luna::EnumBuilder<Channel> Channels =
    Studio.RegisterEnum<Channel>("Channel", ChannelKey());
Channels.Value("Debug", Channel::Debug)
    .Value("Info", Channel::Info)
    .Value("Warning", Channel::Warning)
    .Value("Error", Channel::Error)
    .Alias("Default", "Info")
    .Documentation("Host log channels.")
    .Documentation("Default", "A second name for one canonical enumerator.")
    .Example("HostLog(Studio.Channel.Default)");
```

The enumeration is checked, not guessed:

- Every value is validated against the enumeration's declared C++ underlying type and against the exact-integer domain Luna converts through. An out-of-range value is refused rather than narrowed, wrapped, or rounded. `Value` also has an overload taking `std::int64_t` for a numeric enumerator, validated the same way.
- A duplicate name is refused. A duplicate numeric value is refused unless it is declared explicitly through `Alias`, which is the only way one canonical enumerator gets a second name.
- Scoped behavior is the default. Exposing an unscoped enumeration requires the explicit `AllowUnscoped()` opt-in; without it, an unscoped enumeration is refused deterministically.

### Bitflags

`Bitflags()` declares bitflag behavior with the supported-bit mask computed from the declared enumerators. `Bitflags(SupportedBits)` — taking either an enumerator or a `std::int64_t` mask — declares it with an explicit mask — every declared enumerator must be a subset of it, and a converted value carrying any other bit is rejected whole rather than masked down.

```cpp
enum class Access : unsigned int { Read = 1, Write = 2, Execute = 4 };

Luna::EnumBuilder<Access> Flags =
    Studio.RegisterEnum<Access>("Access", AccessKey());
Flags.Value("Read", Access::Read)
    .Value("Write", Access::Write)
    .Value("Execute", Access::Execute)
    .Bitflags()
    .Documentation("Declared bitflags, so a combined mask converts whole.");
```

A combined mask then arrives in the host as one whole value:

```lua
local Mask = bit32.bor(Studio.Access.Read, Studio.Access.Write)
HostLog(Studio.DescribeAccess(Mask))
```

### Enumerator objects

By default every enumerator reaches a script as its bare number, so `Studio.Channel.Info == 20`. `AsObjects()` publishes each enumerator as one interned enumerator object instead. It is one more call in the enumeration's own chain, staged once alongside its enumerators:

```cpp
Luna::EnumBuilder<Channel> Channels =
    Studio.RegisterEnum<Channel>("Channel", ChannelKey());
Channels.AsObjects()
    .Value("Debug", Channel::Debug)
    .Value("Info", Channel::Info)
    .Alias("Default", "Info");
```

```lua
local Chosen = Studio.Channel.Info
assert(typeof(Chosen) == "EnumItem")
assert(Chosen.Name == "Info")
assert(Chosen.Value == 20)
assert(Chosen.EnumName == "Studio.Channel")
assert(tostring(Chosen) == "Studio.Channel.Info")
assert(Studio.Channel.Default == Studio.Channel.Info)
```

What the opt-in changes:

- Each enumerator is created once per State and retained, so reading the same enumerator twice yields one value. Equality is therefore identity: two enumerators of one enumeration are never equal, and an alias reads the very object its canonical enumerator publishes.
- An enumerator object is immutable and its metatable is protected, exactly like the enumeration table that holds it. Writing a field, adding one, or replacing the metatable each fail deterministically.
- The enumeration's canonical Luau representation becomes userdata rather than number, so a bare number is no longer a value of it. A constant declared of the enumeration publishes the same interned object, whichever of the two installed first.
- The opt-in is per enumeration. An enumeration that does not ask for objects keeps its numeric representation unchanged.

Generated Luau declarations still describe an enumerator by its numeric value, so an object-mode enumeration's generated type is `number` rather than an enumerator type.

At root scope, `BindingRegistry::RegisterEnum` stages the enumeration in a plan of its own, so call `Commit()` on the returned builder.

## Annotations

Three annotation calls appear on every builder — namespaces, enums, classes:

- `Documentation(Text)` documents the declaration itself; `Documentation(Member, Text)` documents one member already staged in it.
- `Attribute(Name, Value)` and `Attribute(Member, Name, Value)` attach an immutable name/value pair.
- `Example(Text)` and `Example(Member, Text)` add a usage example. Examples are reflected in declaration order, so generated material repeats them exactly as declared.

The member must already be declared when the annotation is staged, so a typo fails the commit deterministically instead of silently documenting nothing. There are no annotation helper macros; these ordinary calls are the whole mechanism.

Annotations are what documentation and declaration generation read. See [reflection and generation](reflection-and-generation.md).

---

[← Previous: Values and validation](values-and-validation.md) · [Documentation index](README.md) · [Next: Classes and userdata →](classes-and-userdata.md)
