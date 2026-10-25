# Luna documentation

Luna is a small C++20 library for embedding [Luau](https://luau.org). You describe C++ functions, namespaces, enums and classes with a fluent builder API, and scripts call them. Scripts can also wait, without parking a thread.

These pages start with the public API and work inward, toward validation, the scheduler and the private implementation.

## Reading order

| # | Page | What it covers |
| ---: | --- | --- |
| 1 | [Setup](setup.md) | Add Luna to a CMake project, build options, requirements |
| 2 | [Getting started](getting-started.md) | Build the repository and run a first and a second program |
| 3 | [State and lifetime](state-and-lifetime.md) | `State`, `StateOptions`, moves, threads, teardown |
| 4 | [Registering functions](registering-functions.md) | Callables, parameters, returns, optionals, overloads |
| 5 | [Values and conversion](values-and-conversion.md) | Supported types, containers, `OwnedValue`, `ValuePack`, numbers |
| 6 | [Namespaces, constants and enums](namespaces-constants-and-enums.md) | Hierarchical registration |
| 7 | [Classes and userdata](classes-and-userdata.md) | Members, operators, ownership, inheritance, locking |
| 8 | [Tasks and scheduling](tasks-and-scheduling.md) | `Execute`, `Spawn`, `Tick`, `Suspend`, the task library, chunks |
| 9 | [Signals and delegates](signals-and-delegates.md) | Events, handlers, `Wait` |
| 10 | [Safety and limits](safety-and-limits.md) | Interrupts, budgets, memory, sandboxing, locked bindings |
| 11 | [Errors and results](errors-and-results.md) | Every failure the API can report |
| 12 | [Reflection and definitions](reflection-and-definitions.md) | Symbols, `.d.luau` generation |
| 13 | [Performance](performance.md) | What a call costs, and the benchmark gates |
| 14 | [Architecture](architecture.md) | Follow a call and a commit through the private layers |
| 15 | [Testing and contributing](testing-and-contributing.md) | Run the checks, format the code, project conventions |

## What Luna does

The public entry point is `<luna/luna.hpp>`. A `luna::State` owns one Luau VM. `State::Bindings()` returns the `BindingRegistry` every declaration goes through, `State::Execute()` compiles and runs source, and `State::Load()` compiles source into a reusable `luna::Chunk`.

```cpp
#include <luna/luna.hpp>

luna::State state;
state.Bindings().Register("Add", [](double a, double b) { return a + b; });
state.Execute("assert(Add(20, 22) == 42)");
```

Supported today:

- **Functions.** Free functions, function pointers, lambdas, functors and member-function pointers. Trailing `std::optional` parameters, a variadic `ArgumentPack`, and overload sets chosen by a strict, non-mutating check. `std::pair` and `std::tuple` return several values.
- **Containers.** `std::vector`, `std::map`, `std::unordered_map`, `std::pair`, `std::tuple` and `std::optional` convert in both directions, nested as deeply as you like.
- **Namespaces, constants and enums.** Nested namespaces, constants, and enums with aliases. `AsObjects()` publishes each enumerator as an interned object with `Name`, `Value` and `EnumName`.
- **Classes.** Factories, methods, static methods, properties (with write hooks), fields, operators including generic-`for` iteration, class constants, single inheritance with `IsA`, and three ownership models.
- **A coroutine scheduler.** A native `luna::Suspend(seconds)` yields the running script, including a coroutine the script made itself. `Spawn`, `Tick` and `Cancel` drive tasks from the host loop, a `Delegate` can start its function as a task, and `RegisterTaskLibrary` gives scripts `task.spawn`, `defer`, `delay`, `cancel` and `wait`.
- **Signals and delegates.** A `Delegate` holds one script function. A `Signal` owns a list of handlers, fires them with isolation, and lets a script `:Wait()` for the next fire. A `ScriptSignal` is the dynamic version scripts make and use freely, with connection objects, `Once` and handlers that run as tasks.
- **Limits.** `RequestInterrupt` from any thread, an optional safepoint budget, a memory ceiling, `Sandbox()`, and bindings a script cannot rewrite.
- **Reflection.** `Symbols()`, immutable `ReflectionSnapshot`s, and generated `.d.luau` definitions.
- **Transactional registration.** `Commit()` validates the whole batch first, then links under a protected call. A failure after linking began is rolled back, so a rejected commit publishes nothing.

## Boundaries and limits

- **No Luau types in your code.** Nothing in `<luna/luna.hpp>` names a Luau type, and `tests/boundary.cpp` checks that each public header compiles on its own. `convert.hpp` includes `lua.h` because inline codecs need it; the include path for Luau is an interface dependency of the `luna` target.
- **One State, one thread.** A `State` is not thread-safe. Only `RequestInterrupt`, `ClearInterrupt` and `IsInterruptPending` may be called from another thread, and reflection snapshots can be read anywhere.
- **Numbers are doubles.** Luau has no 64-bit integer. An integer beyond 2^53 loses precision on the way in and out, and a 64-bit integer parameter refuses a value outside its range.
- **Not implemented.** Documentation and attribute macros, `Freeze`, `AsyncTask`, `Constructor`/`Singleton` (a `Factory` covers both), `ReturnPack`, `ExecutionPolicy`, and structural type conversion. See [Architecture](architecture.md#what-was-left-out-on-purpose) for why.

## Source landmarks

| Path | Contents |
| --- | --- |
| `include/luna/` | The public headers. `luna.hpp` includes all of them. |
| `src/` | Everything Luau-aware: `state.cpp`, `registry.cpp`, `model.cpp`, `userdata.cpp`, `convert.cpp`, `docs.cpp`. Not public API. |
| `app/src/main.cpp` | The smallest complete consumer. |
| `demo/` | An interactive ImGui host, plus a headless self-test. |
| `bench/bench.cpp` | The performance gates. |
| `tests/` | `tests.cpp` (behaviour) and `boundary.cpp` (header hygiene). |

---

[← Project README](../README.md) · [Next: Setup →](setup.md)
