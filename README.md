# Luna

Luna is a C++20 binding library for embedding [Luau](https://luau.org/) without exposing the Luau C API to application code. Its public API owns the VM, registers type-safe native callables, executes source through protected operations, and returns inspectable Luna-owned diagnostics.

The current foundation supports global C++ functions, function pointers, and concrete lambdas using `bool`, `int`, `double`, and `std::string` parameters. Callables may return one of those values or `void`.

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

Start with the [setup guide](docs/setup.md) to add Luna through CMake. The [full documentation](docs/README.md) covers State ownership, function registration, value conversion, execution, diagnostics, architecture, and testing.

## AI assistance disclosure

The documentation under `docs/` was written with AI assistance and checked against the current implementation. Some of Luna's code, tests, and refactoring were also produced with AI assistance. The project should still be reviewed and treated like any other source code before it is used in production.

## Features

- Move-only `Luna::State` with deterministic VM ownership
- Type-safe registration through `Luna::BindingRegistry`
- Automatic callable metadata deduction
- Protected Luau compilation and execution
- Transactional registration with duplicate and rollback protection
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

An optional desktop demo combines Luna with [Dear ImGui](https://github.com/ocornut/imgui), [GLFW](https://www.glfw.org/), and [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit). It provides a small Luau editor, a Run button, execution diagnostics, and output from registered C++ callbacks.

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

The unified suite contains 17 RapidCheck properties, each configured for at least 100 successful cases. Both Debug and Release presets are expected to pass all four CTest entries.

## Project layout

```text
include/luna/   Public Luna-owned API
src/state/      Private State, VM, binding, invocation, and execution code
app/src/        Consumer smoke application
demo/           Optional interactive ImGui playground
tests/unit/     Focused and integration tests
tests/property/ Property-based tests
tests/compile/  Standalone-header and consumer-boundary checks
cmake/          Dependency configuration
```

All project source paths are lowercase. C/C++ include blocks are protected with clang-format disable/enable markers to preserve deliberate include ordering.

## Current scope

This foundation intentionally does not yet provide tables, global value proxies, userdata/classes, methods, overload sets, optional or variadic arguments, containers, multiple returns, modules, custom converters, coroutines, hot reload, or asynchronous bindings.

The next planned milestone is global values and tables: typed `State::Set`/`Get`, `State::operator[]`, registry-backed table references, nested table access, and module tables. Usertype/class binding follows after that foundation is stable.
