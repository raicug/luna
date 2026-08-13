# Luna benchmarks

Opt-in measurement targets for registration, overload probing and selection,
conversion, native invocation, reflection lookup, and userdata access. They are
separate executables from the single `LunaTests` correctness executable and the
`LunaTestApp` smoke executable, and they are not part of a normal CTest run.

## Configure and run

```
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON
cmake --build build/bench --target LunaBenchmarks     # build only
cmake --build build/bench --target LunaBenchmarksRun  # build and run every Luna scenario

cmake -S . -B build/bench-raw -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON -DLUNA_BUILD_RAW_LUAU_BASELINES=ON
cmake --build build/bench-raw --target LunaRawLuauBaselinesRun
```

Individual targets are `LunaBenchmarkRegistration`, `LunaBenchmarkOverload`,
`LunaBenchmarkConversion`, `LunaBenchmarkInvocation`, `LunaBenchmarkReflection`,
and `LunaBenchmarkUserdata`. Each accepts `--warmup=<count>`, `--samples=<count>`,
and `--correctness-evidence=<text>`.

`-DLUNA_BUILD_RAW_LUAU_BASELINES=ON` adds `LunaRawLuauInvocation`,
`LunaRawLuauPureCall`, and `LunaRawLuauBaselinesRun`. `LunaRawLuauInvocation`
runs the same Lua source against three implementations: direct Luau C closures,
Luna before `Freeze()`, and Luna after `Freeze()`. `LunaRawLuauPureCall` is a
separate C++-driven cached-closure microbenchmark. Neither target compares
registration, overload selection, reflection, conversion policy, or userdata,
because raw Luau does not provide equivalent facilities.

## Reading the raw Luau comparison

This is an **end-to-end invocation-script benchmark**, not a bare C-closure
microbenchmark. The timer starts immediately before each model's `Run` call and
stops after it returns. A raw-Luau run compiles the source, creates and pins a
disposable coroutine, loads bytecode, resumes it, and executes the native calls.
A Luna run executes the same source through `State::Execute`. Registration and
freezing happen while preparing the model, before the timer begins.

The corpus contains 100,000 native calls per timed sample. That count is large
enough to amortize per-run setup, so the table is useful for comparing the
steady-state cost of this exact script. It is not comparable with the retired
250-call sample: changing the operation count changes how much compilation and
coroutine setup contributes to `ns/op`.

Raw Luau is a lower-bound reference for these three narrow, direct-native-call
scripts. Luna deliberately performs work the raw closures do not: dispatch and
lifetime handling, validation, fault and profiling integration, type-erased
callable support, and return-shape handling. The raw columns do **not** measure
or rank Luna's broader feature set.

### Current sample

Recorded by `LunaRawLuauInvocation` on Windows/AMD64 with Clang 22.1.5, Luau
0.730, Release mode, `--warmup=3`, and `--samples=20`. The runner reported
`claimable=false`, so these are diagnostic numbers rather than a performance
claim.

| Case | Raw Luau ns/op | Luna unfrozen ns/op | Luna frozen ns/op | Freeze saves | Frozen / raw |
|---|---:|---:|---:|---:|---:|
| VoidCall | 18 | 269 | 119 | 150 ns/op (55.8%) | 6.61x |
| ScalarCall | 29 | 461 | 152 | 309 ns/op (67.0%) | 5.24x |
| DynamicPackCall | 41 | 701 | 208 | 493 ns/op (70.3%) | 5.07x |

`Freeze saves` compares Luna frozen with Luna unfrozen within the same row; it
is the useful column for judging the effect of Luna's freeze optimization.
`Frozen / raw` is a scenario-specific ratio, not a claim that Luna implements
the same semantics as the raw closure.

### Pure C++ cached-closure calls

`LunaRawLuauPureCall` measures a different question: one C++ loop invokes one
already-bound closure 100,000 times. Before timing begins, each model creates
its state, registers functions, freezes the frozen model, resolves each global
closure once, and stores it in the Luau registry. The timed loop only restores
the cached closure, pushes prebuilt arguments, invokes it with `lua_pcall`, and
pops its declared returns. It creates no script, bytecode, coroutine, or
binding during a measured sample.

The Luna arm calls the installed Luna closure, not an erased callable or native
function pointer. Consequently it includes Luna's normal Lua-facing dispatch:
unfrozen calls use the generic trampoline and eligible frozen calls use the
frozen trampoline. The raw arm uses the same cached-closure/`lua_pcall` shape
for direct Luau C closures. These results are not comparable with the
end-to-end script table above.

Recorded on Windows/AMD64 with Clang 22.1.5, Luau 0.730, Release mode,
`--warmup=3`, and `--samples=20`; `claimable=false`.

| Case | Raw Luau ns/op | Luna unfrozen ns/op | Luna frozen ns/op | Freeze saves | Frozen / raw |
|---|---:|---:|---:|---:|---:|
| PureVoidCall | 37 | 276 | 99 | 177 ns/op (64.1%) | 2.68x |
| PureScalarCall | 70 | 463 | 167 | 296 ns/op (63.9%) | 2.39x |
| PureDynamicPackCall | 44 | 867 | 237 | 630 ns/op (72.7%) | 5.39x |

### Comparing future runs

Only compare results when all of these match: source corpus, operation count,
warmup/sample counts, compiler, build type, Luau version, architecture, and
CPU environment. Compare frozen `ns/op` with an earlier frozen `ns/op`, or
unfrozen with unfrozen. Do not compare percentage gaps from different corpus
sizes; the denominator changes when fixed setup is amortized differently.

Regenerate this table from one successful `luna-benchmark result` set whenever
the benchmark configuration, corpus, Luau version, compiler, or implementation
changes. Retain the raw result rows with the table so the values remain
auditable.

## Bounded regression checks

`-DLUNA_ENABLE_BENCHMARK_REGRESSION_TESTS=ON` (which requires
`LUNA_BUILD_BENCHMARKS=ON`) registers one bounded `LunaBenchmarkRegression.<Scenario>`
CTest entry per scenario, run with `--warmup=1 --samples=3`. Without that option
no benchmark is registered with CTest at all.

## What every result records

Each measured case prints one line carrying the scenario, case, cache/freeze
mode, corpus, operation count, warmup count, sample count, the min, median,
mean, max, and median-per-operation timings, and the measured build type,
compiler, architecture, and Luau version. No performance claim may be made
without that line.

Two other line kinds appear:

- `blocked` - the corpus could not be prepared in that mode, with Luna's own
  diagnostic. Nothing was measured, so nothing may be claimed about it.
- `functional-failure` - the measured body produced an incorrect result. The
  timing is discarded and the scenario exits non-zero, because a wrong result is
  never a fast result.

## Accepting an optimization

A benchmark result is admissible evidence only while the functional,
deterministic-output, stack-safety, recovery, and compatibility suites are
unchanged. Benchmarks cannot observe that for themselves, so every run prints
`claimable=false` until the runner supplies `--correctness-evidence=<text>` and
no case was blocked or failed.
