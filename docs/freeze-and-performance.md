# Freeze and performance

## Freezing a surface

`BindingRegistry::Freeze()` ends the registration phase of a logical State.

```cpp
const Luna::RegistrationResult Frozen = State.Bindings().Freeze();
if (!Frozen.IsSuccess())
  std::cerr << Frozen.Diagnostic()->Message() << '\n';
```

It does three things, in this order:

1. Validates the complete committed model — every symbol, type, overload set, class relationship graph, member, module, and dispatch target.
2. Prepares every deterministic runtime lookup cache in unpublished immutable storage.
3. Publishes those caches together with the frozen lifecycle transition, and only on success.

So a failed freeze changes nothing: the State stays open to registration with exactly the model it had, and the diagnostic names what failed validation.

After a successful freeze:

- **Registration is refused.** Every category, at every scope, with a deterministic diagnostic. Builders created before the freeze fail as stale rather than touching the VM.
- **Invocation keeps working**, now through the published caches.
- **Reflection keeps working.** Snapshots taken before the freeze still observe their own generation; a snapshot taken after observes the frozen one.

The caches are generation-keyed and equivalent to the uncached lookups they replace. That equivalence is the subject of one of Luna's properties, so freezing changes lookup cost, not lookup results.

Freeze is one-way. There is no thaw, and no way to reopen a State for registration. Build a new State if you need a different surface.

### When to freeze

Freeze after the whole surface is registered and before scripts run in earnest — typically once at startup. It buys validation you would otherwise only discover at call time, plus the cache publication. If your host registers lazily in response to user actions, skip it; the open phase is fully functional.

The demo makes the difference observable: it has a Freeze button and a "register one more function" button, so you can watch the second one start being refused.

## Why a removed symbol refuses instead of dangling

Installed closures do not name records. Each one carries only a permanent dispatch slot, and every invocation retains an immutable dispatch generation naming the target of each slot for the duration of that call. A call therefore finishes on the target it entered with, while later calls resolve the current one. That indirection is private implementation, not public API, but it is why an invalidated target refuses deterministically rather than dangling.

## Measuring

Benchmarks are opt-in and deliberately outside the correctness suite.

```bat
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON
cmake --build build/bench --target LunaBenchmarks
cmake --build build/bench --target LunaBenchmarksRun
```

Six scenarios exist: `LunaBenchmarkRegistration`, `LunaBenchmarkOverload`, `LunaBenchmarkConversion`, `LunaBenchmarkInvocation`, `LunaBenchmarkReflection`, and `LunaBenchmarkUserdata`. Each accepts `--warmup=<count>`, `--samples=<count>`, and `--correctness-evidence=<text>`. An explicit `CMAKE_BUILD_TYPE` is required, because a measurement without its build is not evidence.

`-DLUNA_ENABLE_BENCHMARK_REGRESSION_TESTS=ON`, which requires `LUNA_BUILD_BENCHMARKS=ON`, registers one bounded `LunaBenchmarkRegression.<Scenario>` CTest entry per scenario, run with `--warmup=1 --samples=3`. Without that option no benchmark is registered with CTest at all.

### What a result line records

Every measured case prints the scenario, the case, the cache/freeze mode, the corpus, the operation count, the warmup and sample counts, the min, median, mean, max, and median-per-operation timings, and the measured build type, compiler, architecture, and Luau version. No performance claim may be made without that line.

Two other line kinds appear. `blocked` means the corpus could not be prepared in that mode, with Luna's own diagnostic — nothing was measured, so nothing may be claimed. `functional-failure` means the measured body produced an incorrect result; the timing is discarded and the scenario exits non-zero, because a wrong result is never a fast result.

A benchmark result is admissible evidence only while the functional, deterministic-output, stack-safety, recovery, and compatibility suites are unchanged. Benchmarks cannot observe that for themselves, so every run prints `claimable=false` until the runner supplies `--correctness-evidence=<text>` and no case was blocked or failed.

`benchmarks/README.md` is the authority on the harness.

## Not implemented

There are no profiling or IDE integrations. The benchmark harness reports its own measurements and nothing hooks into an external profiler.

---

[← Previous: Reflection and generation](reflection-and-generation.md) · [Documentation index](README.md) · [Next: Executing Luau →](executing-luau.md)
