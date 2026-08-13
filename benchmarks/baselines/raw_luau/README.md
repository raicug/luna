# Raw Luau invocation baseline

This folder measures direct Luau C closures against the same invocation scripts,
operation counts, and benchmark harness used by `LunaBenchmarkInvocation`.

It is built only when both benchmark options are enabled:

```bat
cmake -S . -B build/bench-raw -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON -DLUNA_BUILD_RAW_LUAU_BASELINES=ON
cmake --build build/bench-raw --target LunaRawLuauBaselinesRun
```

The runner emits `mode=raw-luau` result rows and comparison rows pairing raw
Luau with Luna's unfrozen and frozen invocation paths. The timed region covers
a complete script run rather than only a C closure: raw Luau compiles, loads,
and resumes the script on a disposable coroutine before executing its native
calls. Registration and freeze preparation occur before timing.

Raw Luau is therefore a lower-bound reference for the three direct-native-call
scripts, not a feature-equivalence claim. It does not model Luna registration
transactions, overload selection, conversion policy, reflection, userdata,
ownership, lifetime handling, diagnostics, fault hooks, profiling, or freeze
cache construction. Compare only runs with the same corpus and operation count;
changing those changes how setup cost is amortized into `ns/op`.
