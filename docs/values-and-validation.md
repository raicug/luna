# Values and validation

Luna has two layers here, and it helps to keep them apart. The **supported value types** are what a C++ signature may spell. The **canonical type model** is the normalized vocabulary Luna identifies, reflects, and converts through.

## Supported value types

These are the types a registered signature uses for parameters and returns. A property or field value may additionally be a registered class instance — see [instance values](classes-and-userdata.md#instance-values). A property or field value, and any parameter of a registered callable — root, namespaced, or a `Method`/`StaticMethod`/`Operator`/`Constructor`/`Factory`/`Singleton` — may additionally be any type with its own `Luna::TypeConverter<T>` specialization — see [custom conversions](#custom-conversions) below and [classes and userdata](classes-and-userdata.md#custom-value-types). Publishing a converted *return* value is not yet supported; a target that needs to hand back a table returns an `OwnedValue` instead.

| C++ type | Luau value | Notes |
|---|---|---|
| `bool` | boolean | No truthiness conversion |
| `int` | number | Signed 32-bit; exact integers only |
| `double` | number | Finite values, signed zero, infinities, and NaN are accepted |
| `std::string` | string | Byte-preserving, including embedded NUL bytes |
| `void` | no return | Return type only |

Parameters may additionally be a trailing `std::optional<T>` of one of these, a delegate (`Delegate<Signature>` or `std::function<Signature>`), a converted operand, a registered class instance, or one final variadic `ArgumentView` / `ArgumentPack`. Returns may additionally be `std::pair`, `std::tuple`, or `ReturnPack` of these; a registered class instance; or `OwnedValue` / `ValuePack` for a value whose shape the target decides at run time. See [registering functions](registering-functions.md) for the declared shapes and [classes and userdata](classes-and-userdata.md#returning-instances-and-tables) for instance and table results.

Luna does not coerce strings to numbers, numbers to booleans, or other near matches. The Luau type must agree with the declared C++ type.

## The canonical type model

Every declaration is described by a `TypeDescriptor`: an immutable normalized value where references and top-level cv-qualifiers are already removed, while pointer depth, pointee qualification, array extent, semantic wrappers, enum identity, class identity, and ordered structural children are all preserved.

Fixed Luna-owned leaves are named by `FixedTypeKey`: `Void`, `Boolean`, `Int32`, `Float`, `Double`, `String`, `StringView`, `CString`, `Null`, `Value`, `ValuePack`. Their canonical names live under the reserved `luna.` prefix, so a consumer type can never claim one.

Type constructors are named by `TypeKind`: `Fixed`, `Enumeration`, `Class`, `Converted`, `Pointer`, `Array`, `Optional`, `Sequence`, `Map`, `Pair`, `Tuple`, `SharedOwnership`, `BorrowedReference`, `ArgumentPack`, `ReturnPack`, `Callable`, plus `Unsupported`. `Converted` is the leaf a `TypeConverter<T>` operand canonicalizes to, and `Callable` is a delegate's declared call shape. Each constructor has a fixed canonical arity; a tuple or pack accepts any child count and a callable accepts at least one child.

A descriptor hashes and compares structurally, and it never mixes in an address, a registration order, or a process-random seed. That is what makes `TypeId` and `SymbolId` — 256-bit content-derived digests — portable across States, executions, and generated artifacts.

The model is wider than what a function signature accepts today. `Float`, `StringView`, `CString`, sequences, maps, and fixed arrays are canonical types Luna identifies and reflects — a class property or field may name them — while a registered constant normalizes only to `luna.bool`, `luna.int32`, `luna.float64`, `luna.string`, or an enumeration, and a callable parameter is one of the declared parameter forms above.

## Stable keys for your own types

An enum or a class is a *user-defined leaf*. Luna will not guess its identity from an RTTI name, an address, or the order you registered it, so you supply one explicitly:

```cpp
const Luna::StableTypeKey SpriteKey("app.render.Sprite");
```

A key is 1–256 bytes of ASCII, dot-separated identifier segments, and must not begin with `luna.`. `StableTypeKey::Classify` reports exactly why a text is rejected: `Empty`, `TooLong`, `EmptySegment`, `InvalidLeadingCharacter`, `InvalidCharacter`, or `ReservedPrefix`. The same key must be used everywhere the type is named — its registration, a constant of it, a base edge pointing at it.

## Constants

`RegisterConstant` accepts a wider set of C++ inputs than a signature does, because it normalizes the value in your translation unit before it ever reaches the registry:

| Declared C++ value | Canonical type |
|---|---|
| `bool` | `luna.bool` |
| any integral within the signed 32-bit range | `luna.int32` |
| any floating-point type | `luna.float64` |
| anything convertible to `std::string_view` | `luna.string` |
| an enum, with its `StableTypeKey` | that enumeration |

An unsupported C++ type, a user-defined leaf without its key, and an integer outside the canonical range each produce a deterministic refusal rather than a guessed conversion. An out-of-range refusal reports the value it received verbatim.

## Validation order

Invocation checks are deliberately predictable:

1. required callable metadata must exist and be consistent
2. for a member, the receiver is validated first — it is rank position zero
3. call arity must fit the declared shape
4. candidates are probed and the dominating one is selected
5. arguments are converted from left to right
6. type compatibility is checked before value-specific rules
7. validation stops permanently at the first failure

An `int` argument is checked for numeric type, finiteness, the inclusive range `[-2147483648, 2147483647]`, and then integrality. This order determines which diagnostic is returned.

Strings use length-aware Luau APIs. Bytes are copied exactly, including zero bytes, with a maximum length reported by `MaximumConversionStringBytes()` — currently 1,048,576 bytes in either direction. Oversized arguments are rejected before user code runs; oversized returns become internal return-conversion failures.

## Invocation and returns

The callable runs exactly once, and only after every argument validates. Validation failures never invoke the callable. Return-writing failures restore the callback stack and expose no partial result: multiple returns are published atomically or not at all.

## Custom conversions

A consumer can teach Luna a new type by specializing `Luna::TypeConverter<T>`. The whole boundary is Luna-owned and standard-library types: no VM type, header, pointer, stack index, or macro appears in it.

A specialization supplies three operations:

- `Probe` receives `const ConversionContext&` and reports viability and rank. It never converts and never mutates. Probing purity is enforced by the type system — every mutating, allocating, or publishing operation is non-const — and a violation reaching Luna through a const cast is recorded.
- `Read` receives `ConversionContext&` and returns a `ConversionResult<T>`, carrying either a value or one deterministic status: `TypeMismatch`, `MissingElement`, `OutOfRange`, `PolicyExceeded`, `IncompleteAggregate`, `Rejected`, and so on.
- `Write` reserves resources and then publishes, at most once.

```cpp
struct AppColor final {
  double Red = 0.0;
  double Green = 0.0;
  double Blue = 0.0;
};

namespace Luna {

template <> class TypeConverter<AppColor> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    static_cast<void>(Context);
    if (!Source.IsTable() || Source.Size() != 3)
      return RejectedProbe("a color is a table of three numbers");
    return ViableProbe(ConversionRank::User);
  }

  [[nodiscard]] ConversionResult<AppColor>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<AppColor> Result;
    AppColor Converted;
    double *Channels[] = {&Converted.Red, &Converted.Green, &Converted.Blue};
    for (std::size_t Index = 0; Index < 3; ++Index) {
      const std::optional<double> Channel = Source.Element(Index).ToNumber();
      if (!Channel) {
        Result.Status = ConversionStatus::TypeMismatch;
        Result.Diagnostic = Context.Describe("a color channel is a number");
        return Result;
      }
      *Channels[Index] = *Channel;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = Converted;
    return Result;
  }

  [[nodiscard]] WriteResult Write(const AppColor &Source,
                                  ConversionContext &Context) const {
    OwnedValue Published = OwnedValue::Table();
    Published.Append(OwnedValue::Number(Source.Red));
    Published.Append(OwnedValue::Number(Source.Green));
    Published.Append(OwnedValue::Number(Source.Blue));

    const WriteResult Reserved = Context.Reserve(Published.RequiredReservation());
    if (!Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

}
```

Every value on this boundary reports its category through `ValueCategory` — `None`, `Nil`, `Boolean`, `Number`, `String`, `Table`, `Userdata`, `Function` — with `ValueCategoryText` for the text form.

Three types carry the boundary. `ValueView` is a transient token naming one value inside the current conversion frame: it exposes shape only, carries no pointer or stack index, and becomes inert when its frame ends, so retaining one can never reach released VM storage. `OwnedValue` and `ValuePack` are owning values that outlive any frame — `ToOwned()` is how you keep something. `ConversionContext` is the frame itself: it reports the shape under conversion, the callable name, the one-based position, and the complete nested path such as `argument 2[4].Key`, and `Describe` turns a reason into one deterministic diagnostic carrying all of it.

The same specialization is what makes `AppColor` usable as a property or field value (`Property<AppColor>(...)`, `Field<AppColor>(...)`) and as a `Method`, `StaticMethod`, `Operator`, `Constructor`, `Factory`, or `Singleton` operand — declared with no explicit template argument there, since the parameter's own C++ type already names it:

```cpp
Sprites.Method("Tint", &Sprite::Tint);
Sprites.Factory("Span", &Span);
```

A registered class is a parameter type in its own right — see [instance operands](classes-and-userdata.md#instance-operands) — but `ValueView` also reaches a class instance arriving at a *converted* operand, for the case where the native signature wants a value type rather than a handle. When the value under conversion is one, `IsUserdata()` is true and the view reports the registered `UserdataClassName()`, the canonical `UserdataType()`, the `UserdataText()` its `ToText` operator rendered, `UserdataPermitsMutation()`, and `UserdataStorage()` — the native object itself. Storage is decided at the moment it is requested, through the same access gate a receiver passes, so it yields null for a value from another State, an unpublished or destroyed object, or a borrowed object whose lifetime handle has been invalidated. A converter that confirms the class before it casts therefore refuses a wrong-class or stale operand with the same quality a receiver refusal has, and does it inside `Probe`, before the native target runs:

```cpp
[[nodiscard]] ConversionProbe Probe(Luna::ValueView Source,
                                    const Luna::ConversionContext &Context) const {
  if (!Source.IsUserdata() || Source.UserdataClassName() != "Studio.Vec")
    return Luna::RejectedProbe(Context.Describe("expected a Studio.Vec"));
  if (Source.UserdataStorage() == nullptr)
    return Luna::RejectedProbe(Context.Describe("the instance is no longer usable"));
  return Luna::ViableProbe(Luna::ConversionRank::Exact);
}
```

Writers state everything they need through `ValueReservation` before publishing, and publication is refused unless the complete value fits the reservation. `OwnedValue::RequiredReservation()` computes exactly what one publication needs, and `ConversionContext::Publish` publishes one value while `PublishPack` publishes an ordered `ValuePack`.

---

[← Previous: Registering functions](registering-functions.md) · [Documentation index](README.md) · [Next: Namespaces, constants and enums →](namespaces-constants-and-enums.md)
