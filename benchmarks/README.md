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

`-DLUNA_BUILD_RAW_LUAU_BASELINES=ON` adds `LunaRawLuauInvocation` and
`LunaRawLuauBaselinesRun`. It measures the same three invocation scripts as
`LunaBenchmarkInvocation` through direct Luau C closures, then emits comparison
rows pairing each `raw-luau` result with Luna's `unfrozen-uncached` and
`frozen-cached` result. A comparison carries the median latency, per-operation
delta, and percentage difference. It intentionally does not compare
registration, overload selection, reflection, conversion policy, or userdata,
because raw Luau does not provide equivalent facilities.

## Raw Luau comparison sample

The following bounded sample was recorded by `LunaRawLuauInvocation` on
Windows/AMD64 with Clang 22.1.5, Luau 0.730, Release mode, `--warmup=1`, and
`--samples=3`. It is a convenient display of every current raw-Luau comparison,
not an admissible performance claim: the runner reported `claimable=false`.

| Case | Raw Luau median ns/op | Luna unfrozen ns/op | Delta | Difference | Luna frozen ns/op | Delta | Difference |
|---|---:|---:|---:|---:|---:|---:|---:|
| VoidCall | 54 | 521 | +467 | +864.82% | 514 | +460 | +851.85% |
| ScalarCall | 86 | 714 | +628 | +730.23% | 771 | +685 | +796.51% |
| DynamicPackCall | 93 | 1172 | +1079 | +1160.22% | 1166 | +1073 | +1153.76% |

Regenerate this table from the `luna-benchmark comparison` records whenever the
benchmark configuration, corpus, Luau version, compiler, or implementation
changes.

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
