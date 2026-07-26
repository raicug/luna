# Luna documentation

Luna is intentionally small at this stage, but a fair amount happens between registering a C++ function and calling it from Luau. These pages describe the public API first, then work inward toward validation, execution, and the private implementation.

## Reading order

1. [Setup](setup.md) — add Luna with FetchContent or a local copy.
2. [Getting started](getting-started.md) — build Luna and run a first script.
3. [State and lifetime](state-and-lifetime.md) — understand VM ownership and moves.
4. [Registering functions](registering-functions.md) — expose C++ callables safely.
5. [Values and validation](values-and-validation.md) — learn the supported types and conversion rules.
6. [Executing Luau](executing-luau.md) — see how source is compiled and isolated.
7. [Errors and results](errors-and-results.md) — handle registration and execution failures.
8. [Architecture](architecture.md) — follow a call through Luna's private layers.
9. [Testing and contributing](testing-and-contributing.md) — run the checks and follow project conventions.

## What Luna currently does

The public entry point is `<luna/luna.hpp>`. A `Luna::State` owns one Luau VM, `State::Bindings()` provides function registration, and `State::Execute()` compiles and runs source. Consumer code never needs a Luau header, VM pointer, stack operation, or compile definition.

Supported callable values are `bool`, signed 32-bit `int`, `double`, and `std::string`; a callable may also return `void`. Tables, usertypes, containers, overloads, multiple returns, and coroutines are future work rather than partially implemented features.

## Useful source landmarks

- `include/luna/` contains the consumer-facing headers.
- `src/state/` contains all Luau-aware implementation code.
- `app/src/main.cpp` is the smallest complete consumer example.
- `tests/` contains compile checks, examples, integration tests, and properties.

---

[← Previous: Project README](../README.md) · [Documentation index](README.md) · [Next: Setup →](setup.md)
