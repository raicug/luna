# luna

A small C++20 library for embedding [Luau](https://luau.org). Register native
functions, namespaces, enums and classes with a fluent builder API, then run
scripts against them. Scripts can wait without blocking a thread.

- **Header-light public API.** `#include <luna/luna.hpp>` and link `luna::luna`.
  No Luau type or stack call appears in your code.
- **Staged registration.** Builders append plain data; `Commit()` validates the
  whole batch before anything is linked, so a rejected commit publishes nothing.
- **Coroutine scheduler.** A native `luna::Suspend(seconds)` yields the running
  chunk and the scheduler resumes it later. Many scripts share one thread.
- **Native properties and write hooks.** `PropertyPolicy::Notify` runs after a
  successful write, so no wrapper is needed to observe changes.
- **Safe boundary.** Host exceptions become Luau errors a script can `pcall`, and
  argument errors name the symbol and the parameter position.
- **Interruptible.** `State::RequestInterrupt()` is thread-safe and stops a
  runaway chunk at its next safepoint.
- **Reflection.** `State::Symbols()` and `State::Reflection()` expose what was
  registered, for autocomplete palettes and tooling.

An interactive ImGui demo is included as a separate target that consumes the
library the same way any host does.

## Build

Requires MSVC (C++20) and CMake 3.20+. Luau is fetched automatically; point
`LUNA_LUAU_SOURCE` at a local tree to use it instead.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_TESTING=ON -DLUNA_BUILD_APP=ON -DLUNA_BUILD_BENCH=ON
cmake --build build
ctest --test-dir build
.\build\LunaBench.exe
```

Targets: static library `luna` (alias `luna::luna`), `luna_tests`,
`luna_boundary_tests`, `LunaApp`, `LunaBench`, `LunaDemo` and
`LunaDemoSelfTest`. Options: `LUNA_BUILD_TESTING`, `LUNA_BUILD_APP`,
`LUNA_BUILD_BENCH`, `LUNA_BUILD_DEMO`.

The demo needs Dear ImGui. It is fetched at a pinned revision by default; set
`LUNA_IMGUI_SOURCE` to an existing checkout to build offline:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_IMGUI_SOURCE="C:/path/to/imgui"
```

## Use

```cpp
#include <luna/luna.hpp>

struct Sprite {
  std::string Name;
  bool Visible = true;
  double Transparency = 0.0;
};

// A host type opts in with one line, at namespace scope.
template <> struct luna::detail::RegisteredClassTrait<Sprite> : std::true_type {};

luna::State state;

luna::ClassBuilder<Sprite> Sprites = state.Bindings().RegisterClass<Sprite>(
    "Sprite", luna::StableTypeKey("app.Sprite"));

Sprites.Factory("new", [](std::string Name) { return Sprite{Name}; });
Sprites.Field("Name", &Sprite::Name);
Sprites.Property("Visible", [](const Sprite &Self) { return Self.Visible; },
                 [](Sprite &Self, bool Value) { Self.Visible = Value; });
Sprites.Property("Transparency",
                 [](const Sprite &Self) { return Self.Transparency; },
                 [](Sprite &Self, double Value) { Self.Transparency = Value; },
                 luna::PropertyPolicy::Notify(
                     [](Sprite &Self, std::string_view Name) { /* redraw */ }));
Sprites.Method("Destroy", [](Sprite &Self) { Self.Visible = false; });
Sprites.Commit();

state.Bindings()
    .RegisterNamespace("task")
    .RegisterFunction("wait", [](double Seconds) { luna::Suspend(Seconds); })
    .Commit();

luna::ExecutionResult Result = state.Execute(R"(
  local s = Sprite.new("Square")
  s.Visible = false
  task.wait(0.5)
  assert(s.Visible == false)
)");
```

A native callable that calls `luna::Suspend(Seconds)` yields the running chunk and
resumes after that much virtual time. `State::Tick(dt)` advances the clock from the
host loop; `State::Execute` advances it itself so a self-contained script finishes.
`State::Spawn` starts an independent task and returns at its first suspension.

`for k, v in obj do` works when the class binds `ClassOperator::Iterate`.
`ClassOperator::Index` and `NewIndex` answer keys that are not members, and a
declared property still wins over the fallback.

Tables of runtime-decided shape are built with `OwnedValue`, and multiple return
values with `ValuePack`. Variadic functions take an `ArgumentPack`. Luau functions
passed from a script can be held as `Delegate<Signature>` and called back later.

## Demo

`LunaDemo` is an interactive host: one Luau `State`, one ImGui frame loop, and
panels that read back out of the VM rather than out of library internals.

![luna demo](docs/demo.png)

- **Editor** with sample scripts; `F5` or `Ctrl+Enter` to run.
- **Scene** draws bound `Shape` objects and lets you edit them with sliders. Every
  edit is posted through the VM as a property write, so the write hook fires exactly
  as it does for a script.
- **Scheduler** spawns eight scripts, shows them running as coroutines on one
  thread, and exposes the `Tick(dt)` that advances them.
- **Halting** arms `RequestInterrupt` and reports whether the last run was stopped.
- **Symbols** is the `State::Symbols()` palette, as a tree with a filter.

Two details worth knowing:

1. `Spawn` returns at the first suspension and the host must call `Tick(dt)`;
   `Execute` instead drives its own suspensions to completion.
2. A chunk that never yields cannot be halted from the thread that is blocked
   running it, so the runaway sample arms its interrupt before it runs.

`LunaDemoSelfTest` runs the same harness with no window and no GPU, so the demo's
behaviour is covered by `ctest`.

## Performance

`bench/bench.cpp`, Release `/O2 /Ob2`, median of 5. Each workload runs through the
binding and again as the Luau an author would write by hand for the same effect; the
number shown is the marginal cost (bound minus hand-written). Negative means the
binding is faster.

| Workload | Hand-written | bound | pure Luau | delta | gate |
| --- | --- | ---: | ---: | ---: | ---: |
| property read, 100-member class | `__index` + table read | 65 ns | 58 ns | **7.3 ns** | 45 ns |
| `Vector3` add | table `__add` | 169 ns | 215 ns | **−46 ns** | 60 ns |
| `OwnedValue` table, 8 fields + 2 instances | `build()` with 2 `Sprite.new()` | 1,168 ns | 562 ns | **606 ns** | 1.2 µs |
| `task.wait(0)` yield + resume | — | 264 ns | | **264 ns** | 5 µs |
| register 260 functions + 100 properties | — | 565 µs | | **565 µs** | 25 ms |
| 8 scripts, 1 thread | — | 1 thread | | | 1 + 8 |

A bound property read is a metamethod, so it is compared against a Luau metamethod,
not against a plain table field.

## Design notes

The public headers contain no Luau type, header, or stack operation. One compile
check enforces it: `tests/boundary.cpp` includes each public header alone.
`convert.hpp` is the documented exception: it includes `lua.h`, because inline
codecs and "no Luau in public headers" are mutually exclusive, and the inline codec
is what keeps the hot paths cheap.

Class and member identity are dense `uint32` slots from a monotonic counter.
`Codec<T>` is an inline template specialisation rather than a descriptor plus a
probe, and a rejected `Commit()` is a pure validation failure with nothing to roll
back.

## Not implemented

Documentation and `.d.luau` generation, `Freeze`, `AsyncTask`, a separate `Signal`
type, `Constructor`/`Singleton` (a `Factory` covers both), `ReturnPack`,
`ExecutionPolicy`, annotation macros, and structural type conversion.
