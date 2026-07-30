# Luna documentation

Luna binds C++ to Luau without letting a single Luau type reach consumer code. A fair amount happens between describing a C++ declaration and calling it from a script: canonical identity, one registration transaction, overload resolution, conversion, dispatch, reflection. These pages describe the public API first, then work inward toward validation, execution, and the private implementation.

## Reading order

1. [Setup](setup.md) — add Luna with FetchContent or a local copy.
2. [Getting started](getting-started.md) — build Luna and run a first script.
3. [State and lifetime](state-and-lifetime.md) — VM ownership, moves, thread affinity, lifecycle.
4. [Registering functions](registering-functions.md) — callables, overloads, parameters, returns.
5. [Values and validation](values-and-validation.md) — supported types, canonical types, conversion.
6. [Namespaces, constants and enums](namespaces-constants-and-enums.md) — hierarchical registration.
7. [Classes and userdata](classes-and-userdata.md) — typed objects, members, ownership.
8. [Modules and versioning](modules-and-versioning.md) — manifests, resolution, load-once.
9. [Reflection and generation](reflection-and-generation.md) — snapshots, documentation, `.d.lua`.
10. [Freeze and performance](freeze-and-performance.md) — sealing a surface and measuring it.
11. [Executing Luau](executing-luau.md) — see how source is compiled and isolated.
12. [Errors and results](errors-and-results.md) — handle registration and execution failures.
13. [Architecture](architecture.md) — follow a call through Luna's private layers.
14. [Testing and contributing](testing-and-contributing.md) — run the checks and follow project conventions.

## What Luna currently does

The public entry point is `<luna/luna.hpp>`. A `Luna::State` owns one Luau VM, `State::Bindings()` returns the `BindingRegistry` every declaration goes through, and `State::Execute()` compiles and runs source. Consumer code never needs a Luau header, VM pointer, stack operation, or compile definition.

Supported today:

- **Functions.** `Register` and `RegisterFunction` accept free functions, function pointers, lambdas, functors, static methods, and explicit member wrappers. `Overload<Signature>` selects one C++ target by its declared signature, with no macro, and several declarations under one name form a canonical overload set resolved by Pareto dominance over conversion ranks.
- **Rich call shapes.** Trailing `std::optional` parameters, immutable defaults through `WithDefaults`, and one final variadic parameter as `ArgumentView` (callback lifetime) or `ArgumentPack` (owning).
- **Multiple returns.** `void` publishes zero values, a supported scalar publishes one, and `std::pair`, `std::tuple`, and `ReturnPack` publish ordered multiple values, atomically.
- **Hierarchy.** `RegisterNamespace` with nested `NamespaceBuilder`, `RegisterConstant`, and `RegisterEnum` with enumerators, aliases, bitflags, and an explicit unscoped opt-in. `AsObjects()` publishes each enumerator as one interned enumerator object carrying `Name`, `Value`, and `EnumName`, reporting `typeof` as `EnumItem` and comparing equal only to itself.
- **Classes.** `RegisterClass<T>` with constructors, factories, singletons, allocators, methods, static methods, properties, fields, class-scope constants, base edges, checked casts, and operators — including `Iterate`, which makes a class usable in a Luau generic `for` loop through one declared step. Objects are typed userdata, owned by Lua, borrowed behind a `LifetimeHandle`, or shared through `std::shared_ptr`. A read-write property or a writable field may also declare an optional on-change callback, run after its write already succeeded, and its value type may be any type with its own `TypeConverter<T>` rather than only a foundation scalar.
- **Modules.** Load-once versioned modules with semantic versions, constraint resolution that picks the highest satisfying version, and canonical cycle and conflict diagnostics.
- **Delegates and signals.** `Delegate<Signature>` is an ordinary reflected parameter type carrying one subscribed Luau function, held through Luna's own reference mechanism. `Signal<Signature>` owns a list of them and provides `Subscribe`, `Unsubscribe`, and `Emit`; it uses the same canonical type registry as everything else rather than a parallel callback system.
- **Profiling and debug-UI hooks.** `InstallProfilingHook` reports every invocation stage with the canonical identity reflection already publishes, without a second metadata schema or a change to invocation semantics.
- **Dispatch indirection.** Native closures resolve a stable dispatch slot rather than holding a binding record, and an invocation retains the generation it began with, so publishing a new generation never retargets work already in flight.
- **Canonical identity.** `TypeId`, `SymbolId`, `StableTypeKey`, and canonical descriptors. No RTTI name, address, or registration order participates in any persistent identity.
- **Reflection.** `ReflectionSnapshot` captures one immutable committed generation that stays readable after later registration, freeze, a State move, destruction of the originating State, and from another thread.
- **Generation.** `GenerateDocumentation` and `GenerateDeclarations` read a snapshot and nothing else, and `PublishArtifact`, `PublishDocumentation`, and `PublishDeclarations` replace a file atomically. Output is canonical UTF-8, no BOM, LF endings, byte-identical for equivalent content.
- **Freeze.** `BindingRegistry::Freeze()` validates the whole committed model, publishes generation-keyed lookup caches, then refuses further registration while invocation and reflection keep working.
- **Transactions.** Every category registers through one outermost transaction with reverse-order undo and atomic publication: a refused attempt publishes nothing.

Module lifecycle is load-only through the public API. Registration is additive, and no consumer entry point unloads, replaces, or hot reloads a loaded module. The supporting machinery exists — affected-closure and blocker analysis, staging with reverse-order undo, atomic generation publication, and retained dispatch generations — but no State enables dynamic lifecycle, so such a request is refused deterministically with a load-only diagnostic and changes nothing.

Asynchronous invocation is available for namespace and root functions. A callable that returns `Luna::AsyncTask<T>` or `std::future<T>` suspends the chunk `Execute` is running, and the call resumes with the awaited value once the host settles the work. See [Registering functions](registering-functions.md#asynchronous-results).

Not implemented at all. These are absent, not partial:

- Annotation helper macros. Documentation, attributes, and examples are declared through ordinary builder calls.

None of these degrade quietly. An unsupported callable, parameter, or return fails the public `SupportedCallable`, `SupportedParameter`, and `SupportedReturn` constraints at compile time, and an unsupported value is refused at registration time with a deterministic diagnostic, no published descriptor, and no VM artifact.

IDE, autocomplete, debug-UI, and profiling integrations are available, and deliberately build from nothing but the public model: reflection snapshots, generated artifacts, and canonical identity. `BindingRegistry::InstallProfilingHook(Luna::ProfilingHook)` installs one hook reporting every call's completion, failure, suspension, resumption, or cancellation with the same `SymbolId`/`TypeId` reflection publishes. It runs on the owner thread strictly after Luna has decided the outcome, so it changes nothing about invocation, and a throwing hook is contained and uninstalled rather than reaching Luau.

A few current limitations are worth knowing before you design a surface:

- A parameter of any registered callable — root, namespaced, or a `Method`, `StaticMethod`, `Operator`, `Constructor`, `Factory`, or `Singleton` — may be any type with its own `Luna::TypeConverter<T>` specialization, read through the same probe/read boundary a converted property or field value already uses.
- A **registered class** is also an operand type, spelled `T`, `const T &`, `T &`, `T *`, or `const T *`, once the class opts in with `Luna::RegisteredClassTrait`. The class must be *staged* before a member taking one of its instances is declared — committed by an earlier transaction, or registered earlier in the same pending plan — and a class taking its own instances always qualifies.
- Publishing a **converted** return value is not yet supported. A method, static method, or operator does return a class instance — Lua-owned by value or shared through `std::shared_ptr<T>`, and for a method or static method also borrowed through `T *` with a declared lifetime — as well as one `OwnedValue` or a `ValuePack` of those, which is how a table crosses the boundary as a result.
- `ReturnPack` stays scalar-only by design; a multiple return carrying a table or an object uses `ValuePack`.
- A variadic `ArgumentView`/`ArgumentPack` element may be a registered class instance, passed directly or nested inside a table, carried as `OwnedValue::Kind() == ValueCategory::Userdata` and carrying whatever its class's `ToText` operator rendered.
- A **property or field value** may be a registered class instance — `T`, `std::shared_ptr<T>`, or a borrowed `T *` with a declared lifetime — inferred from the accessor's own return type. Such a member is read-only: a setter, or a writable instance field, is refused at registration.
- A **class constant** (`ClassBuilder::Constant`) carries one of the canonical constant types, so it cannot yet be a class instance; a zero-argument factory or a static method publishes an instance-valued leaf until it can.
- Inherited **fields** are not reachable through a derived class. Reach them through a value of the class that declared them.

## Useful source landmarks

- `include/luna/` contains the consumer-facing headers. They are Luau-free by construction and checked as such.
- `src/state/` contains all Luau-aware implementation code and is not public API.
- `demo/imgui_color_text_edit/src/main.cpp` is the largest worked example: it exercises most of the surface through the public API alone.
- `app/src/main.cpp` is the smallest complete consumer example.
- `tests/` contains unit tests, integration tests, compile checks, generation golden files, and 33 properties.

---

[← Previous: Project README](../README.md) · [Documentation index](README.md) · [Next: Setup →](setup.md)
