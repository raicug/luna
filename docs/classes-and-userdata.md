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
    .Singleton("Current", &AccessActiveSprite);
```

- `Constructor<Args...>()` publishes under Luna's default constructor name, `New`. Several constructors of one class share that name and form one canonical overload set. `Constructor<Args...>(Name)` gives one an explicit name.
- `Factory` returns the class **by value** to state Lua ownership, or `std::shared_ptr<T>` to state shared ownership of exactly one reference.
- `Singleton` takes an accessor returning `T&`, `T*`, or `std::shared_ptr<T>` — an accessor, not the address of an object. Without an explicit policy it states borrowed ownership with one Luna-owned lifetime that stays live for as long as the registration, so a `T&`/`T*` accessor needs nothing more; an accessor returning `std::shared_ptr<T>` states `OwnershipPolicy::Shared()` through the second overload. A policy that contradicts the accessor's declared result is refused transactionally.

An object is published only after allocation, native construction, ownership establishment, identity-cache insertion, metatable association, and protected return publication have all succeeded. Any failure performs exactly the cleanup the completed steps warrant.

## Class constants

`Constant` declares one immutable value on the class table itself, so a leaf value is a plain read rather than a call:

```cpp
Vectors.Constant("Dimensions", 2)
       .Constant("xAxis", "1,0")
       .Constant("DefaultChannel", Channel::Info, ChannelKey())
       .Documentation("Dimensions", "How many axes one vector carries.");
```

```lua
assert(Studio.Vector.Dimensions == 2)
assert(Studio.Vector.DefaultChannel == Studio.Channel.Info)
```

It accepts exactly what `RegisterConstant` accepts — a `bool`, an integer in the canonical 32-bit range, a floating-point value, anything convertible to `std::string_view`, or an enum with its `StableTypeKey` — and it stages into the same class plan every other declaration of the class stages into, so it publishes with them or not at all. A constant is documented and annotated by naming it, exactly as a member is.

A class constant occupies a name on the class table, so it must not collide with a member, a static method, a construction candidate, or another constant of that class. Each collision is refused transactionally with a diagnostic naming what already holds the name. Reflection publishes one `Constant` record scoped to the class, carrying its declared type and value text, and generated `.d.lua` declares it as a field of the class table rather than as a function.

**Not yet available.** A constant's value is one of the canonical constant types above, so `Vector3.zero` cannot yet be a real `Vector3` instance. Publishing one interned Lua-owned instance per State is a distinct piece of work: declare a zero-argument factory or a static method until it lands.

## Ownership

| Model | Who releases the object | Declared by |
|---|---|---|
| Lua-owned | Luna destroys it exactly once and releases the storage it allocated | a constructor, or a by-value factory |
| Shared | Luna holds and releases exactly one `std::shared_ptr` reference per stored object | a factory returning `std::shared_ptr<T>` |
| Borrowed | nobody — the owner's `LifetimeHandle` decides when access ends | a singleton accessor, or `OwnershipPolicy::Borrowed` |

`LifetimeHandle` is how a borrowed object states its lifetime. While the handle is live, access to every value exposed through it is permitted; the moment the owner calls `Invalidate()`, every later access to every one of those values fails before any native code runs.

```cpp
Luna::LifetimeHandle Lifetime;
Sprites.Singleton("Active", &AccessActiveSprite,
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

Which steps a protocol declares decides the cleanup a value can receive, and nothing else does. No deallocation step means Luna does not own the storage. No destruction step means Luna never destroys the object, which is exactly what a borrowed object requires. No allocation step means the protocol can only describe an object that already exists.

A protocol *selected for a class* must declare all four steps, because Luna both creates and releases every value of the class it constructs itself. Two ready-made protocols qualify: `ForOwnedObject<T>()` (global aligned storage, in-place default construction, ordinary destruction, matching deallocation) and `ForStorage<T>(Identity, Allocate, Deallocate)` (your storage, Luna's construction and destruction). `ForAdoptedObject<T>()` (Luna destroys but never allocates or deallocates) and `Undeclared()` (no protocol at all) describe values Luna did not allocate — an adopted, shared, or borrowed object — so selecting either for a class is refused, as is a protocol whose size or alignment cannot hold the class.

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

A class member publishes its value inside the call that asked for it, so an `AsyncTask<T>` or `std::future<T>` result is refused at registration: asynchronous delivery belongs to namespace and root functions. The same refusal covers operators and construction candidates.

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

A property or field value is one of the supported value types, a type with its own `Luna::TypeConverter<T>`, or a registered class instance.

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

The getter and setter, or the field itself, still declare their native C++ type exactly as they would for a scalar member — `Vector3 GetPosition() const`, `void SetPosition(Vector3)`, `Vector3 Velocity`. Reading the member runs the value type's `Write`; writing it runs the value type's `Read`; both go through the identical probe/read/write boundary a converted parameter or return value uses, so a converted member gets the same diagnostics, the same reservation discipline, and the same atomicity guarantees. A getter or setter whose value type declares no `Luna::TypeConverter<T>` specialization is refused at compile time, the same way an unsupported scalar type is. `Property<Value>` and `Field<Value>` accept the same policy and getter/setter-shape overloads their scalar counterparts do. Two differences are worth knowing: an on-change callback is scalar-only, and a converted read runs on every access even under `Lazy()`, because a lazily cached value is one of the four supported value types.

### Instance values

A property or field value may be a **registered class instance**, so a derived object reaches a script as typed userdata rather than as a table or a call. The value type is inferred from the accessor's own declared return type — exactly the way an instance operand and an instance result are — so no explicit template argument names it:

```cpp
Vectors.Property("Unit", &Vector3::Unit)
       .Property("Magnitude", &Vector3::Magnitude);

Bodies.Field("Position", &Body::Position)
      .Property("Anchor", &Body::Anchor)
      .Property("Service", &Body::Service,
                Luna::OwnershipPolicy::Borrowed(HostLifetime));
```

```lua
local U = V.Unit                 -- userdata, not a table
assert(U.X == 0.6)
local http = Part.Service        -- the host's own object, borrowed
```

Inference is what tells the two member value kinds apart when a type is both a registered class and conversion-capable. A plain `Property`/`Field` publishes the instance, because the accessor's own type names a registered class; `Property<Value>`/`Field<Value>`, which names the value type explicitly, still publishes the converted form. So one type can reach a script as userdata through one member and as a table through another, and each member says which it means at its declaration.

The three ownership models are the ones an instance result already declares, and a member states them the same way:

| Declared value | Ownership | Declaration |
|---|---|---|
| `T` by value — a getter returning `T`, or a data member of type `T` | one Lua-owned copy per read | nothing extra |
| `std::shared_ptr<T>` | shared | nothing extra |
| `T *` | borrowed | `OwnershipPolicy::Borrowed(Lifetime)` |

A by-value member publishes a copy, so writing the published object never reaches the owner's storage; a borrowed member publishes the owner's own object, so a write through it is observed by the next read. A pointer-valued member declared without a lifetime is refused at registration, as a borrowed result is, and the class whose instance a member publishes has to be registered before the member is declared — the same staging-order rule instance operands follow.

#### Writing an instance-valued property

An instance-valued **property** is read-write when it declares a setter. The getter states the value type as before; the setter takes the same registered class as an *instance operand*, spelled exactly as a method parameter is — `T`, `const T &`, `T &`, `T *`, or `const T *`:

```cpp
Bodies.Property("Position", &Body::Where, &Body::Place)
      .Property("Link", &Body::Link, &Body::Attach,
                Luna::OwnershipPolicy::Borrowed(HostLifetime));
```

```lua
Part.Position = Vector3.new(2, 7, 0)
assert(Part.Position.X == 2)
```

The ownership policy an instance-valued property already needed for its *getter* is stated after the setter, so a borrowed getter and a setter live in the same declaration.

The write path is the instance-parameter path, not a value conversion. The incoming value has to be live userdata of that class; an accessible base or a declared `Cast` satisfies it exactly as it does for a method operand; a `T &` or `T *` setter requires a mutable view and refuses a const one before native code runs; and an instance of another registered class is refused with the diagnostic a mismatched instance parameter already produces. The native object is handed to the setter directly, so a `T &` or `T *` setter sees the caller's own object rather than a copy of it.

Getter and setter must name the same class. A property with no setter stays read-only, which remains the default.

Two things are refused for this shape:

- An **on-change handler** receives the newly written value as one canonical Luna value, which an instance is not, so `Property(Name, Getter, Setter, OnChange)` on an instance-valued property is refused at registration.
- A writable instance-valued **field** is still refused. A field is the value the object already holds, and assigning through it would have to copy into the owner's storage under a policy no `FieldPolicy` states. Declare a property with a getter and a setter instead; the diagnostic says so.

A read runs on every access even under `PropertyPolicy::Lazy()`, because a lazily cached value is one of the four supported value types.

**The `Assign` operator never applies to a declared member.** `Assign` is the unknown-key write operator: `__newindex` consults it only when the key names no declared member of the class. A member that exists and permits no write refuses with its own read-only diagnostic rather than diverting to `Assign`, so `Part.Heading = v` on a read-only `Heading` is a deterministic refusal even when the class declares `Assign`. `Index` behaves the same way on the read side.

Two objects still must not share one native address. A borrowed member pointing at a data member at offset zero of its owner names the same address the owner's own userdata does; Luna keys userdata identity by address, so that read is refused deterministically at the moment it is published rather than aliasing the owner. The collision is only visible once an address is known, so it is a read-time refusal, not a registration-time one.

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

The operand count of each operator is fixed, and a declaration taking a different number of them — or taking one optionally — is refused transactionally. `Length`, `Negate`, and `ToText` take none; the arithmetic, comparison, `Concatenate`, `Index`, and `Iterate` forms take one; `Assign` takes two and its target returns `void`. Every operator except `Call` and `Iterate` publishes exactly one value, so a `void` or `ReturnPack` result is refused for the rest. Two operators are the exceptions on operand count too: `Call` forwards whatever the call site supplied, and `Iterate` receives one control operand that may be omitted, described under [Iteration](#iteration) below.

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

A `Method`, `StaticMethod`, `Operator`, `Constructor`, `Factory`, or `Singleton` parameter is not limited to the four supported value types either: any type with its own `Luna::TypeConverter<T>` specialization may be an operand too, read through the same probe/read boundary a converted property or field value already uses (see [custom conversions](values-and-validation.md#custom-conversions)). No explicit template argument is needed — the parameter's own declared C++ type is enough, the same way a scalar parameter already is:

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

The operand's class identity comes from what `RegisterClass<T>` recorded for that C++ type, so an operand may be an instance of any registered class, not only of the class declaring the member. That key is recorded once per process, shared by every State in it: registering the same type under a second key later keeps the original identity rather than changing what earlier declarations meant.

A mutable operand — `T &` or `T *` — requires a mutable instance, exactly as a non-const receiver does; a `const` spelling accepts either. Every operand passes the receiver gate before the native target runs, so origin State, metatable identity, payload liveness, borrowed lifetime, dynamic type, and const permission all produce a receiver-quality refusal at the operand's own position. An operand of the wrong class, a scalar, or an omitted operand is a caller error.

One ordering rule applies: a plan is validated entry by entry in staging order, so a class has to be registered before a member that takes one of its instances is declared. Declaring them the other way round is refused with a diagnostic naming the operand position and the unregistered class.

*Registered* means `RegisterClass<T>` has already been staged, not that it has already committed. Both of these satisfy the rule:

- the class was registered by an earlier transaction and is committed, or
- the class was staged earlier in the same pending plan — `RegisterClass` before the declaration that names it, with one `Commit` publishing both.

A class staged *later* in the same plan does not: a forward reference inside one plan is refused, so declare the operand class first when you split a surface across one chain.

A class **taking its own instances** always satisfies the rule, and it is the shape a math type needs most. `RegisterClass<T>` records the class ahead of everything the chain declares for it, so a method, an operator, a constructor, and a factory may all take `T` in the very chain that registers `T`:

```cpp
Luna::ClassBuilder<Vector3> Vectors =
    Studio.RegisterClass<Vector3>("Vector3", Vector3Key());
Vectors.Constructor<double, double, double>()
    .Method("Dot", &Vector3::Dot)            // double(const Vector3 &)
    .Method("Cross", &Vector3::Cross)        // Vector3(const Vector3 &)
    .Operator(Luna::ClassOperator::Add, &Vector3::Plus)
    .Factory("Between", &Vector3::Between);  // Vector3(const Vector3 &, const Vector3 &)
```

```lua
local A, B = Studio.Vector3.New(3, 4, 0), Studio.Vector3.New(1, 0, 0)
print(A:Dot(B), (A + B).X, Studio.Vector3.Between(A, B).X)
```

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

An instance reaches a *variadic* parameter the same way — directly or nested in a table — as an `OwnedValue` of `ValueCategory::Userdata`; see [variadic arguments](registering-functions.md#variadic-arguments).

### Returning instances and tables

A method, static method, or operator returns a registered class instance. The returned class opts in exactly the way an operand class does, with `Luna::RegisteredClassTrait`; a by-value result also has to be move-constructible, as a by-value operand has to be copy-constructible.

| Declared result | Ownership | Declaration |
|---|---|---|
| `T` by value | Lua-owned copy | nothing extra |
| `std::shared_ptr<T>` | shared | nothing extra |
| `T *` | borrowed | `OwnershipPolicy::Borrowed(Lifetime)` |

An operator publishes an instance it owns — `T` by value or `std::shared_ptr<T>`. The borrowed form states its lifetime through the ownership-policy overload, which `Method` and `StaticMethod` accept and `Operator` does not, so an operator does not hand back a borrowed object.

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

An owned return states its reservation before publishing, so a value exceeding the invocation's string policy publishes nothing rather than a truncated prefix.

#### Manufactured instances

An `OwnedValue` also carries an instance the call *built*, which is what a `GetChildren()`-shaped API needs — a table whose elements are objects that did not exist before the call:

| Factory | Ownership | Declaration |
|---|---|---|
| `OwnedValue::Instance<T>(T Value)` | Lua-owned copy | nothing extra |
| `OwnedValue::Instance<T>(std::shared_ptr<T>)` | shared | nothing extra |
| `OwnedValue::Instance<T>(T *, OwnershipPolicy)` | borrowed | `OwnershipPolicy::Borrowed(Lifetime)` |

Each requires `Luna::RegisteredClassTrait<T>` and the class to be registered in the publishing State. The three forms mean exactly what the corresponding direct instance returns mean.

```cpp
[[nodiscard]] Luna::OwnedValue Game::GetChildren() const {
  Luna::OwnedValue Children = Luna::OwnedValue::Table();
  for (const Part &Held : Parts)
    Children.Append(Luna::OwnedValue::Instance<Part>(Held));
  return Children;
}
```

```lua
for _, Child in ipairs(game:GetChildren()) do
  print(Child.Name, Child:Describe())        -- each element is real userdata
end
```

Publication is *deferred*: the owned value records the class key and the produced object, and the userdata is built when the value is materialized onto the stack, through the same publication path a direct instance return uses. Identity caching by native address and the refusal of two objects sharing one address therefore hold unchanged. Nested tables and `ValuePack` elements work identically, at any depth.

An element refuses transactionally and publishes nothing when its class was never registered in this State, when the borrowed form declares no lifetime, or when the object is null. The refusal is decided for every element of the whole return before anything reaches the stack, so a bad element leaves the call published nothing rather than a partial table.

`ReturnPack` gains the matching `AppendInstance<T>` overloads, in the same three forms. A pack that appended only scalars keeps publishing exactly as before; the first `AppendInstance` migrates the scalars already appended and every later element goes through the owned path, so order is preserved:

```cpp
[[nodiscard]] Luna::ReturnPack Wall::Step(std::optional<int> Control) const {
  const int Next = Control ? *Control + 1 : 1;
  if (Next > static_cast<int>(Labels.size()))
    return Luna::ReturnPack::Empty();
  Luna::ReturnPack Produced;
  Produced.AppendInteger(Next);
  Produced.AppendInstance<Badge>(Badge{Labels[Next - 1]});
  return Produced;
}
```

That is what lets the `Iterate` operator yield objects, so a generic for over a container class walks real instances:

```lua
for Position, Badge in Wall do
  print(Position, Badge:Shout())
end
```

An `AppendInstance` element is an integer-keyed value of the pack, so `ReturnPack::Values()` and `At()` — the scalar views — report nothing once a pack carries owned elements; `CarriesOwnedValues()` states which mode a pack is in.

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

HostLog(Hero.Origin.X)                -- instance-valued property, userdata
HostLog(Studio.Sprite.Dimensions)     -- class constant, a plain value

HostLog(Studio.Sprite.Category())     -- static method, no receiver
HostLog(tostring(Studio.Sprite.Square("tile", 2)))  -- factory
```

Every one of those goes through the same validated access gate. A missing receiver, a value from another State, a stale borrowed object, a wrong dynamic type, a const violation, or an unconvertible operand each produce one deterministic refusal before native code runs.

---

[← Previous: Namespaces, constants and enums](namespaces-constants-and-enums.md) · [Documentation index](README.md) · [Next: Modules and versioning →](modules-and-versioning.md)
