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
    GIT_TAG 09e52ac978017a30ed9938e2c1c369bc68d2ac73
)
FetchContent_MakeAvailable(Luna)

add_executable(MyApp src/main.cpp)
target_compile_features(MyApp PRIVATE cxx_std_20)
target_link_libraries(MyApp PRIVATE Luna::Luna)
```

The commit hash keeps your build repeatable. Change it when you want to update Luna.

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

Build normally with CMake. Do not add Luau include paths or link Luau yourself; `Luna::Luna` handles that.

---

[← Previous: Documentation index](README.md) · [Documentation index](README.md) · [Next: Getting started →](getting-started.md)
