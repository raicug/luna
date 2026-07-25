# Getting started

Luna builds as a C++20 static library. The repository uses CMake presets and Ninja, and fetches pinned Luau and RapidCheck revisions during the first configuration.

## Requirements

You need CMake 3.25 or newer, Ninja, a C++20 compiler, and Git. The first configure also needs network access for `FetchContent`.

```bat
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

Use `ninja-release` in all three commands for a Release build. The library target is `Luna`, with the namespaced alias `Luna::Luna`.

## A complete first program

```cpp
#include <luna/luna.hpp>

int main() {
  Luna::State State;
  if (!State.IsReady())
    return 1;

  const auto Registration =
      State.Bindings().Register("Add", [](int A, int B) { return A + B; });
  if (!Registration.IsSuccess())
    return 1;

  const auto Execution = State.Execute("assert(Add(20, 22) == 42)");
  return Execution.IsSuccess() ? 0 : 1;
}
```

Link the consumer only to `Luna::Luna`. Do not add Luau include directories or link Luau directly; Luna keeps that dependency behind its public boundary.

The standard Luau libraries are opened when a ready State is created, so ordinary helpers such as `assert`, `math`, and `string` are available to executed source.

---

[← Previous: Setup](setup.md) · [Documentation index](README.md) · [Next: State and lifetime →](state-and-lifetime.md)
