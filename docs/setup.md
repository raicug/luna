# Setup

Use CMake `FetchContent` unless you specifically need to edit Luna locally.

## Option 1: FetchContent (recommended)

Put this in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApp LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    Luna
    GIT_REPOSITORY https://github.com/raicug/luna.git
    GIT_TAG c17e22c7198fc18eb6abdd36d5e0dfc48a1c1c32
)
FetchContent_MakeAvailable(Luna)

add_executable(MyApp src/main.cpp)
target_compile_features(MyApp PRIVATE cxx_std_20)
target_link_libraries(MyApp PRIVATE Luna::Luna)
```

The commit hash keeps your build repeatable. Pin the commit that matches the documentation you are reading; this one is refreshed whenever these pages are.

## Option 2: Local copy

Place Luna somewhere inside your project, then replace the `FetchContent` block with:

```cmake
add_subdirectory(external/luna)
```

Keep the same `target_link_libraries(MyApp PRIVATE Luna::Luna)` line.

## Use Luna

```cpp
#include <luna/luna.hpp>

int main() {
  Luna::State State;
  return State.IsReady() ? 0 : 1;
}
```

`<luna/luna.hpp>` is the single entry point: every consumer-facing type is reachable from it. Build normally with CMake. Do not add Luau include paths or link Luau yourself; `Luna::Luna` keeps the VM and compiler as private, link-only dependencies.

Set `BUILD_TESTING=OFF` if you want the library alone. Luna calls `include(CTest)`, so with testing left on it also fetches RapidCheck and configures its own test targets. The small `LunaTestApp` smoke executable is always added.

## Options you may want

Luna's own build exposes three options. None of them is needed to consume the library.

| Option | Default | Effect |
|---|---|---|
| `LUNA_BUILD_IMGUI_DEMO` | `OFF` | Builds the `LunaImGuiDemo` binding playground. Stays outside CTest. |
| `LUNA_BUILD_BENCHMARKS` | `OFF` | Builds the measurement targets under `benchmarks/`. |
| `LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS` | `OFF` | Registers bounded benchmark CTest entries. Requires `LUNA_BUILD_BENCHMARKS`. |

---

[← Previous: Documentation index](README.md) · [Documentation index](README.md) · [Next: Getting started →](getting-started.md)
