# Architecture

How luna is put together behind the public headers. Read this when you are changing the library, or when you want to know why something behaves the way it does.

## Layers

```text
 your code
    │   #include <luna/luna.hpp>
    ▼
 public headers (include/luna/)        fwd · value · convert · bound · state · signal
    │   templates: codecs, Callable, builders, Delegate, Signal
    ▼
 src/                                   registry · model · userdata · convert · state · docs
    │   Luau-aware, not public API
    ▼
 Luau (VM + Compiler)
```

| File | Owns |
| --- | --- |
| `fwd.hpp` | Ids, results, diagnostics, `Symbol`, `LifetimeHandle`, `OwnershipPolicy`, `ClassOperator` |
| `value.hpp` | `Value`, `OwnedValue`, `ValuePack`, `ArgumentPack` |
| `convert.hpp` | The codec: `ReadValue`, `WriteValue`, `AcceptValue`, the object layout |
| `bound.hpp` | `Callable`, `Target`, `Delegate`, the builders and `BindingRegistry` |
| `state.hpp` | `State`, `StateOptions`, `Chunk`, `ReflectionSnapshot` |
| `signal.hpp` | `Signal`, `Connection`, `FireReport`, `RegisterSignal` |
| `src/runtime.hpp` | `Runtime`: the VM, scheduler, memory arena and guard |
| `src/model.*` | `Model`: classes, symbols, overload lists and the undo log |
| `src/registry.cpp` | `Commit`: validation, linking, rollback |
| `src/userdata.cpp` | Instances and the metamethods behind a class |
| `src/convert.cpp` | `OwnedValue` and `ArgumentPack` implementations |
| `src/state.cpp` | The scheduler, `Chunk`, hooks and the guarded call |
| `src/docs.cpp` | `.d.luau` generation |

The boundary rule: nothing a consumer includes names a Luau *type* in its own declarations except through `lua_State *` in codec templates. `tests/boundary.cpp` compiles each public header alone to enforce that.

## A call, end to end

`Add(2, 3)` from a script:

1. Luau calls the closure `Callable<double, double, double>::Call`. Its one upvalue is a userdata holding the `Callable`.
2. `Dispatch` runs `InvokeCall` inside a `try`. Each parameter is read by `ReadSlot<A>` at its stack position, using the class ids resolved when the callable was linked. A bad argument sets an error string.
3. The erased `std::function` runs. `CallAndWrite` pushes its return with `WriteReturn`: a pair or tuple becomes several values, a class by value is moved into new userdata, a reference is borrowed.
4. If the callable called `luna::Suspend`, `SuspensionRequested` is set. `Dispatch` checks that the call is in a yieldable coroutine, lets `CompleteSuspension` record when the task wakes, and returns `lua_yield`.
5. An exception, or an argument error, becomes `luaL_error` carrying the function's name. The suspension request is cleared first, so it cannot leak into the next call.

A member read, `s.Width`, goes through the class's `__index` closure. Its upvalues are the members table, the class name and an optional fallback. A method is returned as a closure; a property or field is stored as a bare callable, so `__index` reads its thunk from the userdata header and calls it in place.

## The class model

A registered class has, in the VM:

| Table | Reachable by | Holds |
| --- | --- | --- |
| Metatable | `getmetatable` (locked) | `__index`, `__newindex`, `__type`, `__metatable`, and the operators |
| Members | `__index` upvalue | Methods, property getters, field getters, `IsA` |
| Setters | `__newindex` upvalue | Property and field setters |
| Public table | `Studio.Sprite` | Factories, static methods, constants |

Each is pinned in the registry by an integer ref held in `ClassInfo`, which also stores the class id, the base id and offset, the alias list and the lifetime control. A class id is a dense `uint32` from a counter; `ClassIdForToken` maps a type token (`&TypeToken<T>`) to it.

An instance is a userdata holding an `Object`:

| Field | Meaning |
| --- | --- |
| `Magic` | Identifies luna's own userdata, so a foreign one is never misread |
| `ClassId` | The instance's class |
| `Tag` | `Owned`, `Shared` or `Borrowed` |
| `Pointer` | The object |
| `Destroy` | How to destroy an owned object |
| `Control` | The `LifetimeHandle` control block for a borrowed object |
| `Shared` | A `shared_ptr<void>` holder, for a shared object |

Reading an instance checks `Magic`, the class and, for a borrowed one, `Control->Alive`. A derived instance passed where a base is expected goes through `Model::RebaseTo`, which adds each class's stored base offset along the chain.

Inheritance copies the base's members, setters and metamethods into the derived class's tables at commit, skipping names the derived class already has.

## Registration pipeline

`Commit(Runtime &, Batch &&)` is the only place the VM and the model change. It runs in four steps.

**1. Validate.** Pure functions over the staged batch. They check names, duplicates, enum aliases, each callable's classes, name-versus-type clashes, member-kind clashes and that no namespace path runs into a non-table global. Nothing has been touched.

**2. Mark.** `Model::Mark()` records the sizes of the symbol list, the class list and the lifetimes.

**3. Link, under a guard.** The batch is linked inside `RunGuarded`, a `lua_pcall` through a closure made at start-up (so even setting up the call cannot run out of memory). Class tables are unlocked first. Linking creates classes, installs members, operators and constants, links functions, constants and enums, and last runs `RegisterValue` factories. **Every write is logged**: before a table slot changes, `RememberSlot` records what was there; overload lists, symbol edits, base edges, aliases, namespaces and class lifetimes are logged by the model.

**4. Seal or roll back.** On success the log is dropped. On a failure, whether a Lua error or an ordinary one such as a factory that returns nothing, the log is replayed newest first: slots restored, symbols and overload lists truncated, classes this batch created removed. The memory limit is lifted for this and a full collection run afterwards. The class tables are then locked again.

A Lua error inside the guard comes back as `registration failed: ...`, so `Commit` never throws.

## Callables and overloads

`MakeCall` erases any callable into a `Target`: a `Push` function that makes a closure, a `Context` function that makes the bare callable, the `Thunk` that invokes it, an `Accept` thunk for overloads, the arity, the required count, and `Needs`, the class tokens in the signature.

Registering a name a second time builds an **overload set**: one closure whose upvalues are the candidates. `OverloadDispatch` calls each candidate's `Accept` in order and invokes the first that passes. `Accept` is `AcceptValue`, the strict test that never converts or mutates the stack.

## The scheduler

`Runtime` holds a `std::vector<Task>`. A `Task` is a coroutine, its registry ref, a wake time, an optional wait key, and an optional `Resume` function that pushes values for a `Wake`.

- `ResumeTask` saves `HasRunning`, `RunningIndex` and the budget counter, resumes, and restores them, so a nested `Spawn`, `Execute` or `Chunk::Call` leaves the outer task intact. It marks the task `Running` so a nested pump cannot resume it.
- It never holds a reference into the vector across a resume, because a script can start tasks and grow it.
- `PumpScope` counts active scheduler loops. `CompactTasks` reclaims finished entries only when that count is zero.
- `CompleteSuspension` turns a `Suspend` or `Wait` request into a wake time or a wait key on the running task.

## Interrupts and budgets

`lua_Callbacks::interrupt` is installed lazily: at construction when a budget is set, otherwise on the first `RequestInterrupt`. The hook ignores collector steps, decrements the budget, checks the atomic stop flag, and raises the reason with `lua_error` after making room on the stack. The reason string is behind a mutex.

## Delegates

A `Delegate` stores the *main thread* of the state and a registry ref, not the coroutine that captured the function. `Invoke` creates a fresh thread from the main thread, anchors it on the main stack, pushes the function and arguments, and calls with `lua_pcall`. It is why a delegate survives the end of the task that registered it.

## Guarded calls

`RunGuarded(Runtime &, work, failure)` runs `work` under `lua_pcall` on the main thread through a closure created in the `State` constructor. It is used for the link phase, the rollback, and for creating a task and compiling into it. A Luau error or a `MarshalError` comes back as text.

## Symbols

`Model` keeps `SymbolEntries`, indexed by qualified name, behind a mutex. `Snapshot()` builds an immutable vector that `ReflectionSnapshot` and `SymbolRange` share, and caches it until a symbol changes. `docs.cpp` renders that vector.

## What was left out on purpose

| Left out | Why |
| --- | --- |
| Content-hashed 256-bit type and symbol ids | A dense counter is enough; nothing persists an id |
| A parallel descriptor model | The codec is templates, so there is no second description of a type to keep in step |
| `Freeze` and generation-keyed caches | Luau's own tables are the lookup, and a `vector` indexes by slot |
| A module lifecycle | Nothing unloads a module |
| `AsyncTask` / `std::future` returns | The scheduler and `Suspend` cover waiting |
| `ReturnPack`, `ExecutionPolicy` | `ValuePack` and `StateOptions` cover them |
| Annotation macros | Everything is declared through builders |
| Structural type conversion | A type is a scalar, a string, a container, or a registered class |
| Fault-injection hooks in the library | Tests drive real failures |

---

[← Previous: Performance](performance.md) · [Documentation index](README.md) · [Next: Testing and contributing →](testing-and-contributing.md)
