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

## Logical identity and thread affinity

A State has two identities. The *owner object* is the `Luna::State` variable you hold; the *logical State* is the implementation it owns. A move transfers the implementation, so the logical State keeps its identity, its committed model, and its reflection generations, while the owner object changes.

The owner thread belongs to the logical State. It is the thread that constructed the implementation, and a move does not adopt the thread that performed the move. Every VM-backed operation — registration, commit, freeze, execution — must happen on that thread. Reflection snapshots are the exception: they own immutable storage and are readable from any thread.

Practically: create a State on the thread that will script with it, and pass snapshots, not registries, to worker threads.

## Lifecycle phases

A logical State is *ready* and open to registration, then optionally *frozen*.

| Phase | Registration | Invocation | Reflection |
|---|---|---|---|
| Not ready | refused | — | empty snapshot |
| Ready | permitted | permitted | permitted |
| Frozen | refused | permitted | permitted |

`BindingRegistry::Freeze()` performs the transition. It is described in [freeze and performance](freeze-and-performance.md).

## BindingRegistry lifetime

`State::Bindings()` returns a lightweight, non-owning `BindingRegistry`. It refers to the particular State object, not directly to its VM. A registry obtained before moving a State remains attached to the moved-from source and therefore sees it as not ready.

Keep a registry no longer than its State:

```cpp
Luna::BindingRegistry Bindings = State.Bindings();
const Luna::RegistrationResult Result = Bindings.Register("Tick", []() {});
```

Getting a fresh registry at each registration site is cheap and avoids lifetime confusion.

## Builder lifetime

`NamespaceBuilder`, `EnumBuilder<T>`, and `ClassBuilder<T>` own a *pending plan* rather than anything installed. Destroying one uncommitted has no effect on the VM, on reflection, or on dispatch.

Each builder also carries the logical State identity, the owner-object epoch, the scope, and the lifecycle generation it was created with. Using one after the implementation moves to another owner object, after the owner is destroyed, after freeze, or after its scope disappears fails with one deterministic stale-builder diagnostic instead of touching the VM. So a builder is a short-lived local, staged and committed in one place.

## What a State owns

Privately, each State owns the VM, the committed binding, type, and reflection model, the transaction machinery, the dispatch table those records are reached through, the frozen lookup caches once published, and test support. An installed closure holds only a permanent dispatch slot, so it never names a record directly. During destruction, the VM closes before the dispatch table and records are released, so no invocation can resolve a slot after its target is gone.

A reflection snapshot deliberately does not participate in that ownership. It shares immutable published storage, so it stays valid and unchanged after the State that produced it is destroyed.

---

[← Previous: Getting started](getting-started.md) · [Documentation index](README.md) · [Next: Registering functions →](registering-functions.md)
