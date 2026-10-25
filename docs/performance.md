# Performance

What a call through luna costs, how it is measured, and what makes it cheap.

## Method

`bench/bench.cpp` is built as `LunaBench`. Run it from a Release build, because the numbers only mean something with inlining intact:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\LunaBench.exe
```

Each workload runs twice in the same state: once through a binding, and once as the Luau a script author would write by hand for the same effect. The number that matters is the **marginal cost**, bound minus hand-written. A negative delta means the binding is faster.

Every figure is the median of 5 runs. Each workload has a **gate**, an absolute limit on the delta, and the program reports `N of M gates met`.

## Results

Measured with MSVC `/O2 /Ob2`.

| Workload | Hand-written | Bound | Pure Luau | Delta | Gate |
| --- | --- | ---: | ---: | ---: | ---: |
| Property read, 100-member class | `__index` plus a table read | 57 ns | 49 ns | **8.3 ns** | 45 ns |
| `Vector3` add | a table with `__add` | 155 ns | 198 ns | **−43 ns** | 60 ns |
| `OwnedValue` table, 8 fields and 2 instances | a `build()` that calls `Sprite.new()` twice | 1,222 ns | 540 ns | **683 ns** | 1.2 µs |
| `task.wait(0)`, yield and resume | none | 262 ns | | **262 ns** | 5 µs |
| Register 260 functions and 100 properties | none | 808 µs | | **808 µs** | 25 ms |
| 8 scripts on 1 thread | none | 1 thread | | | 1 + 8 |

A bound property read is a metamethod call, so it is compared against a Luau metamethod, not against a plain table field. The plain-field comparison would measure the wrong thing.

## Why a call is cheap

- **No type erasure in the codec.** A parameter is read by an inline template specialisation, `ReadValue<A>`, compiled into the callable. There is no descriptor to look up and no string handling on the instance-read path.
- **Class ids are resolved once.** A callable stores the id of every class-typed parameter, resolved when it is linked, so the call path never looks a type up.
- **A property read dispatches directly.** The getter is kept in the members table as a bare callable, not a closure, and `__index` invokes its thunk in place after one type check and one pointer read.
- **Dense identity.** A class id is a `uint32` slot from a counter. There are no hashes to compute.
- **A yield is a coroutine switch.** `Suspend` records the request, the call yields, and the scheduler resumes the coroutine. There is no thread, no sleep and no polling.
- **Safepoints cost nothing until asked.** The interrupt hook is not installed until a budget or a `RequestInterrupt` needs it.

## Where time goes

| Cost | Why |
| --- | --- |
| Building a table result | Each field is a `lua_setfield`; each instance is a userdata allocation |
| An overloaded call | The candidate checks run first, and then the winner reads its arguments again |
| A thrown exception | Caught at the boundary and rethrown as a Luau error; keep it for real errors |
| A traceback | Computed once for an error that reaches the host, never for one a script catches |

## Registration cost

Registering is a one-time cost. 360 registrations take about 0.8 ms, dominated by creating the closures and recording what to undo if the commit fails. A batch is validated in one pass over its staged data, with the symbol list indexed by name, so a large API does not slow each registration down.

## Reproducing

`LunaBench` prints `pass` or `FAIL` beside each gate and a summary line, `N of M gates met`. It exits 0 either way, so a CI step has to read the summary. The gates are generous on purpose: they catch a regression of an order of magnitude, not 5%.

---

[← Previous: Reflection and definitions](reflection-and-definitions.md) · [Documentation index](README.md) · [Next: Architecture →](architecture.md)
