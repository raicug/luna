# State and lifetime

A `luna::State` owns one Luau VM, the committed model of everything registered into it, and a task scheduler. This page covers its options, what moving or destroying it does to the objects around it, and which calls are safe from which thread.

## Creating a state

```cpp
luna::State state;                          // defaults
luna::State limited{luna::StateOptions{}};  // explicit options
if (!state.IsReady()) { /* allocation of the VM failed */ }
```

`IsReady()` is false only if the VM could not be created, or for a state that has been moved from. Every other method on a state that is not ready returns a failure result, a null handle or an empty value rather than crashing.

### StateOptions

```cpp
struct StateOptions {
  std::uint64_t InstructionBudget = 0;   // safepoints per resume; 0 is no limit
  std::size_t MemoryLimitBytes = 0;      // allocator ceiling; 0 is no limit
  bool StripUnsafeLibs = false;          // remove io, package and debug
  bool StrictMembers = false;            // unknown member reads raise an error
  std::function<void(const TaskFailure &)> OnTaskError;
};
```

| Option | Meaning |
| --- | --- |
| `InstructionBudget` | Counts safepoints (loop iterations and calls). A script that exhausts it fails with `ErrorCategory::Interrupted` and the message `instruction budget exhausted`. It is re-armed on every resume, so a script that yields gets a fresh budget. |
| `MemoryLimitBytes` | A ceiling on the Luau allocator. An allocation past it fails with `not enough memory`, and the state stays usable. |
| `StripUnsafeLibs` | Sets `io`, `package` and `debug` to `nil`. `os` stays: Luau's `os` is only `clock`, `time`, `date` and `difftime`. |
| `StrictMembers` | Reading a member a class does not have raises `X is not a valid member of Y`, instead of returning `nil`. |
| `OnTaskError` | Receives failures from tasks nobody is waiting on. See [Tasks and scheduling](tasks-and-scheduling.md#failures-in-background-tasks). |

`state.Options()` returns the options a state was made with, and `state.MemoryUsed()` the bytes the VM currently holds.

## Moving

`State` is move-only. Moving transfers the VM, the model and the scheduler; the moved-from state reports `IsReady() == false`.

Objects taken from a state *before* a move keep working after it:

| Taken before the move | After the move |
| --- | --- |
| `BindingRegistry` from `Bindings()` | Registers into the state that now owns the VM, and `Owner()` names it |
| `Signal` attached with `Attach(state)` | Still wakes its waiters |
| `Chunk` from `Load()` | Still callable |
| `Delegate` | Still invokable |

That works because they hold the state's *runtime*, which a move does not relocate, and a liveness token that says whether it still exists.

## Destroying

Destroying a state closes the VM. Anything that outlives it notices rather than crashing:

| Outlives the state | Behaviour |
| --- | --- |
| `BindingRegistry` | `RegisterFunction` and friends return a failed `RegistrationResult` ("the state is gone"); `Owner()` returns `nullptr` |
| `Signal` | `Fire` still runs host handlers; there is nothing to wake; `AttachedState()` returns `nullptr` |
| `Chunk` | `Call()` returns a failure ("the owning state is gone") |
| `Delegate` | `IsBound()` is false; `Invoke` returns a failed `DelegateCallResult` |
| `ReflectionSnapshot` | Stays fully readable. It owns its strings. |

> **Note.** A `Delegate` releases its registry reference when its last copy is destroyed, and that must happen on the thread that owns the state. Release delegates before, or on the same thread as, the state.

## Threads

A `State` has no internal locking. Use it from one thread at a time, normally the one that created it.

| Call | From another thread |
| --- | --- |
| `RequestInterrupt`, `ClearInterrupt`, `IsInterruptPending` | Safe |
| `Symbols`, `Reflection`, `Definitions` | Safe against a concurrent commit: the symbol list is guarded |
| `ReflectionSnapshot`, `SymbolRange` already taken | Safe: immutable |
| Everything else | Not safe |

To run several scripts at once, give each worker its own `State`, or use one state and let the scheduler interleave them as coroutines (see [Tasks and scheduling](tasks-and-scheduling.md)).

## Sandboxing

```cpp
state.Sandbox();
```

`Sandbox()` calls Luau's `luaL_sandbox`: the globals and the standard library tables become read-only, and scripts can no longer add globals. Because registration writes globals, **register everything first, then sandbox**. A commit after `Sandbox()` is refused with a diagnostic that says so; it does not throw.

## Reading the model

`state.Symbols()` returns a `SymbolRange` over everything registered, and `state.Reflection()` returns an immutable `ReflectionSnapshot`. `state.Definitions()` renders them as `.d.luau`. See [Reflection and definitions](reflection-and-definitions.md).

## Reaching the state from a callable

A native function sometimes needs the state that is running it, to `Spawn` another script or `Wake` a waiter. `luna::StateFromVM(args.GetVM())` returns it from an `ArgumentPack`:

```cpp
registry.RegisterFunction("spawn", [](luna::ArgumentPack args) {
  luna::State *host = luna::StateFromVM(args.GetVM());
  (void)host->Spawn(std::string(args.View(0).AsString()));
});
```

Starting a task from inside a task is safe. The scheduler saves and restores its own bookkeeping around every resume.

---

[← Previous: Getting started](getting-started.md) · [Documentation index](README.md) · [Next: Registering functions →](registering-functions.md)
