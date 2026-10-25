# Errors and results

Every fallible operation returns a value you can inspect. Nothing in the public API throws for a failure it can describe, and a host exception never unwinds through the VM.

## The result types

| Type | Returned by |
| --- | --- |
| `RegistrationResult` | `Register`, `RegisterFunction`, `RegisterValue`, every builder's `Commit()` |
| `ExecutionResult` | `Execute`, `Chunk::Call` |
| `DelegateCallResult` | `Delegate::Invoke` |
| `TaskFailure` | `StateOptions::OnTaskError` |
| `FireReport` | `Signal::Fire` |

```cpp
const luna::ExecutionResult result = state.Execute(source);
if (!result.IsSuccess()) {
  const luna::ErrorDiagnostic *error = result.Diagnostic();
  std::printf("%s\n", error->Where().c_str());
}
```

`IsSuccess()` is true when `Diagnostic()` is `nullptr`. `ExecutionResult` adds `IsInterrupted()` and `IsPending()` (a success whose script is parked on a signal).

## ErrorDiagnostic

| Member | Meaning |
| --- | --- |
| `Category()` | An `ErrorCategory` |
| `Message()` | The text, including any `chunk:line:` prefix Luau added |
| `Chunk()`, `Line()` | Split out of the prefix; `Line()` is 0 when there is none |
| `HasLocation()` | Whether a line was found |
| `Where()` | `chunk:line: message`, without repeating a prefix the message already has |
| `Traceback()` | The stack where the error was raised, or empty |

### ErrorCategory

| Category | Meaning |
| --- | --- |
| `None` | No error |
| `Compile` | The source did not compile |
| `Runtime` | The script raised an error, or ran out of memory |
| `Interrupted` | `RequestInterrupt` or the instruction budget stopped it |
| `Registration` | A commit was refused |

### Tracebacks

A task that fails keeps its frames, so the traceback is read from the failed coroutine, once, and only for an error that reaches the host. A script that catches an error with `pcall` costs no traceback. The text lists the chunk and line of each frame, for example `luna:2` for a script's own `error(...)`.

## Argument errors

A call that passes the wrong thing fails with the function, the position and what was expected:

```text
luna:1: argument 1 of NeedsNumber: expected a number, got string
luna:1: argument 1 of ns.get: expected ns.Point, got ns.Bag
```

An overload set reports `no overload of 'Describe' accepts 2 arguments`. A script catches any of these with `pcall`.

## Registration refusals

A refused `Commit()` publishes nothing. These are the messages, grouped by cause:

| Message contains | Cause |
| --- | --- |
| `invalid name` | A name that is empty or has an empty segment (`"a..b"`, `".a"`) |
| `duplicate registration for 'X' in one commit` | Two functions, classes, enums or constants with one name in one batch |
| `has no type token` | A class staged without a type |
| `enum 'X' has no values` | An enum with no enumerators |
| `aliases 'Y', which is not one of its values` | An alias to something that is not there |
| `produced no value` | A `RegisterValue` factory that returned nothing |
| `takes a class that is not registered` | A callable over a class that is neither registered nor in this batch |
| `is already registered for a different type` | A name already used by another C++ type |
| `is declared as both a method and a property` | One name used as two kinds of member |
| `is declared more than once as a property` | A repeated property, field or constant |
| `is already bound to a value that is not a table` | A namespace over a non-table global |
| `declares base 'X', which is not registered before it` | A base that does not exist yet |
| `declares 'X' as its base, but that class is a different type` | `BaseOf<B>` and the name disagree |
| `which is declared later in the same commit` | A base in the same batch, after the derived class |
| `registration failed: ...` | A Lua error during linking: read-only globals (`State::Sandbox` was called), or out of memory |
| `the state is not ready` / `the state is gone` / `registry is not attached` | The registry has no live state |

Everything before `registration failed` is found by validation, before any VM or model state changes. A `registration failed` happens after linking started, and is rolled back: the VM tables, the symbol list, the overload sets and the classes the batch created are all put back, and what the attempt built is collected.

## Execution failures

| Cause | Category | Message |
| --- | --- | --- |
| Source does not compile | `Compile` | Luau's syntax error |
| A script `error(...)` or a runtime fault | `Runtime` | The message, with `chunk:line:` |
| A callable throws | `Runtime` | `Name: what()` |
| `RequestInterrupt` | `Interrupted` | The reason given |
| The instruction budget | `Interrupted` | `instruction budget exhausted` |
| Out of memory | `Runtime` | `not enough memory` |
| A state that is not ready | `Runtime` | `the state is not ready` |
| `Chunk::Call` on a chunk that did not compile | `Compile` | The compile error, kept from `Load` |
| `Chunk::Call` after the state was destroyed | `Runtime` | `the owning state is gone` |
| `wait()` where nothing can yield | `Runtime` | `Name: cannot wait here, ...` |
| An instance whose borrow ended | `Runtime` | `the instance is no longer available` |
| An instance of an unregistered class passed in | `Runtime` | `an instance of a class that is not registered cannot be passed to Lua` |

## Failures nobody is waiting for

A task that `Spawn`, `Tick` or `Execute` ran for someone else cannot return its failure. Set `StateOptions::OnTaskError` to receive it as a `TaskFailure` with the task handle and the `ErrorDiagnostic`. See [Tasks and scheduling](tasks-and-scheduling.md#failures-in-background-tasks).

## Exceptions

A C++ exception thrown by a callable becomes a Luau error, so a script can `pcall` it. An exception thrown by a `Signal` host handler, or an error raised by a script handler, is counted in the `FireReport` and does not stop the other handlers. An exception thrown by `OnTaskError` itself is swallowed.

---

[← Previous: Safety and limits](safety-and-limits.md) · [Documentation index](README.md) · [Next: Reflection and definitions →](reflection-and-definitions.md)
