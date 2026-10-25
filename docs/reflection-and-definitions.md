# Reflection and definitions

Everything registered is described by a list of symbols. You can walk it for an autocomplete palette, take an immutable copy that outlives the state, or render it as a `.d.luau` file for editors.

## Symbols

```cpp
for (const luna::Symbol &entry : state.Symbols()) {
  std::printf("%-40s %d\n", entry.QualifiedName().c_str(), (int)entry.Kind());
}
```

| Member | Meaning |
| --- | --- |
| `Name()` | The last segment: `Area` |
| `QualifiedName()` | The full path: `Studio.Sprite.Area` |
| `Kind()` | A `SymbolKind` |
| `Arity()` | How many arguments the callable takes, or `-1` when unknown. A method's receiver is not counted. |
| `Required()` | How many of them cannot be omitted, or `-1` |

### SymbolKind

| Kind | A |
| --- | --- |
| `Function` | Namespace or root function |
| `Method`, `StaticMethod`, `Factory` | Class members |
| `Property`, `Field` | Data members |
| `Class` | A registered class |
| `Enumeration`, `Enumerator`, `EnumeratorAlias` | An enum, its values, and its aliases |
| `Constant` | A constant, a class constant, or a published value |
| `Namespace` | A namespace table |
| `OverloadSet` | A name registered more than once |

A derived class lists its inherited members alongside its own, so a palette for `Player` shows what it got from `Entity`.

## Snapshots

`state.Symbols()` is a view of the list as of now. `state.Reflection()` returns a `ReflectionSnapshot`, an immutable copy that owns its strings:

```cpp
luna::ReflectionSnapshot snapshot = state.Reflection();
// later, even after more registration, and after the state is gone:
for (const luna::Symbol &entry : snapshot.Symbols()) { /* ... */ }
```

A snapshot is thread-safe to read. Taking one while another thread commits is also safe: the symbol list is guarded.

## Definitions

`state.Definitions()` renders the symbols as `.d.luau` text, and `luna::GenerateDefinitions(snapshot)` does the same from a snapshot:

```lua
declare Plain: (any) -> any
declare WithOptionals: (any, any?) -> any
declare Overloaded: (...any) -> any

declare Point: {
    Sum: () -> any,
    Tag: any,
    new: () -> any,
}

declare Tools: {
    Text: {
        Trim: (any) -> any,
    },
}
```

| Rule | Result |
| --- | --- |
| A callable | `(any, any?) -> any`: its arity, with `?` on an optional tail |
| A method | Its receiver is not counted |
| An overload set | `(...any) -> any` |
| A property, field, constant | `any` |
| A namespace | A nested table type |
| A name with members | Declared as a shape, never also as a value |
| A name that is a Luau keyword | Written `["end"]: ...` as a member; a global of that name is left out |

The output is sorted and deterministic, so generating twice gives the same text, and it ends without a trailing newline.

### Limits of the output

- Types are `any`. luna does not record a parameter type name, so the file documents shape and arity, not types.
- A class's instance members and its static ones (`new`, constants) sit in one shape.
- Natively installed members such as `IsA` are not emitted.

## Using it

Write the text to a file your editor reads, for example as a `.d.luau` definitions file for `luau-lsp`:

```cpp
std::ofstream("definitions.d.luau") << state.Definitions();
```

---

[← Previous: Errors and results](errors-and-results.md) · [Documentation index](README.md) · [Next: Performance →](performance.md)
