# Signals and delegates

A **delegate** holds one script function so the host can call it later. A **signal** is a list of handlers, host functions and script functions, that the host fires. A script can connect to a signal, or wait for the next time it fires.

## Delegates

A `luna::Delegate<R(A...)>` parameter captures the function a script passes.

```cpp
luna::Delegate<void(double)> onChange;

registry.Register("OnChange", [&onChange](luna::Delegate<void(double)> handler) {
  onChange = handler;
});

// later, from the host:
const luna::DelegateCallResult result = onChange.Invoke(3.5);
if (!result.IsSuccess())
  std::printf("handler failed: %s\n", result.Diagnostic().c_str());
```

```lua
OnChange(function(value) print("changed to", value) end)
```

| Member | Meaning |
| --- | --- |
| `IsBound()` | The function is held and its state is alive |
| `Invoke(args...)` | Calls it with the arguments converted, returns a `DelegateCallResult` |
| `DelegateCallResult::IsSuccess()` / `Diagnostic()` | Whether it ran, and the error text if not |

### How a call runs

`Invoke` runs the function on a **fresh coroutine of its own**, not on the coroutine of the script that handed it over. That is what makes a delegate safe to keep: the registering script's coroutine is freed once its task ends, while the function is held in the state's registry.

Consequences:

- A delegate works after the script that registered it has finished, and after garbage collection.
- A delegate works while the registering script is suspended, without disturbing it.
- A handler cannot `wait`. The host called it, so there is no task to suspend, and `wait()` raises `cannot wait here` rather than doing nothing. A handler that must wait should start a task with `Spawn`.
- The function's return value is ignored.

An error in the handler comes back in the result. It does not propagate to the host.

## Signals

`luna::Signal<void(A...)>` is declared by the host and fired by the host.

```cpp
luna::Signal<void(double)> changed;
changed.Attach(state);            // needed for :Wait()

luna::Connection token = changed.Connect([](double value) { redraw(value); });
changed.Fire(2.0);
token.Disconnect();               // idempotent
```

### Signal

| Member | Meaning |
| --- | --- |
| `Connect(std::function<void(A...)>)` | Adds a host handler; returns a `Connection` |
| `Connect(Delegate<void(A...)>)` | Adds a script handler; returns a `Connection` |
| `Fire(args...)` | Runs every handler and returns a `FireReport` |
| `HandlerCount()` | How many handlers are connected |
| `Clear()` | Removes every handler |
| `Attach(state)` | Lets `Wait` find the state |
| `AttachedState()` | The state, or `nullptr` if none or it is gone |

`Connection::Connected()` says whether a handle is still live. `Disconnect()` can be called more than once.

### Firing

```cpp
struct FireReport {
  std::size_t Invoked;
  std::size_t Failed;
  std::string FirstError;
  bool IsClean() const;
};
```

`Fire` runs handlers inline, in connection order, over a snapshot of the list:

- **Isolation.** A handler that throws, or a script handler that errors, is counted in `Failed` and its message kept in `FirstError`. The rest still run.
- **Self-disconnect.** A handler may disconnect itself, or another handler, while firing. A handler disconnected before its turn does not run.
- **By-value arguments.** Each handler receives its own copy, so one handler cannot consume the value another sees.

After the handlers finish, `Fire` wakes anything parked on `Wait`, so the state a waiter observes includes the fire.

## Exposing a signal to scripts

```cpp
luna::RegisterSignal(registry, "Changed", luna::StableTypeKey("app.Changed"), changed);
```

This registers a class for the signal and publishes the instance at `Changed`. A script sees:

| Script call | Meaning |
| --- | --- |
| `Changed:Connect(fn)` | Connects a script function. Returns an id. |
| `Changed:Disconnect(id)` | Disconnects it. Returns whether it was connected. |
| `Changed:GetHandlerCount()` | How many handlers there are |
| `Changed:Wait()` | Parks the calling script until the next `Fire`, and returns the fired values |

```lua
Changed:Connect(function(value) fired = fired + value end)

local value = Changed:Wait()      -- resumes with the value that was fired
```

`Wait` parks the script with no wake time, so only a fire resumes it: one host thread, no polling loop.

```cpp
(void)state.Spawn("local v = Changed:Wait() print(v)");
changed.Fire(7.0);
state.Tick(0.0);                  // the waiter resumes, v == 7
```

A signal with several arguments returns them all: `local n, text = Both:Wait()`.

`Wait` needs the signal to be attached to the state. `Execute` of a script that waits returns at once with `IsPending()` true.

## Lifetime

A `Signal` holds the state's runtime and a liveness token, not the `State` object. It keeps working if the state is moved. If the state is destroyed, `Fire` still runs the host handlers and simply has nobody to wake.

`RegisterSignal` publishes the signal as a *borrowed* instance, so the `Signal` object must outlive the state's use of it.

---

[← Previous: Tasks and scheduling](tasks-and-scheduling.md) · [Documentation index](README.md) · [Next: Safety and limits →](safety-and-limits.md)
