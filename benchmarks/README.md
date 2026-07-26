# Luna benchmarks

Opt-in measurement targets for registration, overload probing and selection,
conversion, native invocation, reflection lookup, and userdata access. They are
separate executables from the single `LunaTests` correctness executable and the
`LunaTestApp` smoke executable, and they are not part of a normal CTest run.

## Configure and run

```
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON
cmake --build build/bench --target LunaBenchmarks     # build only
cmake --build build/bench --target LunaBenchmarksRun  # build and run every scenario
```

Individual targets are `LunaBenchmarkRegistration`, `LunaBenchmarkOverload`,
`LunaBenchmarkConversion`, `LunaBenchmarkInvocation`, `LunaBenchmarkReflection`,
and `LunaBenchmarkUserdata`. Each accepts `--warmup=<count>`, `--samples=<count>`,
and `--correctness-evidence=<text>`.

An explicit `CMAKE_BUILD_TYPE` is required, because a measurement without its
build is not evidence.

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
