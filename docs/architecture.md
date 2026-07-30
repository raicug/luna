# Architecture

The public API is deliberately smaller than the implementation behind it. Consumer templates learn a declaration's C++ shape in the consumer's own translation unit, erase it into a canonical descriptor, and hand that over. All Luau interaction stays under `src/state/`, which is private and not public API.

```text
State
 ├─ BindingRegistry → builders → canonical descriptors
 └─ Impl
     ├─ lifecycle      logical identity, owner thread, phase, epochs
     ├─ transaction    capture → preparation → installation → publication
     ├─ identity       canonical encoding, digests, identity registry
     ├─ type           canonical types, conversion frames, conversion boundary
     ├─ registration   pending plans, overload groups, shape validation, store
     ├─ invocation     trampolines, argument binding, overload selection, returns
     ├─ userdata       class registry, construction, ownership, member access
     ├─ module         manifest registry, resolution, load, lifecycle
     ├─ reflection     immutable published storage and views
     ├─ freeze         validated model plus published lookup caches
     ├─ dispatch       permanent closure slots and dispatch generations
     ├─ vm             VM owner, closure installer, tables, interned enum items, stack checkpoints
     ├─ execution      compiler and disposable-thread executor
     ├─ generation     documentation, declaration, publication writers
     └─ testing        private fault injection and test hooks

Luau call → trampoline → receiver gate → overload selection
                       → argument conversion → native target → return publication
```

## Description path

A public builder never installs anything. It captures the declared C++ shape where the type is still complete — parameter and return descriptors, storage size and alignment, destructibility, the enumeration's underlying range — and stages it into a pending plan. That is what lets the backend, which never sees your types, allocate storage and validate values without guessing.

Canonical identity is derived from those descriptors alone. A descriptor is normalized, hashed structurally, and encoded canonically into a 256-bit digest. No RTTI name, address, registration order, locale, or process-random value participates, which is why a `TypeId` or `SymbolId` means the same thing in another State, another run, and a generated artifact.

## Registration path

Every category enters one outermost `RegistrationTransaction` with four phases:

1. **Capture** reads the owner thread, readiness and freeze phase, entry stack depth, logical identity with its epochs, and the current immutable generation set — once. Every later decision of the attempt reads that capture, so the attempt cannot observe a moving target.
2. **Preparation** validates the whole candidate graph: name grammar, parameter and return shapes, overload distinguishability, enumerator ranges, class relationship uniqueness, member policy coherence, module resolution.
3. **Installation** creates the VM values and dispatch targets, recording an undo step for each one.
4. **Publication** swaps in the new immutable generation set atomically.

A failure at any phase runs the recorded undo steps in reverse order and restores the exact pre-attempt state, including any previous global value. A nested builder does not open its own transaction; it joins the outermost one, which is why one plan publishes wholly or not at all.

## Invocation path

The trampoline retains the current dispatch generation, resolves its slot to a target through it, and then works in fixed order: for a member, the receiver access gate (presence, origin State, metatable identity, payload liveness, borrowed lifetime, dynamic type, const permission); then arity against the declared shape; then candidate probing and Pareto-dominant selection; then argument conversion left to right, with defaults materialized only for omitted parameters; then the native target; then return publication of zero, one, or many values.

It catches every C++ exception before returning to Luau. On failure, non-trivial C++ objects leave scope before a minimal tail restores the callback depth, pushes one prepared message, and raises the Luau error.

## Dispatch indirection

A closure carries only the permanent dispatch slot of its canonical path, never a record address. Each State owns one dispatch table that issues those slots and publishes an immutable dispatch generation naming the target of every slot. An invocation retains the generation it entered with, so a call finishes on the target it entered with while later calls resolve the current one. Records remain owned by exactly one State and are destroyed only after the VM has closed.

## Execution and stack safety

The executor compiles source and runs it in a pinned disposable thread. `StackCheckpoint` objects restore root and helper-operation depths on ordinary C++ exits. Error tails restore callback depth explicitly because Luau error raising may not unwind C++ destructors normally.

## Reflection and generation

Reflection is not a view onto live State storage. Each published generation is immutable storage that a `ReflectionSnapshot` shares by reference count, so a snapshot survives later registration, freeze, a State move, and destruction of the originating State, and is readable from another thread. Generation consumes a snapshot and nothing else — no State, VM, table, or dispatch target — which is what makes generated bytes a pure function of captured content plus options.

## Public boundary

Headers under `include/luna/` use only Luna and standard-library types. No Luau type, header, pointer, constant, stack operation, or registry reference is reachable from them, and no consumer macro or compile definition is required. Luau headers appear only in private implementation files, and the VM and compiler are private, link-only dependencies of the `Luna` target.

That boundary is enforced by the build, not by convention. Standalone compile checks build every public header with only Luna's public include directory, and a separate consumer check compiles against that directory with no Luau link dependency and no compile definitions. The build policy test fails if a Luau target leaks into Luna's interface, if a public header lacks its standalone check, or if a target stops compiling as C++20 with extensions disabled.

---

[← Previous: Errors and results](errors-and-results.md) · [Documentation index](README.md) · [Next: Testing and contributing →](testing-and-contributing.md)
