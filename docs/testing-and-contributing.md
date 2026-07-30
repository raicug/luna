# Testing and contributing

Run the same sequence in both supported configurations before considering a change complete:

```bat
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug

cmake --preset ninja-release
cmake --build --preset ninja-release
ctest --preset ninja-release
```

CTest exposes four entries. `LunaTests` is the one unified test executable; `LunaBuildPolicy` checks project boundaries and configuration; `LunaTestApp.Build` builds the smoke target; and `LunaTestApp` runs the end-to-end consumer flow.

The unified executable contains focused unit tests, compiler/VM integration tests, generation tests with golden artifacts, every standalone public-header compile check, the Luau-free consumer compile checks, and 33 RapidCheck properties. CTest sets `RC_PARAMS=max_success=100 verbose_shrinking=1`, so each property runs at least 100 successful generated cases and prints replay information on failure.

Test sources are grouped by category under `tests/`: `unit/`, `property/`, `integration/`, `generation/`, and `compile/`. Every public header must have a matching standalone compile source under `tests/compile/standalone/`, or configuration fails outright.

## Benchmarks

Benchmarks are opt-in, separate from correctness, and outside a normal CTest run:

```bat
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON
cmake --build build/bench --target LunaBenchmarksRun
```

`-DLUNA_ENABLE_BENCHMARK_REGRESSION_TESTS=ON` adds bounded `LunaBenchmarkRegression.<Scenario>` CTest entries. Every measurement records its build type, compiler, architecture, Luau version, corpus, warmup and sample counts, and cache/freeze mode, and no performance claim is admissible without that line. See [freeze and performance](freeze-and-performance.md) and `benchmarks/README.md`.

## The demo

```bat
cmake --preset ninja-debug -DLUNA_BUILD_IMGUI_DEMO=ON
cmake --build --preset ninja-debug --target LunaImGuiDemo
```

`LunaImGuiDemo` is a binding playground: a script editor, host output, a filterable reflection browser, generated artifacts, the C++ snippet each feature was bound with, and a diagnostics log recording every `RegistrationResult`. It stays outside CTest because it opens a window and waits for input, and the build policy test fails if it is ever registered as a test.

It is also the best place to check a documentation claim, because it uses the public API only and links `Luna::Luna` alone.

## Repository conventions

- Use C++20 with extensions disabled.
- Keep project source paths lowercase.
- Put consumer-facing headers under `include/luna/` and Luau-aware code under `src/`.
- Keep public headers Luau-free: no Luau type, header, pointer, constant, stack operation, or registry reference, and no consumer macro requirement.
- Wrap C/C++ include blocks with `// clang-format off` and `// clang-format on`.
- Add a standalone compile source for every new public header.
- Keep Luau VM and compiler linkage private to `Luna`.
- Keep tests in the single `LunaTests` executable; compile-only checks may use object targets.
- Preserve stack depth, transaction, and result invariants on every failure path.
- Prefer describing a declaration over discovering it: capture the C++ shape where the type is complete, and refuse deterministically rather than guessing.

## Scope and roadmap

Luna currently covers functions with overloads and rich call shapes, namespaces, constants, enums, classes as typed userdata, load-once versioned modules, canonical identity, immutable reflection, documentation and `.d.lua` generation with atomic publication, and freeze.

Module lifecycle is load-only through the public API: no consumer entry point unloads, replaces, or hot reloads a loaded module. The supporting machinery is implemented and tested privately, but no State enables dynamic lifecycle, so such a request is refused deterministically and mutates nothing.

Asynchronous invocation is available for namespace and root functions: a callable may return `Luna::AsyncTask<T>` or `std::future<T>`, which suspends the executing chunk until the host settles the work. Only the chunk `Execute` is running can suspend, and class members and operators refuse asynchronous delivery at registration time.

Delegates and signals are available: `Delegate<Signature>` is an ordinary reflected parameter carrying one subscribed Luau function, and `Signal<Signature>` owns a list of them through `Subscribe`, `Unsubscribe`, and `Emit`, using the same canonical type registry and transaction as everything else.

Generic-for iteration of a class is available through the `Iterate` operator, whose target is one step of the loop rather than an iterator object. Enumerator objects are available through `EnumBuilder::AsObjects()`, which publishes each enumerator as one interned userdata value instead of its bare number; generated Luau declarations still describe such an enumerator by its numeric value.

Not implemented at all, and absent rather than partial:

- annotation helper macros

IDE, autocomplete, debug-UI, and profiling integrations are available: `InstallProfilingHook` reports every invocation stage using the same canonical `SymbolId`/`TypeId` reflection publishes, runs on the owner thread only after Luna has already decided the outcome, and contains and uninstalls a hook that throws.

Two limitations of the shipped surface are worth fixing before new features and are documented where they bite: a registered class cannot be a *parameter* type of a `Method` or `Operator`, and inherited *fields* are not reachable through a derived class.

---

[← Previous: Architecture](architecture.md) · [Documentation index](README.md) · [Next: Project README →](../README.md)
