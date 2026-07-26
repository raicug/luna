# Architecture

The public API is deliberately smaller than the implementation behind it. Consumer templates learn a callable's C++ signature, but all Luau interaction stays under `src/state/`.

```text
State
 ├─ BindingRegistry → callable traits → erased descriptor
 └─ Impl
     ├─ VirtualMachineOwner → executor / closure installer
     ├─ BindingStore → stable BindingRecord objects
     └─ FaultInjector → private test-only failure controls

Luau call → NativeTrampoline → validator → argument reader
                               → erased callable → return writer
```

## Registration path

The public template deduces ordered parameter kinds and return disposition, then stores the callable in an `ErasedCallableDescriptor`. `State::Impl` validates the request, prepares a heap-stable `BindingRecord`, and asks the closure installer to place a native global under protection. The store commits only after that succeeds.

A closure carries only the permanent dispatch slot of its canonical path, never a record address. Each State owns one dispatch table that issues those slots and publishes an immutable dispatch generation naming the target of every slot. Records remain owned by exactly one State and are destroyed only after the VM has closed.

## Invocation path

`NativeTrampoline` retains the current dispatch generation, resolves its slot to a target record through it, validates arity and arguments, invokes the erased callable, and writes zero or one result. It catches every C++ exception before returning to Luau. On failure, non-trivial C++ objects leave scope before a minimal tail restores the callback depth, pushes one prepared message, and raises the Luau error.

## Execution and stack safety

The executor compiles source and runs it in a pinned disposable thread. `StackCheckpoint` objects restore root and helper-operation depths on ordinary C++ exits. Error tails restore callback depth explicitly because Luau error raising may not unwind C++ destructors normally.

## Public boundary

Headers under `include/luna/` use only Luna and standard-library types. Luau headers appear only in private implementation files. Standalone compile checks build every public header with only Luna's public include directory, and a separate consumer check verifies that no Luau dependency leaks through.

---

[← Previous: Errors and results](errors-and-results.md) · [Documentation index](README.md) · [Next: Testing and contributing →](testing-and-contributing.md)
