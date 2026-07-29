# Executing Luau

`State::Execute(std::string_view)` compiles and runs one contiguous source buffer. It returns an `ExecutionResult`; it does not return values produced by the script.

```cpp
const Luna::ExecutionResult Result = State.Execute(R"(
  local Answer = Add(19, 23)
  assert(Answer == 42)
)");
```

A non-ready State rejects execution before compilation or stack work begins, and execution is confined to the owner thread of the logical State.

## What happens during Execute

Luna copies the supplied source, compiles it with the pinned Luau compiler, and creates a fresh Luau thread sharing the State's global environment. The bytecode is loaded as chunk `=Luna` and run on that thread under protected execution.

A bound callable that delivers its value later suspends that thread instead of finishing inside the call. Luna then advances the suspended work on the owner thread and resumes the same thread with the awaited value, so `Execute` still returns only after the chunk finished, failed, or was cancelled. Nothing else runs on the owner thread while work is outstanding, and no suspended call outlives the execution that started it.

A script may also subscribe a Luau function to a `Luna::Signal<Signature>` the host exposes. The subscribed function is held through Luna's own reference mechanism for as long as it stays subscribed, and `Emit` calls it like any other protected callback: a failing handler is reported and recovered without failing the emission that called it. See [Registering functions](registering-functions.md#delegates-and-signals).

The thread is temporary. Whether execution succeeds, fails to compile, raises a Luau error, or reaches a native callback failure, Luna resets and unreferences it before returning. Registered globals, namespaces, enums, classes, and module surfaces live on the root State and remain available to later executions.

This disposable-thread design keeps failed chunks from leaving execution values behind. A root stack checkpoint also restores the exact depth observed when execution began.

## Recovery

Compilation and runtime failures do not poison a ready State. You can inspect the failure and immediately execute another chunk:

```cpp
const Luna::ExecutionResult Bad = State.Execute("local =");
const Luna::ExecutionResult Good = State.Execute("assert(Add(1, 2) == 3)");
```

Previously committed bindings remain installed after failure. Native validation errors, receiver refusals, overload resolution failures, and translated C++ exceptions all follow the same protected path.

## Current limitations

These are intentional gaps rather than partial features:

- No file-loading helper, custom chunk name, or sandbox environment.
- No mechanism for retrieving script return values.
- Only the chunk `Execute` is running can suspend an asynchronous call. A script coroutine or a metamethod that reaches such a callable is refused deterministically, and the started work is cancelled.
- No annotation helper macros or IDE/profiling integrations.

Execution is otherwise unrestricted: a chunk may freely call any registered function, construct registered classes, read constants and enumerators, and use module surfaces.

---

[← Previous: Freeze and performance](freeze-and-performance.md) · [Documentation index](README.md) · [Next: Errors and results →](errors-and-results.md)
