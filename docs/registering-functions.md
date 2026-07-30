# Registering functions

Register a callable through the registry associated with a State:

```cpp
int Calls = 0;
const Luna::RegistrationResult Result = State.Bindings().RegisterFunction(
    "Multiply", [&Calls](double Left, double Right) {
      ++Calls;
      return Left * Right;
    });
```

`RegisterFunction` and `Register` are the same operation. `Register` is the established spelling and a source-compatible alias: both build the same canonical descriptor, enter the same transaction, install the same callable target, and report the same diagnostics in the same precedence.

Luna accepts free functions, function pointers, concrete lambdas with one unambiguous call operator, functors, static member functions, and explicit member wrappers. Capturing and move-only callables are supported when they can be stored from the value passed to `Register`.

Generic lambdas, C-style variadics, references, and unsupported parameter or return types fail at compile time. A null function pointer has a valid signature but fails registration with `NullCallable`.

## Overload sets

Several declarations under one name form one canonical overload set. `Overload<Signature>` selects which C++ target you mean, without a macro, by naming the signature:

```cpp
int Measure(std::string Text);
int Measure(int Width, int Height);

const Luna::RegistrationResult First = Registry.RegisterFunction(
    "Measure", Luna::Overload<int(std::string)>(&Measure));
const Luna::RegistrationResult Second = Registry.RegisterFunction(
    "Measure", Luna::Overload<int(int, int)>(&Measure));
```

Three target forms are accepted. A free or static function is selected by the declared signature alone. A member function pointer names its class as a second argument — `Luna::Overload<void(int), Actor>(&Actor::Move)` — and the wrapper's own declared signature gains the receiver as its first parameter. Any other callable object is accepted when it is invocable with exactly the declared parameter types and yields exactly the declared return type.

A wrapper is not a cast. Its declared parameter and return types are the ones Luna canonicalizes, so a signature naming a type the registry cannot convert is still refused.

At call time, candidates are ranked per argument — `Exact`, `SafeBuiltIn`, `User` — and the winner is the one that Pareto-dominates the others over the whole rank sequence. Ranks are never summed into a score, so a candidate never wins one position by losing another. Two candidates whose declared shapes no call could tell apart are refused transactionally at registration rather than left to resolve ambiguously later.

## Parameters

Beyond the plain supported value types, a parameter may be:

- **Optional.** A trailing `std::optional<T>` maps both omission and an explicit `nil` to the empty value; a present non-nil value converts as its ordinary type.
- **Defaulted.** `WithDefaults` attaches immutable default values to the trailing parameters. Each default is validated at registration and materialized only when its parameter is omitted. An explicit `nil` is a supplied value, so it follows the parameter's ordinary conversion instead of selecting the default.
- **Variadic.** One final parameter of `Luna::ArgumentView` or `Luna::ArgumentPack` consumes every remaining call argument.

```cpp
std::string Greet(std::string Name, std::optional<std::string> Title);
std::string Shorten(std::string Text, int Limit);

const Luna::RegistrationResult Optional =
    Registry.RegisterFunction("Greet", &Greet);
const Luna::RegistrationResult Defaulted =
    Registry.RegisterFunction("Shorten", Luna::WithDefaults(&Shorten, 8));
```

A required parameter may not follow an optional or defaulted one, a variadic parameter must be final, and there is at most one. Each of those is a deterministic refusal naming the offending one-based position.

### Variadic arguments

`ArgumentView` is callback-lifetime only. It names the argument frame Luna opened for the current invocation, reports the one-based call position of every element, and becomes inert once the invocation returns. `ToOwned()` is the documented way to keep the arguments; it yields an owning `ArgumentPack`. Declaring the parameter as `ArgumentPack` asks Luna for the owning form directly.

```cpp
std::string Join(Luna::ArgumentView Arguments) {
  std::string Joined;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (!Joined.empty())
      Joined += ", ";
    Joined += std::to_string(Arguments.Position(Index));
    Joined += '=';
    if (const std::optional<std::string> Text = Arguments.ToText(Index))
      Joined += *Text;
    else if (const std::optional<double> Number = Arguments.ToNumber(Index))
      Joined += std::to_string(*Number);
    else
      Joined += Luna::ValueCategoryText(Arguments.Kind(Index));
  }
  return Joined;
}
```

A variadic element may also be a registered class instance — passed directly, or nested inside a table — carried as `Luna::OwnedValue::Kind() == ValueCategory::Userdata`. Its `UserdataClassName()` names the registered class whether or not the concrete C++ type is known to the reading code, and `UserdataIsLive()` reports whether the underlying object is still valid; the value itself is retained through Luna's own reference mechanism, so it stays usable for the remainder of the call even though the `ArgumentView`/`ArgumentPack` frame it came through is not the object's owner.

```cpp
int CountLiveInstances(Luna::ArgumentView Arguments) {
  int Count = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    const Luna::OwnedValue Element = Arguments.At(Index);
    if (Element.Kind() == Luna::ValueCategory::Userdata && Element.UserdataIsLive())
      ++Count;
  }
  return Count;
}
```

## Returns

A callable publishes returns in exactly one of three shapes:

| Return type | Published values |
|---|---|
| `void` | zero |
| one supported value | exactly one |
| `std::pair`, `std::tuple` | ordered, count fixed by the signature |
| `Luna::ReturnPack` | ordered, count decided by the invocation |

```cpp
std::tuple<int, int, std::string> Analyze(std::string Text);

Luna::ReturnPack Tally(Luna::ArgumentView Arguments) {
  Luna::ReturnPack Pack;
  Pack.AppendInteger(static_cast<int>(Arguments.Size()))
      .AppendNumber(0.5)
      .AppendText("counted");
  return Pack;
}
```

A pack is staging, not output. Luna converts and validates every element before any return is exposed, so a refused element publishes zero values rather than a partial list. The typed appends exist because a bare literal would otherwise let the value variant pick a surprising alternative.

## Asynchronous results

A namespace or root function may deliver its value after the call that started it returns. Return `Luna::AsyncTask<T>` or `std::future<T>`, where `T` is `void`, one supported value, or `Luna::ReturnPack`:

```cpp
Luna::AsyncTask<std::string> ReadLater(std::string Path) {
  Luna::AsyncCompletionSource<std::string> Source;
  Luna::AsyncTask<std::string> Pending = Source.Task();
  Workers.Enqueue([Source, Path]() mutable {
    if (Source.IsCancellationRequested())
      return;
    static_cast<void>(Source.Complete(Load(Path)));
  });
  return Pending;
}
```

Luau calls it like any other function: `local text = ReadLater('logo.png')`. The declared return type is the awaited value, so reflection, conversion, validation, overload resolution, and generated declarations all describe `string`; `ReflectionRecord::IsAsynchronous()` is what names the delivery.

What Luna guarantees while the work is outstanding:

- The chunk `Execute` is running suspends. Its native frame is gone, so nothing keeps the stack, an `ArgumentView`, a `ValueView`, or a conversion context alive. The suspended call owns copies of its arguments and its awaited return metadata, and it retains the immutable dispatch generation it was dispatched through, so it resumes correctly even if that binding is retired meanwhile.
- Luna advances the work on the owner thread and never touches the VM from anywhere else. Your worker settles its own completion state and nothing more.
- The first settlement wins. `Complete`, `Fail`, and `Cancel` each return `false` when the work already settled, so a late worker can never publish a second value.
- A failed or cancelled call raises a deterministic diagnostic through the normal protected path: the stack is restored exactly, no value is published, `pcall` can catch it, and the State stays reusable.
- Cancellation is settled state, not a race. If the execution ends or the State is destroyed while a call is still suspended, Luna requests cancellation, settles the call as cancelled, and the host observes `AsyncStage::Cancelled` on its own completion state.

Two boundaries are deliberate. Only the chunk `Execute` is running can suspend, so a script coroutine or a metamethod that reaches such a callable is refused with a deterministic diagnostic and the started work is cancelled. Class members and operators publish their value inside the expression that invoked them, so asynchronous delivery there is refused at registration time.

## Delegates and signals

A parameter declared as `Luna::Delegate<Signature>` accepts one subscribed Luau function. It is an ordinary reflected parameter of a canonical callable type; no macro, annotation, or parallel callback path is involved:

```cpp
int Subscribe(Luna::Delegate<void(int)> Handler) {
  return Alarm.Subscribe(std::move(Handler));
}
```

`Delegate<Signature>` holds the subscribed function through Luna's own reference mechanism rather than a raw stack index, so it can be stored, copied, and called long after the registering call returns. Calling it never throws by default: `Invoke(Arguments...)` returns a `DelegateCallResult` naming the outcome (`Ready`, `Released`, `ForeignThread`, `HandlerFailed`, `ResultMismatch`). `operator()` is the throwing form, for code that prefers an exception to a checked result.

`Luna::Signal<Signature>` owns a list of subscribed delegates and provides the three operations an event source needs:

```cpp
Luna::Signal<void(int)> Alarm;

Registry.RegisterFunction("Subscribe", [&Alarm](Luna::Delegate<void(int)> Handler) {
  return Alarm.Subscribe(std::move(Handler));                // returns a token
});
Registry.RegisterFunction("Unsubscribe", [&Alarm](int Token) {
  return Alarm.Unsubscribe(Token);                           // releases the handler
});
Registry.RegisterFunction("Raise", [&Alarm](int Level) {
  return static_cast<int>(Alarm.Emit(Level).Delivered);
});
```

What Luna guarantees:

- Unsubscribing releases the handler's native reference immediately, for every copy of that delegate — a later call through any of them reports `DelegateStatus::Released`.
- Emitting takes one snapshot of the current subscribers, so a handler may subscribe or unsubscribe while the emission is running: a handler removed during the emission is never called by it, one added during the emission is delivered starting with the next emission, and no handler is ever called twice by the same emission.
- A handler that raises a Luau error does not fail the emission; `SignalEmission` reports how many delivered, how many were skipped, how many failed, and the first failure's message.
- Replacing the State's lifecycle generation invalidates every handler subscribed through it; a handler that outlives its State, or a call made from a thread other than the State's owner thread, refuses deterministically rather than touching a closed or foreign virtual machine.

## Names

A registered name segment must be 1–255 ASCII bytes and match `[A-Za-z_][A-Za-z0-9_]*`. `Player_Score2` is valid; an empty name, `2Players`, `Player.Score`, and non-ASCII names are not. Qualified names are built by Luna from nested scopes, so you never spell a dot yourself.

When more than one problem exists, Luna reports one deterministic result in this order:

1. invalid global name
2. State not ready
3. null callable target
4. duplicate registered name

## Registration is transactional

Every category — a root function, a namespace plan, an enum, a class, a module graph — registers through one *outermost* transaction. Luna prepares the descriptors, stages the pending model, installs the VM values, and publishes one new generation atomically. A failure undoes the completed steps in reverse order, restores any previous global value, and leaves earlier bindings exactly as they were.

A root single-symbol call such as `Register` or `RegisterConstant` is its own outermost transaction and commits immediately. A builder chain is one transaction spanning the whole plan: nothing it stages reaches the VM before `Commit`.

Names already registered through Luna cannot be replaced. A duplicate attempt returns `DuplicateGlobalName`, and the original callable remains installed. There is no public unregister, rebind, or replace operation.

Registration changes only the target State. Two States may independently register the same name without sharing behavior or storage.

---

[← Previous: State and lifetime](state-and-lifetime.md) · [Documentation index](README.md) · [Next: Values and validation →](values-and-validation.md)
