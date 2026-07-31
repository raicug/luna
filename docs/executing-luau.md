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

## Loading a chunk

`State::Execute` compiles and runs in one step and discards whatever the script returned. `State::Load` separates the two: it compiles source into a `Luna::Chunk` that owns validated bytecode and can be invoked repeatedly.

```cpp
const Luna::Chunk Sum = Owner.Load("local A, B = ... \nreturn A + B", "sum");
if (!Sum.IsLoaded())
  std::cerr << Sum.Diagnostic()->Message() << '\n';   // ErrorCategory::Compilation

Luna::ValuePack Arguments;
Arguments.Append(Luna::OwnedValue::Number(19));
Arguments.Append(Luna::OwnedValue::Number(23));

const Luna::ChunkResult Produced = Sum.Invoke(Arguments);
if (Produced.IsSuccess())
  std::cout << Produced.At(0).ToNumber().value_or(0) << '\n';   // 42
```

`Load` validates by loading the compiled bytecode once and discarding the result, so a chunk that reports `IsLoaded()` will load again. A chunk that does not compile carries its diagnostic through the usual result type: `Diagnostic()` returns an `ErrorCategory::Compilation` failure, and invoking it repeats that diagnostic and runs nothing. The second `Load` overload names the chunk, which is the name Luau reports in error messages; the default is `LunaChunk`.

Arguments and results use the shapes a registered callable already publishes. Arguments are a `ValuePack`, so a chunk receives scalars, tables, nested tables, and manufactured instances through `OwnedValue::Instance<T>`. Results are a `ValuePack` too - as many values as the chunk returned, each an `OwnedValue`, so a returned table is readable field by field. A chunk that raises reports an `ErrorCategory::Runtime` failure; Luna never throws a Luau error into consumer code.

A `Chunk` is an ordinary value: default-constructible, copyable, and storable. It holds a shared handle to the State that compiled it, so invoking a chunk whose State is gone reports `StateNotReady` instead of touching a closed VM. A chunk runs on that State's owner thread only.

### Re-entrancy

A chunk may be invoked **from inside a native call** - a method, a delegate handler, a namespace function - which is what makes `Execute` re-entrancy defined rather than undefined:

- The nested chunk runs on **its own fresh Luau thread**, created from the root exactly as `Execute` creates one. It does *not* share the calling chunk's stack, and it has its own stack checkpoint, so a nested failure cannot leave values on the caller's stack.
- It **shares the State's globals and its dispatch generation**: a global the outer chunk assigned is visible, and a call the nested chunk makes resolves the same dispatch slots at the same generation the rest of the execution is using.
- A **nested chunk that errors does not unwind the calling chunk.** `Invoke` returns a failed `ChunkResult` to the native code that made the call, and that native code decides what to do: report it, ignore it, or raise its own error. If it raises, the outer `pcall` observes that as an ordinary error.
- Nesting is bounded at **16 levels**. A call past the limit is refused deterministically with a diagnostic naming the limit, rather than exhausting the host stack; the refusal is reported to the native code that made the nested call and the State stays usable.
- An interrupt applies to nested chunks too, because the interrupt is installed on the Luau global state. `ChunkResult::IsInterrupted()` reports it.

### Publishing a chunk to a script

A registered callable may **return a `Luna::Chunk`**, and Luna publishes it as a real Luau function rather than as userdata with a `Run` method. That is what makes `loadstring` expressible:

```cpp
Registry.RegisterFunction("loadstring", [&Owner](std::string Source) {
  return Owner.Load(Source, "loadstring");
});
```

```lua
local F = loadstring("local A, B = ... \nreturn A + B")
assert(type(F) == "function")
print(F(1, 2))      -- 3
```

The published closure is an ordinary Luau function: it runs on the caller's thread inside the caller's protected frame, takes whatever arguments the script passes, returns as many values as it produces, sees the State's globals, and can be called any number of times. An error inside it propagates like any Luau error, so `pcall(F)` catches it. A source the compiler rejects refuses the call that produced it - the script never receives a broken function.

## Interrupting execution

A chunk that never calls a host callable - `while true do end` - would otherwise run until it returns. `State::RequestInterrupt(std::string Reason)` stops it:

```cpp
Owner.RequestInterrupt("the user pressed stop");   // any thread
// ...
const Luna::ExecutionResult Stopped = Owner.Execute(Source);
if (Stopped.IsInterrupted())
  std::cerr << Stopped.Diagnostic()->Message() << '\n';
Owner.ClearInterrupt();
```

`RequestInterrupt` is callable **from any thread**, which is the point: a UI thread raises the flag while the owner thread is inside `Execute`. `ClearInterrupt()` disarms it and `IsInterruptPending()` reports the current state; all three are safe on a State whose VM never came up, where they do nothing.

With an interrupt pending, the executing chunk raises a deterministic error at its next interrupt point - a loop back-edge or a call - carrying `Execution interrupted:` and the reason the host stated. The error travels the ordinary protected path, so:

- **`pcall` catches it** like any other error, and the message it receives is the one the host stated.
- The disposable execution thread is reset and the **root stack depth is restored**, exactly as for a script error.
- The **State stays reusable**: registered globals, namespaces, and classes are untouched, and the next `Execute` runs normally.
- `ExecutionResult` reports it as **interrupted, not as a script error** - `IsInterrupted()` is true and the diagnostic category is `ErrorCategory::Interrupted` - so a host can tell "the user stopped it" from "the script threw".

Two rules follow from the flag being sticky:

- The flag **stays armed until `ClearInterrupt()`**. That is what makes the stop button work: a script that catches the error with `pcall` and keeps looping is interrupted again at the next interrupt point, so it cannot outlast the request. The cost is that the host clears the flag once the stop has been handled; a chunk started with the flag still armed is interrupted immediately.
- **Requesting an interrupt with no chunk executing raises nothing and mutates nothing** beyond arming the flag. A host pressing stop always races the chunk finishing, so this case is a plain no-op rather than an error.

If a suspended asynchronous call is outstanding when the interrupt is observed, Luna requests cancellation on that work and settles the call as cancelled - the same path a State shutdown takes - and `Execute` returns the interrupted result without resuming the chunk.

The interrupt applies to the whole Luau global state, so a nested chunk invoked from inside a native call is interrupted too, and the error propagates out through the calling chunk.

## Current limitations

These are intentional gaps rather than partial features:

- No file-loading helper and no sandbox environment. `State::Load` names a chunk, but every chunk shares the State's one global environment.
- `Execute` still discards what the script returned. Use `State::Load` and `Chunk::Invoke` when the values matter.
- Only the chunk `Execute` is running can suspend an asynchronous call. A script coroutine or a metamethod that reaches such a callable is refused deterministically, and the started work is cancelled.
- No annotation helper macros. Documentation, attributes, and examples are ordinary builder calls, and IDE, debug-UI, and profiling integrations are available through the public model.

Execution is otherwise unrestricted: a chunk may freely call any registered function, construct registered classes, read constants and enumerators, and use module surfaces.

---

[← Previous: Freeze and performance](freeze-and-performance.md) · [Documentation index](README.md) · [Next: Errors and results →](errors-and-results.md)
