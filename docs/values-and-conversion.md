# Values and conversion

Three value carriers cross the boundary, each with one job, plus a set of types luna converts for you.

| Carrier | Job | Lifetime |
| --- | --- | --- |
| `luna::Value` | A read-only view of a value already on the stack | Valid while the call runs |
| `luna::OwnedValue` | A value the host builds, to be written into Lua later | Owns everything it holds |
| `luna::ValuePack` | An ordered list of `OwnedValue`, published as N return values | Owns |

No Luau type appears in any of them.

## Value

`ArgumentPack::View(i)` returns a `Value`:

```cpp
const luna::Value v = args.View(0);
switch (v.Category()) {            // None, Nil, Boolean, Number, String, Table,
                                   // Userdata, Function, Thread
case luna::ValueCategory::Number: use(v.AsNumber()); break;
case luna::ValueCategory::String: use(v.AsString()); break;
default: break;
}
```

`AsNumber`, `AsBoolean` and `AsString` take a fallback and return it when the category does not match. The string is a view into the VM's memory.

## OwnedValue

`ArgumentPack::At(i)` returns an `OwnedValue`, and a callable returns one to hand the script something whose shape is decided at runtime.

```cpp
luna::OwnedValue list = luna::OwnedValue::Table();
list.Append(1).Append("two").Append(luna::OwnedValue::Instance<Sprite>(Sprite{}));
list.SetField("count", 3);
return list;
```

| Construct | Makes |
| --- | --- |
| `OwnedValue()` / `Nil()` | `nil` |
| `OwnedValue(true)`, `Boolean(b)` | a boolean |
| `OwnedValue(1.5)`, `Number(n)`, or any integer | a number |
| `OwnedValue("x")`, `Text(sv)` | a string |
| `OwnedValue::Table()` | an empty table; fill it with `Append` and `SetField` |
| `OwnedValue::Instance<T>(value)` or `Instance<T>(shared_ptr<T>)` | an instance of a registered class `T` |

Tables are copy-on-write: copying an `OwnedValue` is cheap, and appending to a copy does not change the original.

### What reading a value back keeps

`ArgumentPack::At` converts a script value into an `OwnedValue`:

| Script value | Becomes |
| --- | --- |
| `nil`, boolean, number, string | the same |
| table | a table: the **sequence** `1..#t` keeps its positions, holes included, as `Items`; every other key becomes a named `Fields` entry. A number outside the sequence keeps its value as text (`[7]` becomes the field `"7"`). |
| a shared instance (`std::shared_ptr<T>` or `OwnedValue::Instance`) | the same instance |
| an instance the VM owns, or only borrows | `nil`, because it cannot outlive the VM |
| function, thread, vector, buffer | `nil` |

Nesting stops at depth 32, which also bounds a cycle; a table past that depth reads as empty.

Passing an instance of a class that was never registered *into* Lua is an error (`an instance of a class that is not registered cannot be passed to Lua`), not a silent `nil`.

## ValuePack

```cpp
luna::ValuePack pack;
pack.Append(1).Append("two");     // a script sees two return values
return pack;
```

`ValuePack` is also what `Chunk::Call` takes as arguments.

## Conversion table

| C++ type | Read from script | Written to script |
| --- | --- | --- |
| `bool` | boolean | boolean |
| integers, `float`, `double` | number | number |
| `enum` | number | number |
| `std::string`, `std::string_view` | string | string |
| `std::optional<T>` | `T` or absent | `T`, or `nil` as a container element |
| `std::vector<T>` | table, in order | table |
| `std::map<K, V>`, `std::unordered_map<K, V>` | table | table |
| `std::pair<A, B>`, `std::tuple<...>` | table of that many elements | table (or several returns at the top level) |
| registered class `T` | instance | new instance |

Containers nest: `std::map<std::string, std::vector<std::tuple<double, Sprite>>>` is a valid parameter and a valid return.

### Numbers

Luau numbers are doubles. Two consequences:

- An integer parameter refuses a fraction and a value outside its range. The check is exact: for a 64-bit integer, `2^63` is one past the largest value and is refused; `NaN` and infinity are always refused.
- An integer beyond 2^53 loses precision on the way in and out. A handle or hash that needs 64 bits should travel as a string.

### Keys

A `std::map<std::string, V>` parameter can be filled from a table with numeric keys: the key is read from a copy, so the iteration is not disturbed, and the number becomes its text (`{[1] = 2}` gives the key `"1"`).

## Custom types

A type luna does not know can be given a codec by specialising `luna::detail::CustomCodec`:

```cpp
template <> struct luna::detail::CustomCodec<Money, void> {
  static constexpr bool Enabled = true;
  static bool Read(lua_State *L, int index, Money &out, ArgError &error);
  static int Write(lua_State *L, const Money &value);       // pushes, returns the count
  static bool Accepts(lua_State *L, int index);             // optional: strict test for overloads
};
```

`luna::Delegate` is implemented this way. A codec is the only place a Luau type appears in code you write; the public types never need one.

---

[← Previous: Registering functions](registering-functions.md) · [Documentation index](README.md) · [Next: Namespaces, constants and enums →](namespaces-constants-and-enums.md)
