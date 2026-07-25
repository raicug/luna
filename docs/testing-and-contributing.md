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

The unified executable contains focused unit tests, compiler/VM integration tests, every standalone public-header compile check, the Luau-free consumer compile check, and 17 RapidCheck properties. CTest sets `RC_PARAMS=max_success=100`, so each property runs at least 100 successful generated cases and prints replay information on failure.

## Repository conventions

- Use C++20 with extensions disabled.
- Keep project source paths lowercase.
- Put consumer-facing headers under `include/luna/` and Luau-aware code under `src/`.
- Wrap C/C++ include blocks with `// clang-format off` and `// clang-format on`.
- Add a standalone compile source for every new public header.
- Keep Luau VM and compiler linkage private to `Luna`.
- Keep tests in the single `LunaTests` executable; compile-only checks may use object targets.
- Preserve stack depth and result invariants on every failure path.

## Scope and roadmap

Luna currently stops at callable registration and protected source execution. The next milestone adds typed global values, table references, nested table access, and module tables. Usertype/class binding comes after the table and reference lifetime model is stable. Later work may add overloads, optional values, containers, multiple returns, custom converters, sandboxes, coroutines, and hot reload.

---

[← Previous: Architecture](architecture.md) · [Documentation index](README.md) · [Next: Project README →](../README.md)
