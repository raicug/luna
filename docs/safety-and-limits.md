# Safety and limits

How to stop a runaway script, cap what it can allocate, and keep it from rewriting the bindings you gave it.

## Interrupting a script

```cpp
state.RequestInterrupt("stopped by the host");   // safe from any thread
```

Luau calls an interrupt hook at every **safepoint**: loop back edges, calls and returns. luna's hook raises the host's reason as an error, so the script stops at its next safepoint, even inside `while true do end`. No budget or option is needed.

```cpp
std::thread watchdog([&] {
  std::this_thread::sleep_for(std::chrono::seconds(2));
  state.RequestInterrupt("took too long");
});
const luna::ExecutionResult result = state.Execute(source);   // returns, interrupted
watchdog.join();
```

The result reports `ErrorCategory::Interrupted` and `IsInterrupted()`, and its message is the reason you gave.

| Call | Notes |
| --- | --- |
| `RequestInterrupt(reason)` | Callable from any thread. Installs the hook if it is not installed yet. |
| `ClearInterrupt()` | Lets the state run again |
| `IsInterruptPending()` | Whether a request is waiting |

An interrupt is **sticky**: until `ClearInterrupt()`, every script stops at its first safepoint and `Execute` refuses to start. A script that never reaches a safepoint (straight-line code with no loop and no calls) cannot be interrupted, but it also cannot run long.

A native function is not interrupted while it runs. A long native call stops the script only when it returns.

## The instruction budget

```cpp
luna::StateOptions options;
options.InstructionBudget = 50'000'000;
luna::State state(options);
```

The budget is the same hook counting safepoints. A script that uses them all stops with `instruction budget exhausted` and `ErrorCategory::Interrupted`. It is re-armed on each resume, so a script that yields gets a fresh budget every time it is resumed, and a long-lived task is not killed for being long-lived, only a single slice that runs too long.

It is off by default (`0`). There is no single-stepping, so arming it costs one counter decrement per safepoint.

## The memory limit

```cpp
options.MemoryLimitBytes = 64u << 20;
```

A ceiling on the Luau allocator. An allocation past it fails with Luau's `not enough memory`, as a normal error the script can `pcall`, and the state stays usable.

The limit holds everywhere:

- Creating a task and compiling into it (`Execute`, `Spawn`, `Load`, `Chunk::Call`) report the failure as a result.
- A registration that runs out of memory is **undone** and reported as a failed `RegistrationResult`. While luna puts things back it lifts the limit, then restores it and collects what the failed attempt built.

Leave some headroom above what a state needs to start and run; a state filled to its limit has nothing left to make a task with.

`state.MemoryUsed()` reports the bytes the VM holds.

## Stripping libraries

```cpp
options.StripUnsafeLibs = true;
```

Sets `io`, `package` and `debug` to `nil`. `os` stays, because Luau's `os` is only `clock`, `time`, `date` and `difftime`.

## Sandboxing

`state.Sandbox()` makes the globals and the standard library read-only. Do it after every registration, because registering writes globals and is refused once they are locked:

```cpp
registry.Register("Seven", [] { return 7.0; });
state.Sandbox();
state.Execute("_G.Injected = 1");     // an error
registry.Register("Late", [] {});      // refused: "...register before sandboxing"
```

## Locked bindings

A script can reach the tables behind what you registered, so luna locks them:

| What | Protection |
| --- | --- |
| A class's metatable | `getmetatable(instance)` returns `The metatable is locked`, never the table |
| The class table (`Studio.Sprite`) and the tables behind it | Read-only |
| Enum tables and enumerator objects | Read-only |

The host can still extend a class in a later commit. luna lifts the lock for the commit and restores it afterwards. Namespace tables are covered by `Sandbox()`, not by this.

## Borrowed pointers

A script that keeps a borrowed instance can outlive the object it points at. `ClassBuilder::BorrowedFrom(handle)` ties every borrowed instance of a class to a `LifetimeHandle`, and `handle.Invalidate()` makes them fail with `the instance is no longer available`. See [Classes and userdata](classes-and-userdata.md#ownership).

## Type confusion

A class-typed parameter never accepts an instance of another class, and never accepts anything if its class is unregistered. luna refuses a registration that names an unregistered class, because the class's id is resolved when the callable is linked.

## Misspelt members

By default a class returns `nil` for a member it does not have. With `StrictMembers` it raises an error naming the member and the class, which catches a typo where it is written.

```cpp
options.StrictMembers = true;
```

## What is not protected

- A `State` is not thread-safe beyond the interrupt calls. See [State and lifetime](state-and-lifetime.md#threads).
- A script can still use all the CPU a native function it calls uses.
- Two scripts in one state share globals, unless you sandbox them.

---

[← Previous: Signals and delegates](signals-and-delegates.md) · [Documentation index](README.md) · [Next: Errors and results →](errors-and-results.md)
