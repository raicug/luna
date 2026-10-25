# Tasks and scheduling

Every script runs as a **task**: a Luau coroutine owned by the state's scheduler. A script that calls `wait` is suspended, not blocked, so one host thread can run many scripts at once.

## The model

- Time is **virtual**. The scheduler keeps a clock that only moves when the host advances it, with `Tick(dt)`, or when `Execute` fast-forwards it.
- A task is *due* when its wake time has passed. `Tick` resumes every due task, once.
- A task is *parked* when it waits for a signal rather than for time. Only `Wake` resumes it.
- Finished tasks are reclaimed automatically; `TaskSlots()` stays small however many scripts have run.

## Running source

| Call | Behaviour |
| --- | --- |
| `Execute(source)` | Runs to completion. Advances the clock itself, so a script that waits still finishes. Returns the script's result. |
| `Spawn(source)` | Starts a task and returns its handle at its **first suspension**. The host must call `Tick`. Returns 0 if the source does not compile. |
| `Load(source, name)` then `Chunk::Call(args)` | Compiles once, runs many times. A chunk that waits carries on from `Tick`. |
| `Tick(dt)` | Advances the clock by `dt` seconds and resumes every due task. |
| `Cancel(handle)` | Drops a task. Returns false if it is not running. |
| `PendingTasks()` | How many tasks are still alive |

```cpp
const std::uint64_t handle = state.Spawn("for i = 1, 3 do wait(1) print(i) end");
for (int frame = 0; frame < 200; ++frame)
  state.Tick(1.0 / 60.0);
```

### Execute and the clock

`Execute` drives its own suspensions: when its script waits, it advances the shared clock to the wake time and keeps going. That also resumes every other task that becomes due on the way, and it means a single `wait(3600)` moves every task's time on by an hour. When the host should control time, use `Spawn` and `Tick`.

If the script is parked on a signal instead, `Execute` returns at once with a result whose `IsSuccess()` is true and `IsPending()` is true: the script has not finished, and a later `Tick` will. A finished script is never pending.

## Suspending from native code

```cpp
registry.Register("wait", [](double seconds) { luna::Suspend(seconds); });
```

`luna::Suspend(seconds)` records the request. When the callable returns, the call yields the running task, which the scheduler resumes after `seconds` of virtual time. No thread is parked, and nothing polls.

`luna::Suspend()` with no argument yields until the next `Tick`.

### Where a callable may wait

Only a task the scheduler runs can yield. In any other context the request is refused with an error that names the function:

```text
Value: cannot wait here, because this call is not running in a task that can yield
```

That covers a handler a host `Delegate` or `Signal` calls, and a getter reached through a metamethod. It is an error rather than a silent no-op so a missing wait never goes unnoticed. A callable that asks to wait and then fails does not leave the request behind for the next call.

## Waiting for an event

`State::Wait(key)` and `State::Wake(key)` are the primitive a `Signal` uses:

```cpp
state.Wake(&event);                                  // resume everything parked on &event
state.Wake(&event, [](void *vm) { /* push values */ return 0; });
bool any = state.HasWaiters(&event);
```

A `Wake` resumes the parked tasks at the next `Tick`. When it is given a function, that function pushes the values the parked call returns. This is how `Signal:Wait()` hands a script the values that were fired. See [Signals and delegates](signals-and-delegates.md).

## Chunks

```cpp
luna::Chunk chunk = state.Load("local n = ... wait(0.1) return n", "job");
if (!chunk.IsValid()) { /* the compile error is reported by Call() */ }

luna::ValuePack args;
args.Append(42);
luna::ExecutionResult first = chunk.Call(args);   // runs until it finishes or first waits
```

`Call` can be repeated, and each call runs on its own coroutine. A chunk that fails to compile is still a `Chunk`; `Call()` returns the compile error. The chunk's own return values are not collected.

Passing an `OwnedValue::Instance` of a class that is not registered fails the call with `not registered`, and does not throw.

## Failures in background tasks

`Execute` returns its own script's failure. Nothing returns the failure of a task nobody is waiting for, so `StateOptions::OnTaskError` does:

```cpp
luna::StateOptions options;
options.OnTaskError = [](const luna::TaskFailure &failure) {
  std::fprintf(stderr, "task %llu: %s\n",
               (unsigned long long)failure.Task,
               failure.Error.Where().c_str());
};
luna::State state(options);
```

It is called for a `Spawn` that fails on its first run, a task that fails after `Tick` resumed it, and a task `Execute` pumped on the way. `TaskFailure::Task` is the handle, or **0** when the source did not compile and no task was made. The handler runs on the pumping thread, may call back into the state, and an exception it throws is swallowed.

## Re-entrancy

A native callable may start another task (`Spawn`), run source (`Execute`, `Chunk::Call`) or tick the scheduler. The scheduler saves and restores its bookkeeping around every resume, never resumes a task that is already running, and never reclaims a slot a running loop is holding, so the outer task is undisturbed:

```cpp
registry.RegisterFunction("spawn", [](luna::ArgumentPack args) {
  luna::State *host = luna::StateFromVM(args.GetVM());
  (void)host->Spawn(std::string(args.View(0).AsString()));
});
// spawn("...") followed by wait(1) in the caller still waits.
```

## Running out of memory

Making a task and compiling its source can run out of memory when a limit is set. `Execute`, `Spawn`, `Load` and `Chunk::Call` report that as a failure result (or 0, or an invalid chunk) instead of throwing.

---

[← Previous: Classes and userdata](classes-and-userdata.md) · [Documentation index](README.md) · [Next: Signals and delegates →](signals-and-delegates.md)
