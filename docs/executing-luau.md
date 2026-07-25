# Executing Luau

`State::Execute(std::string_view)` compiles and runs one contiguous source buffer. It returns an `ExecutionResult`; it does not return values produced by the script.

```cpp
const auto Result = State.Execute(R"(
  local Answer = Add(19, 23)
  assert(Answer == 42)
)");
```

A non-ready State rejects execution before compilation or stack work begins.

## What happens during Execute

Luna copies the supplied source, compiles it with the pinned Luau compiler, and creates a fresh Luau thread sharing the State's global environment. The bytecode is loaded as chunk `=Luna` and run with a protected call requesting zero chunk results.

The thread is temporary. Whether execution succeeds, fails to compile, raises a Luau error, or reaches a native callback failure, Luna resets and unreferences it before returning. Registered globals live on the root State and remain available to later executions.

This disposable-thread design keeps failed chunks from leaving execution values behind. A root stack checkpoint also restores the exact depth observed when execution began.

## Recovery

Compilation and runtime failures do not poison a ready State. You can inspect the failure and immediately execute another chunk:

```cpp
auto Bad = State.Execute("local =");
auto Good = State.Execute("assert(Add(1, 2) == 3)");
```

Previously committed bindings remain installed after failure. Native validation errors and translated C++ exceptions follow the same protected path.

Current limitations are intentional: there is no file-loading helper, custom chunk name, sandbox environment, coroutine API, or mechanism for retrieving script return values.

---

[← Previous: Values and validation](values-and-validation.md) · [Documentation index](README.md) · [Next: Errors and results →](errors-and-results.md)
