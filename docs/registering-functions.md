# Registering functions

`BindingRegistry::Register` and `RegisterFunction` publish a C++ callable under a name. They are the same call; `Register` is the short form.

```cpp
luna::BindingRegistry registry = state.Bindings();

registry.Register("Add", [](double a, double b) { return a + b; });
registry.RegisterFunction("Greet", [](std::string name) { return "hello " + name; });
```

Both return a `RegistrationResult`. A name may be dotted (`"Tools.Text.Trim"`): the namespaces on the way are created if they do not exist, and a script reaches the function as `Tools.Text.Trim(...)`.

## What counts as a callable

| Form | Notes |
| --- | --- |
| Free function or function pointer | `&Add` |
| Lambda | Captures are fine; the lambda is copied into the VM |
| Functor with one non-template `operator()` | A generic lambda is refused at compile time, with a message |
| Member-function pointer | Only inside a class: `.Method("Area", &Sprite::Area)` |

A signature luna cannot read is a `static_assert` with an explanation, not a template error three screens long. An unsupported parameter or return type is the same: the message says what to specialise (`RegisteredClassTrait` for a class, `CustomCodec` for a custom type).

## Parameters

A parameter is read from the script's argument at its position. A wrong type is an error that names the function and the position:

```text
luna:1: argument 1 of Add: expected a number, got string
```

| C++ type | Accepts |
| --- | --- |
| `bool` | a boolean only |
| `int`, `double`, `float`, any integer or enum | a number. An integer type also rejects a fraction and a value out of range. |
| `std::string`, `std::string_view` | a string (a number is converted when the function is **not** overloaded) |
| `std::optional<T>` | `T`, or nothing/`nil` |
| `std::vector<T>`, `std::map<K, V>`, `std::pair`, `std::tuple` | a table, read element by element |
| a registered class: `T`, `const T &`, `T &`, `T *`, `std::shared_ptr<T>` | an instance of that class, or of a class derived from it |
| `luna::Delegate<R(A...)>` | a function |
| `luna::ArgumentPack` | every argument from this position on |
| `luna::OwnedValue`, `luna::Value` | any value |

`std::string_view` borrows the script's string. It is valid for the duration of the call only.

### Optional parameters

A `std::optional<T>` parameter, and an `ArgumentPack`, may be left out. Once a parameter may be omitted, every one after it must also be: this is checked at compile time.

```cpp
registry.Register("Scale", [](double v, std::optional<double> by) {
  return v * by.value_or(2.0);
});
// Scale(3) == 6, Scale(3, 10) == 30
```

### Variadic functions

An `ArgumentPack` reads lazily, so a call pays only for the arguments it inspects.

```cpp
registry.Register("print", [](luna::ArgumentPack args) {
  for (std::size_t i = 0; i < args.Size(); ++i) {
    const luna::Value v = args.View(i);          // a view; valid during the call
    switch (v.Category()) {
    case luna::ValueCategory::String: std::printf("%.*s ", (int)v.AsString().size(), v.AsString().data()); break;
    case luna::ValueCategory::Number: std::printf("%g ", v.AsNumber()); break;
    default: std::printf("? "); break;
    }
  }
});
```

`args.At(i)` returns an owning `OwnedValue` instead; see [Values and conversion](values-and-conversion.md).

## Returns

| Return type | What the script receives |
| --- | --- |
| `void` | nothing |
| a number, `bool`, string | one value |
| an enum | its number |
| `std::vector<T>`, `std::map<K, V>` | a table |
| `std::optional<T>` | the value, or **no** value when empty |
| `std::pair<A, B>` | two return values |
| `std::tuple<A, B, C>` | three return values |
| `OwnedValue` | one value, which may be a table |
| `ValuePack` | as many values as it holds |
| a registered class by value | a new instance the script owns (moved, so move-only types work) |
| `T &` | the host's own object, borrowed: writes reach it |
| `T *` | the host's object, borrowed; `nullptr` is `nil` |
| `std::shared_ptr<T>` | a shared instance; an empty pointer is `nil` |

A pair or tuple returned *inside* a container becomes a table; only a top-level one becomes several values.

> **Warning.** A `T &` or `T *` return gives the script a pointer that luna cannot keep alive. Tie it to a `LifetimeHandle` with `ClassBuilder::BorrowedFrom`, or the script may hold a dangling reference. See [Classes and userdata](classes-and-userdata.md#ownership).

## Errors inside a callable

An exception that escapes a callable is turned into a Luau error that a script can `pcall`:

```cpp
registry.Register("Parse", [](std::string text) { return std::stoi(text); });
// pcall(Parse, "x") -> false, "Parse: invalid stoi argument"
```

Letting an exception unwind through the VM's C frames would be undefined behaviour, so luna catches it at the boundary and rethrows it as a Luau error carrying the function's name.

## Overloads

Register the same name more than once and the registrations form an **overload set**. A call tries the candidates in registration order and takes the first one that accepts the arguments.

```cpp
registry.Register("Describe", [](std::string s) { return "text " + s; });
registry.Register("Describe", [](double n) { return "number"; });
// Describe("a") -> the string overload; Describe(5) -> the number overload
```

The test a candidate must pass is **strict and does not touch the arguments**:

- a number is not a string, and a string is not a number;
- an integer parameter rejects a fraction or an out-of-range value, so `f(int)` then `f(double)` sends `f(2.5)` to the second;
- arity must fit: too few arguments, or too many for a non-variadic candidate, rejects it.

Because the test never converts anything, a candidate that is turned down cannot change the arguments the next one sees. If nothing accepts, the call fails with `no overload of 'Describe' accepts 2 arguments`.

Where overloads can be registered:

| Where | Same name in one commit | Same name across commits |
| --- | --- | --- |
| Namespace or root function | refused as a duplicate | overload set |
| A class's methods, static methods, factories | overload set | overload set |

Reflection reports an overloaded name as `SymbolKind::OverloadSet`.

## Waiting inside a callable

A callable can ask the scheduler to suspend the running script:

```cpp
registry.Register("wait", [](double seconds) { luna::Suspend(seconds); });
```

`luna::Suspend` yields after the function returns, so no thread is parked. See [Tasks and scheduling](tasks-and-scheduling.md).

---

[← Previous: State and lifetime](state-and-lifetime.md) · [Documentation index](README.md) · [Next: Values and conversion →](values-and-conversion.md)
