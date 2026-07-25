# Registering functions

Register a callable through the registry associated with a State:

```cpp
int Calls = 0;
auto Result = State.Bindings().Register(
    "Multiply", [&Calls](double Left, double Right) {
      ++Calls;
      return Left * Right;
    });
```

Luna accepts free functions, function pointers, and concrete lambdas with one unambiguous call operator. Capturing and move-only callables are supported when they can be stored from the value passed to `Register`.

Generic lambdas, overloaded call operators, member-function pointers, references, variadics, and unsupported parameter or return types fail at compile time. A null function pointer has a valid signature but fails registration with `NullCallable`.

## Global names

A registered name must be 1–255 ASCII bytes and match `[A-Za-z_][A-Za-z0-9_]*`. `Player_Score2` is valid; an empty name, `2Players`, `Player.Score`, and non-ASCII names are not.

When more than one problem exists, Luna reports one deterministic result in this order:

1. invalid global name
2. State not ready
3. null callable target
4. duplicate registered name

## Registration is transactional

Luna prepares a stable binding record, installs a protected native closure, and commits the record only after installation succeeds. A failure rolls back the pending record, restores the previous global value, and leaves earlier bindings unchanged.

Names already registered through Luna cannot be replaced. A duplicate attempt returns `DuplicateGlobalName`, and the original callable remains installed. There is no public unregister or rebind operation yet.

Registration changes only the target State. Two States may independently register the same name without sharing behavior or storage.

---

[← Previous: State and lifetime](state-and-lifetime.md) · [Documentation index](README.md) · [Next: Values and validation →](values-and-validation.md)
