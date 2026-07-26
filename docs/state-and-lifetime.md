# State and lifetime

`Luna::State` is the public owner of the Luau VM. It follows RAII: construction makes one VM creation attempt, destruction closes an owned VM once, and `IsReady()` reports whether creation succeeded.

```cpp
Luna::State State;
if (!State.IsReady()) {
  // VM creation failed; registration and execution return diagnostics.
}
```

A State cannot be copied. It can be move-constructed or move-assigned without creating another VM. After a move, the destination owns the original VM and the source is valid but not ready.

```cpp
Luna::State Source;
Luna::State Destination = std::move(Source);

assert(Destination.IsReady());
assert(!Source.IsReady());
```

Move assignment releases any VM already owned by the destination before transferring the source. Self-move is guarded and leaves ownership unchanged.

## BindingRegistry lifetime

`State::Bindings()` returns a lightweight, non-owning `BindingRegistry`. It refers to the particular State object, not directly to its VM. A registry obtained before moving a State remains attached to the moved-from source and therefore sees it as not ready.

Keep a registry no longer than its State:

```cpp
auto Bindings = State.Bindings();
auto Result = Bindings.Register("Tick", []() {});
```

Getting a fresh registry at each registration site is cheap and avoids lifetime confusion.

## What a State owns

Privately, each State owns the VM, committed binding records, the dispatch table those records are reached through, and fault/test support. An installed closure holds only a permanent dispatch slot, so it never names a record directly. During destruction, the VM closes before the dispatch table and records are released, so no invocation can resolve a slot after its target is gone.

---

[← Previous: Getting started](getting-started.md) · [Documentation index](README.md) · [Next: Registering functions →](registering-functions.md)
