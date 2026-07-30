# Classes and userdata

`RegisterClass<T>` exposes a C++ class as typed userdata. It takes a name segment plus the class's `StableTypeKey`, and returns a `ClassBuilder<T>` that stages the whole class in the pending plan of its chain.

```cpp
Luna::ClassBuilder<Entity> Entities =
    Studio.RegisterClass<Entity>("Entity", EntityKey());
Entities.Constructor<std::string>()
    .Field("Tag", &Entity::Tag)
    .Property("Name", &Entity::Name, &Entity::Rename)
    .Method("Label", &Entity::Label)
    .StaticMethod("Category", &Entity::Category)
    .Documentation("One named host object.")
    .Documentation("New", "Constructs one entity from its name.")
    .Example("local E = Studio.Entity.New('crate')");
```

Registration stages exactly three things for one class: its canonical class type, its class symbol, and the cached metatable identity that type owns in this logical State. Nothing reaches the VM before the plan commits, and destroying an uncommitted builder has no effect.

The class is described, never discovered. The declared C++ shape your translation unit still knows — storage size, alignment, and whether the type is destructible, abstract, polymorphic, copyable, and movable — is captured where the type is complete. The registration backend never sees your type, which is what lets Luna allocate, construct, and release userdata storage without guessing a layout.

## Construction

Three kinds of construction candidate exist, and each is an ordinary callable candidate: same overload grouping, same parameter descriptors, same conversion registry, same transaction, same diagnostics. What they add is the result — exactly one value of the class.

```cpp





Sprites.Constructor<std::string, double, double>()
    .Constructor<>("Empty")
    .Factory("Square", &Sprite::Square)
    .Singleton("Current", &ActiveSprite);
```

- `Constructor<Args...>()` publishes under Luna's default constructor name, `New`. Several constructors of one class share that name and form one canonical overload set. `Constructor<Args...>(Name)` gives one an explicit name.
- `Factory` returns the class **by value** to state Lua ownership, or `std::shared_ptr<T>` to state shared ownership of exactly one reference.
- `Singleton` returns `T&`, `T*`, or `std::shared_ptr<T>`. It defaults to borrowed ownership with one Luna-owned lifetime that stays live for as long as the registration. A second overload takes an explicit `OwnershipPolicy`; a policy that contradicts the accessor's declared result is refused transactionally.

An object is published only after allocation, native construction, ownership establishment, identity-cache insertion, metatable association, and protected return publication have all succeeded. Any failure performs exactly the cleanup the completed steps warrant.

## Ownership

| Model | Who releases the object | Declared by |
|---|---|---|
| Lua-owned | Luna destroys it exactly once and releases the storage it allocated | a constructor, or a by-value factory |
| Shared | Luna holds and releases exactly one `std::shared_ptr` reference per stored object | a factory returning `std::shared_ptr<T>` |
| Borrowed | nobody — the owner's `LifetimeHandle` decides when access ends | a singleton accessor, or `OwnershipPolicy::Borrowed` |

`LifetimeHandle` is how a borrowed object states its lifetime. While the handle is live, access to every value exposed through it is permitted; the moment the owner calls `Invalidate()`, every later access to every one of those values fails before any native code runs.

```cpp
Luna::LifetimeHandle Lifetime;
Sprites.Singleton("Active", &ActiveSprite,
                  Luna::OwnershipPolicy::Borrowed(Lifetime));


Lifetime.Invalidate();
```

Invalidation is atomic and idempotent: calling it again changes nothing. Copies of a handle share its record, so a value exposed through any copy is rejected as soon as any copy is invalidated. The record is reference counted, so a handle may be destroyed while its values are still being cleaned up. Garbage collection of a Lua-owned object is likewise idempotent — the release path runs exactly once per value.

`LifetimeHandle::Undeclared()` declares no lifetime; exposing a borrowed value with it is refused, because a borrowed value always requires an explicit one.

## Allocators

`Allocator` states the semantic storage protocol Luna obtains and gives back storage through, for every value of the class it creates itself.

```cpp
Sprites.Allocator(Luna::ClassAllocator::ForOwnedObject<Sprite>());
```

The protocol is four semantic steps — obtain aligned storage, construct one object in it, destroy an object known to be constructed, give the storage back — and none of them names a VM, a Luau type, a stack index, or a registry reference. It prescribes no member names: supply whichever steps you want to own as ordinary callables, and Luna erases them, with whatever state they captured, into one immutable reference-counted record. Luna retains that record until the last userdata depending on it has finished cleanup, so an arena a value was allocated from is still reachable while that value is being destroyed.

Which steps you declare decides the cleanup a value can receive, and nothing else does. No deallocation step means Luna does not own the storage. No destruction step means Luna never destroys the object, which is exactly what a borrowed object requires. No allocation step means the protocol can only describe an object that already exists.

Three ready-made protocols cover the common cases: `ForOwnedObject<T>()` (global aligned storage, in-place default construction, ordinary destruction, matching deallocation), `ForAdoptedObject<T>()` (Luna destroys but never allocates or deallocates), and `ForStorage<T>(Identity, Allocate, Deallocate)` (your storage, Luna's construction and destruction). `Undeclared()` names no protocol at all.

The selection belongs to the class rather than to one declaration, so it may be stated before or after the candidates it applies to; stating it twice keeps the last protocol named. A protocol Luna could not actually create and release a value through is refused transactionally rather than reinterpreted.

## Methods

`Method` stages an instance method; `StaticMethod` stages one with no receiver.

```cpp
Sprites.Method("Bounds", &Sprite::Bounds)
    .Method("Grow", &Sprite::Grow)
    .StaticMethod("Category", &Sprite::Category);
```

An instance method states the object it operates on rather than having it guessed: a member function pointer of the class — or of one of its bases — states it through its own class and const qualification, and an explicit wrapper states it through a first parameter of `Type &`, `const Type &`, `Type *`, or `const Type *`.

The receiver is rank position zero of every call. Its presence, origin State, metatable identity, payload liveness, lifetime, dynamic type, and const permission are all validated before one ordinary argument is inspected. That is what makes these three one call rather than three spellings that happen to agree:

```lua
Hero:Grow(2)
Hero.Grow(Hero, 2)
Studio.Sprite.Grow(Hero, 2)
```

It is also why a dot call without a receiver fails as a receiver refusal instead of as a shifted argument. A const method may be called through a const value of the class; a non-const one may not. A virtual method dispatches through the object the call site supplied.

Several methods under one name form one canonical overload set, resolved by exactly the rules every other overload set follows. Two whose declared shapes no call could tell apart are refused transactionally.

A static method declares no receiver at all, so it is an ordinary callable candidate of the class scope reached by a dot call.

## Properties and fields

A property is a declared getter, optionally a declared setter, plus a `PropertyPolicy` stating direction and evaluation.

```cpp
Sprites.Property("Area", Luna::PropertyPolicy::Lazy(), &Sprite::Area)
    .Property("Name", [](const Sprite &Value) { return Value.Name(); },
              [](Sprite &Value, std::string Renamed) {
                Value.Rename(std::move(Renamed));
              })
    .Field("Width", &Sprite::Width)
    .Field("Height", &Sprite::Height, Luna::FieldPolicy::ReadOnly());
```

A single getter means the plain read-only immediate property. A getter plus a setter means read-write; both must carry the same declared value type, and the setter always requires a mutable view, so a const value permits the read and refuses the write before native code runs.

`PropertyPolicy` states everything else explicitly: `ReadOnly()`, `WriteOnly()`, `ReadWrite()`, `Computed()`, `ComputedReadWrite()`, `Lazy()`, `LazyReadWrite()`. `Immediate` reads the value the object already holds. `Computed` runs the getter on every read and never reuses a result. `Lazy` runs the getter until it succeeds once, then reuses that result for that exposed object under that dispatch generation; a failed getter is never reused, and a successful write invalidates a cached value. A policy that contradicts the accessor it was given — a write-only property claiming lazy evaluation, for instance — is refused transactionally.

A field is never raw memory access. Luna generates the same getter and setter descriptors a property uses, so a field obeys its declared type, its constness, and its ownership restriction like every other member. The declared value is copied across the boundary in both directions, so no reference into an object Luna does not own can escape. `FieldPolicy` offers `ReadOnly()`, `ReadWrite()`, and `Owned(MemberOwnership)`; only `MemberOwnership::Copied` is honored, because that is the only ownership Luna can promise across the member boundary. A const-qualified data member is read-only whatever else is stated.

A property or field value is one of the supported value types.

### Reacting to a write

A read-write `Property` or a writable `Field` may declare one more, optional argument: an on-change callback, invoked with the object and the newly written value immediately after the setter or field write it follows has already succeeded.

```cpp
Sprites.Property("Name", &Sprite::Name, &Sprite::Rename,
                 [](Sprite &Value, const std::string &Renamed) {
                   Value.NotifyRenamed(Renamed);
                 })
    .Field("Width", &Sprite::Width,
          [](Sprite &Value, const double &Updated) {
            Value.InvalidateBounds();
          });
```

The callback receives the object by reference and the value already written, by the same declared type the setter accepts. It runs only after a successful write — never for a write refused by the receiver, the declared direction, or an incompatible value — and it never itself un-does that write: an exception it throws is contained exactly like an exception from a setter, but the underlying value has already changed. A field's on-change callback is unreachable when the field permits no writes at all, since there is then never a write for it to follow.

### Custom value types

A property or field value is not limited to the four supported value types. Any type with its own `Luna::TypeConverter<T>` specialization — a `Vector3`, a `Color`, anything a consumer has already taught Luna to convert (see [custom conversions](values-and-validation.md#custom-conversions)) — may be a property's or a field's value too. Name it explicitly as the first template argument, the same way `Base<T>` and `Cast<T>` name a related class explicitly:

```cpp
Studio.RegisterClass<Body>("Body", BodyKey())
    .Constructor<>()
    .Property<Vector3>("Position", &Body::GetPosition, &Body::SetPosition)
    .Field<Vector3>("Velocity", &Body::Velocity);
```

The getter and setter, or the field itself, still declare their native C++ type exactly as they would for a scalar member — `Vector3 GetPosition() const`, `void SetPosition(Vector3)`, `Vector3 Velocity`. Reading the member runs the value type's `Write`; writing it runs the value type's `Read`; both go through the identical probe/read/write boundary a converted parameter or return value uses, so a converted member gets the same diagnostics, the same reservation discipline, and the same atomicity guarantees. A getter or setter whose value type declares no `Luna::TypeConverter<T>` specialization is refused at compile time, the same way an unsupported scalar type is. `Property<Value>` and `Field<Value>` accept the same policy, on-change, and getter/setter-shape overloads their scalar counterparts do.

## Inheritance and casts

`Base<BaseType>(BaseKey)` states one explicit base edge to an already registered class.

```cpp
Sprites.Base<Entity>(EntityKey());
```

Whether `BaseType` really is a base, whether it is reachable through one unambiguous public path, and how a pointer is adjusted are all captured where both types are complete. The edge is then accepted only as part of the whole relationship graph of the attempt, so declaration order never changes the outcome. A base that is not registered, an inaccessible base, a duplicate edge, an edge closing a cycle, and any pair of classes reachable through more than one path are all refused transactionally.

One registered accessible path is what permits a value of the derived class to be received as a receiver of the base — nothing else does:

```lua
HostLog(Studio.Entity.Label(Hero))
```

`Cast<SourceType>(SourceKey)` states the safe downcast policy that permits a value exposed as `SourceType` to be received as this class, using internal runtime type assistance. A downcast is never implicit: without the declaration, a base value simply is not a value of this class. The compatibility check is non-mutating and runs before any conversion is committed and before any native target is invoked. A second overload takes an explicit policy identity and a stateless check receiving the object as `const SourceType &`; a check that refuses is an ordinary incompatible object. The assistance stays internal — no persistent identity of the class derives from a runtime type name or address.

**Current limitation.** Inherited *fields* are not reachable through a derived class. Each class exposes the members it declares, so reach a base field through a value of the declaring base:

```lua
local Crate = Studio.Entity.New("crate")
Crate.Tag = 7
```

## Operators

`Operator` stages one declared operator behavior.

```cpp
Sprites.Operator(Luna::ClassOperator::Add, &Sprite::Padded)
    .Operator(Luna::ClassOperator::Length, &Sprite::Pixels)
    .Operator(Luna::ClassOperator::ToText, &Sprite::ToText)
    .Documentation(Luna::ClassOperator::Add,
                   "Sprite + padding is the padded area.");
```

`ClassOperator` covers `Call`, `Length`, `Equal`, `Less`, `LessEqual`, `Add`, `Subtract`, `Multiply`, `Divide`, `Modulo`, `Power`, `Negate`, `Concatenate`, `ToText`, `Index`, `Assign`, and `Iterate`. The target states its receiver exactly the way an instance method does, and every operand after that receiver is an ordinary parameter. So the receiver is rank position zero of an operator call too, validated before one operand is inspected, and the candidate resolves through the same canonical overload rules.

The operand count of each operator is fixed, and a declaration taking a different number of them — or taking one optionally — is refused transactionally. Two operators are exceptions: `Call` forwards whatever the call site supplied, and `Iterate` receives one control operand that may be omitted, described under [Iteration](#iteration) below.

`Index` and `Assign` keep Luna's own metamethods. The declaration is the behavior Luna consults for a name the class declares nothing for, never a replacement of Luna's reserved dispatch.

### Iteration

`Iterate` makes a class usable in a Luau generic `for` loop. Its target is one step of the loop rather than an iterator object: it receives the control value the previous step published first — omitted on the first step — and returns a `Luna::ReturnPack` of everything that step produced. An empty pack ends the loop.

```cpp
Luna::ReturnPack Roster::Step(std::optional<int> Control) const {
  const int Next = Control ? *Control + 1 : 1;
  if (Next > static_cast<int>(Names.size()))
    return Luna::ReturnPack::Empty();
  Luna::ReturnPack Produced;
  Produced.AppendInteger(Next).AppendText(Names[Next - 1]);
  return Produced;
}
```

```cpp
Rosters.Operator(Luna::ClassOperator::Iterate, &Roster::Step);
```

```lua
for Position, Name in Roster do
  HostLog(Position, Name)
end
```

Luna supplies the loop's iterator itself, so the step never has to produce a Luau function: `Iterate` is the one operator whose control operand may be optional or defaulted, and the one besides `Call` whose result count the call decides rather than the operator. A step that publishes a single scalar instead of a pack is refused transactionally.

Operators are documented, annotated, and exemplified by naming the operator rather than the Luna-owned segment it is published under: `Documentation(ClassOperator, Text)`, `Attribute(ClassOperator, Name, Value)`, `Example(ClassOperator, Text)`.

### Converted operands

A `Method`, `StaticMethod`, or `Operator` parameter is not limited to the four supported value types either: any type with its own `Luna::TypeConverter<T>` specialization may be an operand too, read through the same probe/read boundary a converted property or field value already uses (see [custom conversions](values-and-validation.md#custom-conversions)). No explicit template argument is needed — the parameter's own declared C++ type is enough, the same way a scalar parameter already is:

```cpp
Sprites.Method("Dot", &Sprite::Dot)
       .Operator(Luna::ClassOperator::Add, &Sprite::Offset);
```

```lua
local D = Hero:Dot({1, 0, 0})
local Moved = Hero + {4, 0, 0}
```

Diagnostics for a converted operand read the same way a converted member value's do: a value the operand's own `Probe` rejects is a caller error naming the operand position, reported before the native target runs. Publishing a converted *return* value is not yet supported — but a method that wants to hand back a table or an object says so directly, described under [returning instances and tables](#returning-instances-and-tables) below.

### Instance operands

A registered class is an operand type in its own right. The class opts in once, and then a bare `T`, `const T &`, `T &`, `T *`, or `const T *` in any method, static method, operator, constructor, factory, or singleton signature names one instance of it:

```cpp
template <> struct Luna::RegisteredClassTrait<Bead> : std::true_type {};

struct Slot final {
  int Total = 0;
  [[nodiscard]] int Measure(const Bead &Placed) const;
  void Absorb(Bead *Placed);
};

Slots.Method("Measure", &Slot::Measure).Method("Absorb", &Slot::Absorb);
```

```lua
local S, B = Studio.Slot.New(), Studio.Bead.New()
B.Weight = 7
print(S:Measure(B))   -- an instance of another class as the operand
S:Absorb(B)           -- and the caller's own object is what was written
```

The opt-in is deliberate rather than automatic. Treating every class type as an operand would silently turn long-standing compile-time refusals — `std::string_view`, `Luna::ReturnPack`, a `std::function` of an unsupported shape, a native event source — into registration-time failures, so a class states its own participation instead.

The operand's class identity comes from what `RegisterClass<T>` recorded for that C++ type, so an operand may be an instance of any registered class, not only of the class declaring the member. The first registration of a C++ type wins: registering the same type under a second key later keeps the original identity rather than changing what earlier declarations meant.

A mutable operand — `T &` or `T *` — requires a mutable instance, exactly as a non-const receiver does; a `const` spelling accepts either. Every operand passes the receiver gate before the native target runs, so origin State, metatable identity, payload liveness, borrowed lifetime, dynamic type, and const permission all produce a receiver-quality refusal at the operand's own position. An operand of the wrong class, a scalar, or an omitted operand is a caller error.

One ordering rule applies: a plan is validated entry by entry in staging order, so a class has to be registered before a member that takes one of its instances is declared. Declaring them the other way round is refused with a diagnostic naming the operand position and the unregistered class.

### Converted operands carrying instances

A registered class also reaches a *converted* operand, which is useful when the native signature wants a value type rather than a handle. A class instance arriving at a converted operand is visible to that operand's `TypeConverter<T>` as a userdata value, and `ValueView` exposes everything needed to recover the native object from it:

```cpp
[[nodiscard]] ConversionResult<Vector3> Read(Luna::ValueView Source,
                                             Luna::ConversionContext &Context) const {
  ConversionResult<Vector3> Result;
  if (Source.IsUserdata() && Source.UserdataClassName() == "Studio.Vector3") {
    if (void *Object = Source.UserdataStorage()) {
      Result.Status = ConversionStatus::Success;
      Result.ConvertedValue = *static_cast<const Vector3 *>(Object);
      return Result;
    }
  }
  Result.Status = ConversionStatus::TypeMismatch;
  Result.Diagnostic = Context.Describe("expected a Vector3");
  return Result;
}
```

`UserdataClassName()` is the registered qualified name and `UserdataType()` is the canonical `TypeId`, so the converter confirms the identity before it casts — a value of a different class simply fails the check. `UserdataStorage()` yields null whenever handing the object out would be unsound: the capture has expired, the owning State has gone away, or the object is no longer published, which is the same stale-borrow refusal a receiver performs. `UserdataPermitsMutation()` reports whether the instance was exposed mutably, and `UserdataText()` carries whatever the class's `ToText` operator rendered.

### Returning instances and tables

A method, static method, or operator returns a registered class instance in any of the three ownership models a construction publishes:

| Declared result | Ownership | Declaration |
|---|---|---|
| `T` by value | Lua-owned copy | nothing extra |
| `std::shared_ptr<T>` | shared | nothing extra |
| `T *` | borrowed | `OwnershipPolicy::Borrowed(Lifetime)` |

```cpp
struct Vector { double X, Y; [[nodiscard]] Vector Scaled(double By) const; };
struct Game   { Http Service; [[nodiscard]] Http *GetService(std::string Name); };

Vectors.Method("Scaled", &Vector::Scaled);
Games.Method("GetService", &Game::GetService,
             Luna::OwnershipPolicy::Borrowed(HostLifetime));
```

```lua
local V = Studio.Vector.New()
local S = V:Scaled(2.5)                       -- a new object, owned by Lua
local http = game:GetService("HttpService")   -- the host's own object, borrowed
```

A borrowed result outlives the call, so it states the lifetime Luna checks on every later access — exactly what a borrowed singleton states. Declaring a pointer result without one is refused at registration rather than at the first call, and declaring a lifetime for a result that is not borrowed is refused too.

Two objects must not share one native address. Luna caches userdata identity by address, so a member at offset zero of its owner would be the same identity as its owner.

For a value whose shape the target decides at run time, return `Luna::OwnedValue` for one value or `Luna::ValuePack` for ordered multiple values. Both publish whatever category each value carries — a table, nested tables, a scalar, nil, or an instance the call itself received:

```cpp
[[nodiscard]] Luna::OwnedValue Http::Decode(std::string Text) const {
  Luna::OwnedValue Decoded = Luna::OwnedValue::Table();
  Decoded.SetField("enabled", Luna::OwnedValue::Boolean(true));
  Luna::OwnedValue Names = Luna::OwnedValue::Table();
  Names.Append(Luna::OwnedValue::Text("alpha"));
  Decoded.SetField("names", std::move(Names));
  return Decoded;
}
```

```lua
local decoded = http:Decode(encoded)
print(decoded.enabled, decoded.names[1])
```

`ReturnPack` stays deliberately scalar-only. It is the *declared* multiple-return shape, and widening its element type would change what every existing `AppendNumber`/`AppendText` call site means. A target that needs a table inside a multiple return returns a `ValuePack` instead. An owned return states its reservation before publishing, so a value exceeding the invocation's string policy publishes nothing rather than a truncated prefix.

One limit is worth naming: an `OwnedValue` carrying userdata holds a reference to an instance the call *received*, so a pack can pass an instance through but cannot manufacture a new one. A method returning a newly built object returns it directly rather than nesting it in a pack.

## Access from Luau

```lua
local Hero = Studio.Sprite.New("hero", 3, 4)
HostLog(tostring(Hero))          -- ToText operator
HostLog(Hero.Width)              -- field
HostLog(Hero.Area)               -- lazy property
HostLog(#Hero)                   -- Length operator
HostLog(Hero + 2)                -- Add operator, number operand

for Field, Value in Hero do      -- Iterate operator
  HostLog(Field, Value)
end

local Width, Height = Hero:Bounds()   -- ordered multiple returns
HostLog(Hero:Grow(2))
Hero.Name = "champion"                -- read-write property

HostLog(Studio.Sprite.Category())     -- static method, no receiver
HostLog(tostring(Studio.Sprite.Square("tile", 2)))  -- factory
```

Every one of those goes through the same validated access gate. A missing receiver, a value from another State, a stale borrowed object, a wrong dynamic type, a const violation, or an unconvertible operand each produce one deterministic refusal before native code runs.

---

[← Previous: Namespaces, constants and enums](namespaces-constants-and-enums.md) · [Documentation index](README.md) · [Next: Modules and versioning →](modules-and-versioning.md)
