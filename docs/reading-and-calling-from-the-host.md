# Reading and calling from the host

The host can read a script's globals, assign them, and call its functions without running a chunk. Paths are dotted: `"Config.nested.depth"` walks `Config`, then `nested`, then reads `depth`.

```cpp
state.Execute("Config = { speed = 4 }  function Add(a, b) return a + b end");

const luna::Outcome<double> speed = state.Get<double>("Config.speed");
if (speed)
  std::printf("%g\n", speed.Value());

state.Set("Config.speed", 9.5);
const double sum = state.Call<double>("Add", 2, 3).ValueOr(0.0);
```

| Member | Meaning |
| --- | --- |
| `Get<T>(path)` | Reads the value at `path` as a `T`. Any type in the [conversion table](values-and-conversion.md#conversion-table) works, including containers and `std::optional<T>` for a field that may be absent. |
| `Set(path, value)` | Assigns the value. Every table before the last segment must exist; the last field is created if it does not. |
| `Call<R>(path, args...)` | Calls the function at `path` and reads its first result as an `R`. `Call(path, args...)` ignores results. |

String literals are passed as text. Each call returns an `Outcome`.

## Outcome

`luna::Outcome<T>` holds a value or an `ErrorDiagnostic`. `Outcome<void>` holds only the error.

| Member | Meaning |
| --- | --- |
| `IsSuccess()`, `operator bool` | Whether there is a value |
| `Value()` | The value; only valid after `IsSuccess()` |
| `ValueOr(fallback)` | The value, or `fallback` on failure |
| `Diagnostic()` | The `ErrorDiagnostic`, or `nullptr` on success |

A failure reports a missing parent (`'Config' is nil`), a malformed path, a value of the wrong type, a function that is not a function, a script error, or `Interrupted` while a [stop is pending](safety-and-limits.md).

## Calls run to completion

`Call` runs the function on a coroutine of its own and returns when it ends, like [`Delegate::Invoke`](signals-and-delegates.md#how-a-call-runs). The function cannot `wait`. To run a function that waits, start it with a delegate's `Spawn`.

## Keeping a function

`Get` can read a delegate, which keeps the function for later calls:

```cpp
auto add = state.Get<luna::Delegate<double(double, double)>>("Add");
const double five = add.Value().Invoke(2, 3).ValueOr(0.0);
```

## Delegate return values

`Delegate<R(A...)>::Invoke` returns the function's result when `R` is not `void`: a `DelegateReturn<R>`, which is a `DelegateCallResult` with `Value()` and `ValueOr()`. A result that cannot become an `R` fails the call.
