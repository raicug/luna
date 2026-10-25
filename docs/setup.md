# Setup

Luna is a static library built with CMake. It needs Luau, which it fetches for you or takes from a copy you already have.

## Requirements

| Requirement | Version |
| --- | --- |
| CMake | 3.20 or newer |
| Compiler | A C++20 compiler. MSVC is the tested one. |
| Git | Needed by the first configure, unless you point at a local Luau tree |
| Network | The first configure fetches Luau `0.730`, unless you point at a local tree |

## Add Luna with FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(
  luna
  GIT_REPOSITORY https://github.com/raicug/luna.git
  GIT_TAG main
)
set(LUNA_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LUNA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LUNA_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LUNA_BUILD_DEMO OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(luna)

target_link_libraries(my_app PRIVATE luna::luna)
```

## Add Luna from a local copy

```cmake
add_subdirectory(external/luna)
target_link_libraries(my_app PRIVATE luna::luna)
```

Link your target to `luna::luna` and nothing else. The Luau include directories reach you through the `luna` target as `SYSTEM` includes, so Luna's own headers can compile, while your code never names a Luau type.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `LUNA_BUILD_TESTING` | `ON` | Builds `luna_tests` and `luna_boundary` and registers them with CTest |
| `LUNA_BUILD_APP` | `ON` | Builds `LunaApp`, the smoke consumer |
| `LUNA_BUILD_BENCH` | `ON` | Builds `LunaBench`, the performance gates |
| `LUNA_BUILD_DEMO` | `ON` on Windows, `OFF` elsewhere | Builds `LunaDemo` and `LunaDemoSelfTest`; needs Dear ImGui |
| `LUNA_LUAU_VERSION` | `0.730` | The Luau tag to fetch |
| `LUNA_LUAU_SOURCE` | empty | An existing Luau source tree to use instead of fetching |
| `LUNA_IMGUI_SOURCE` | empty | An existing Dear ImGui checkout, to build the demo offline |

## Building offline

Point the build at trees you already have:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DLUNA_LUAU_SOURCE="C:/deps/luau" `
  -DLUNA_IMGUI_SOURCE="C:/deps/imgui"
```

`LUNA_LUAU_SOURCE` must contain Luau's own `CMakeLists.txt`. Luna builds it as a subdirectory, with its CLI, tests and web targets switched off, and requires `Luau.VM` and `Luau.Compiler` to be static libraries.

## What you get

| Target | Kind | Purpose |
| --- | --- | --- |
| `luna` / `luna::luna` | static library | The library |
| `luna_tests` | executable | Behavioural tests |
| `luna_boundary` | executable | Compiles every public header on its own |
| `LunaApp` | executable | Smallest complete consumer |
| `LunaBench` | executable | Benchmarks with pass/fail gates |
| `LunaDemo` | Win32 executable | Interactive ImGui host |
| `LunaDemoSelfTest` | executable | The demo's harness with no window, run by CTest |

---

[← Documentation index](README.md) · [Next: Getting started →](getting-started.md)
