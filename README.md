# Luna

Luna is a C++20 binding library for embedding [Luau](https://luau.org/) without exposing the Luau C API to application code. Its public API owns the VM, registers type-safe native callables, executes source through protected operations, and returns inspectable Luna-owned diagnostics.

Every exposed symbol originates from one canonical reflected descriptor. Functions, overload sets, namespaces, constants, enums, classes, and versioned modules all register through a single atomic transaction, and the committed model can be captured as an immutable reflection snapshot, frozen for read-only invocation, or turned into documentation and Luau `.d.lua` declarations.

## Example

```cpp
#include <luna/luna.hpp>

#include <iostream>

int main() {
  Luna::State State;
  if (!State.IsReady()) {
    std::cerr << "Luna state creation failed\n";
    return 1;
  }

  int ObservedResult = 0;
  const auto Registration = State.Bindings().Register(
      "Add", [&ObservedResult](int Left, int Right) {
        ObservedResult = Left + Right;
        return ObservedResult;
      });

  if (!Registration.IsSuccess()) {
    std::cerr << Registration.Diagnostic()->Message() << '\n';
    return 1;
  }

  const auto Execution = State.Execute("assert(Add(19, 23) == 42)");
  if (!Execution.IsSuccess()) {
    std::cerr << Execution.Diagnostic()->Message() << '\n';
    return 1;
  }

  return ObservedResult == 42 ? 0 : 1;
}
```

Consumers include only Luna headers and link only `Luna::Luna`. Luau headers, VM pointers, stack operations, and link dependencies remain private to the library.

## Documentation

Start with the [setup guide](docs/setup.md) to add Luna through CMake. The [full documentation](docs/README.md) covers State ownership, function registration, value conversion, namespaces and enums, classes and userdata, modules and versioning, reflection and generation, freeze, execution, diagnostics, architecture, and testing.

## AI assistance disclosure

The documentation under `docs/` was written with AI assistance and checked against the current implementation. Some of Luna's code, tests, and refactoring were also produced with AI assistance. The project should still be reviewed and treated like any other source code before it is used in production.

## Features

- Move-only `Luna::State` with deterministic VM ownership and owner-thread affinity
- Functions through `Register` and `RegisterFunction`, with macro-free `Overload<Signature>` disambiguation
- Canonical overload sets resolved by Pareto dominance over conversion ranks
- Trailing `std::optional` parameters, immutable defaults via `WithDefaults`, and variadic `ArgumentView` / `ArgumentPack`
- Zero, scalar, and ordered multiple returns via `std::pair`, `std::tuple`, and `ReturnPack`, published atomically
- Nested namespaces, constants, and enums with aliases, bitflags, and an explicit unscoped opt-in
- Classes as typed userdata: constructors, factories, singletons, allocators, methods, properties, fields, base edges, checked casts, and operators
- Lua-owned, borrowed, and `std::shared_ptr` shared ownership with `LifetimeHandle` invalidation
- Load-once versioned modules with semantic-version constraint resolution
- Content-derived `TypeId` and `SymbolId`: no RTTI name, address, or registration order in any persistent identity
- Owning immutable `ReflectionSnapshot` that outlives its State and reads from any thread
- Deterministic documentation and `.d.lua` generation with atomic artifact publication
- `Freeze()` to validate the model and publish generation-keyed lookup caches
- Transactional registration across every category, with reverse-order undo
- Deterministic argument validation and conversion diagnostics
- Embedded-NUL string support up to 1,048,576 bytes
- Exception translation at the native callback boundary
- Stack restoration after registration, execution, and callback failures
- Luna-owned `RegistrationResult`, `ExecutionResult`, and `ErrorDiagnostic` types
- C++20 public headers with no Luau include-path requirement

## Building

Requirements:

- CMake 3.25 or newer
- Ninja
- A C++20 compiler
- Git and network access during the initial dependency fetch

Debug:

```bat
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

Release:

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
ctest --preset ninja-release
```

Luau and RapidCheck are fetched at pinned revisions through CMake `FetchContent`.

## Interactive editor demo

An optional desktop demo combines Luna with [Dear ImGui](https://github.com/ocornut/imgui), [GLFW](https://www.glfw.org/), and [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit). It is a binding playground: a syntax-highlighted Luau editor with example scripts, host output, a filterable reflection browser, the generated documentation and `.d.lua` artifacts, the exact C++ snippet each feature was bound with, and a log of every registration result. It registers most of Luna's surface through the public API alone, so it doubles as the largest worked example.

```bat
cmake --preset ninja-debug -DLUNA_BUILD_IMGUI_DEMO=ON
cmake --build --preset ninja-debug --target LunaImGuiDemo
.\build\ninja-debug\demo\imgui_color_text_edit\LunaImGuiDemo.exe
```

The demo is deliberately not registered with CTest because it opens a window and waits for user input. Its dependencies are pinned and fetched only when `LUNA_BUILD_IMGUI_DEMO` is enabled.

## Tests

CTest registers four checks:

- `LunaTests` — the unified unit, integration, compile-boundary, and property suite
- `LunaBuildPolicy` — C++20, dependency-boundary, layout, and test-policy checks
- `LunaTestApp.Build` — smoke application build fixture
- `LunaTestApp` — end-to-end State, registration, invocation, and result validation

The unified suite contains 30 RapidCheck properties, each configured for at least 100 successful cases. Both Debug and Release presets are expected to pass all four CTest entries.

Benchmarks are separate and opt-in. `-DLUNA_BUILD_BENCHMARKS=ON` adds one target per measured area under `benchmarks/`, and every result records the build type, compiler, architecture, Luau version, corpus, warmup, sample count, and cache/freeze mode it was measured with. They stay outside the correctness CTest run unless `LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS` is enabled.

## Project layout

```text
include/luna/     Public Luna-owned API
src/state/        Private State, VM, binding, invocation, and execution code
app/src/          Consumer smoke application
demo/             Optional interactive ImGui binding playground
benchmarks/       Opt-in measurement targets, outside the correctness run
tests/unit/       Focused tests
tests/integration/ Tests through the real compiler and virtual machine
tests/property/   Property-based tests
tests/generation/ Generator tests and pinned golden artifacts
tests/compile/    Standalone-header and consumer-boundary checks
cmake/            Dependency configuration
```

All project source paths are lowercase. C/C++ include blocks are protected with clang-format disable/enable markers to preserve deliberate include ordering.

## Current scope

Not implemented yet. These are absent rather than partial:

- Module unload, hot reload, and replacement of a loaded module
- Coroutines and asynchronous invocation
- Delegates, signals, and events
- Annotation helper macros — documentation, attributes, and examples are ordinary builder calls
- IDE and profiling integrations

Two current limitations are worth knowing before designing a surface:

- A registered class cannot be used as a **parameter** type of a `Method` or an `Operator`. It works as a receiver and as a construction result, but an operand or argument is one of the supported value types.
- Inherited **fields** are not reachable through a derived class. Reach them through a value of the class that declared them.
