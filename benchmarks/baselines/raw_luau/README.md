# Raw Luau invocation baseline

This folder measures direct Luau C closures against the same invocation scripts,
operation counts, and benchmark harness used by `LunaBenchmarkInvocation`.

It is built only when both benchmark options are enabled:

```bat
cmake -S . -B build/bench-raw -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUNA_BUILD_BENCHMARKS=ON -DLUNA_BUILD_RAW_LUAU_BASELINES=ON
cmake --build build/bench-raw --target LunaRawLuauBaselinesRun
```

The runner emits `mode=raw-luau` result rows and `comparison` rows pairing raw
Luau with Luna's unfrozen and frozen invocation paths. Comparison rows include
median latency, per-operation delta, and percentage difference. It is a native
invocation baseline only. It does not model Luna registration transactions,
overload selection, conversion policy, reflection, userdata, ownership, or
freeze caches.
