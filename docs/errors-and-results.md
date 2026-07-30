# Errors and results

Registration and execution report ordinary C++ values instead of exposing Luau errors or throwing them through consumer code.

```cpp
if (!Result.IsSuccess()) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  std::cerr << Diagnostic->Message() << '\n';
}
```

Both `RegistrationResult` and `ExecutionResult` follow the same invariant: success has no diagnostic, while failure owns exactly one non-empty diagnostic. `Diagnostic()` therefore returns `nullptr` on success and a pointer to result-owned data on failure.

Every builder `Commit()`, `BindingRegistry::Freeze()`, `ProvideModule`, and `RegisterModule` returns a `RegistrationResult` too, so one shape covers the whole registration surface.

One deliberate exception exists. `Delegate<Signature>::operator()` throws `Luna::DelegateFailure` when the subscribed handler refuses; `Delegate::Invoke` is the value-returning form and reports a `DelegateCallResult`, while `Signal::Emit` reports a `SignalEmission` count rather than failing.

## Categories

`ErrorCategory` has seven values:

| Category | Meaning |
|---|---|
| `StateNotReady` | The State cannot accept the operation: no VM after failed creation or a move, a frozen State, a stale builder or captured scope, or a call from a thread other than the owner thread |
| `InvalidGlobalName` | A name segment failed the ASCII identifier grammar |
| `DuplicateGlobalName` | The name is already taken — an existing Luna binding, a Luna-owned member, an already declared enumerator value, or a path Luna does not own as that symbol |
| `NullCallable` | A registered function pointer was null |
| `Compilation` | Source could not be compiled or loaded |
| `Runtime` | Protected Luau execution failed, including native caller errors |
| `Internal` | Luna could not complete required bookkeeping, conversion, or VM work |

The category is a coarse channel; the message carries the specifics. A refused registration names what was refused and why — a stale builder, a frozen State, an ambiguous overload pair, an out-of-range enumerator, an unregistered base, an incoherent property policy, a module cycle, a version conflict. Equivalent inputs always produce the same message, which is what makes a refusal something you can assert on.

Compilation, runtime, and internal execution messages use stable prefixes. Luna supplies a category-specific fallback whenever an underlying layer provides no usable text, so a diagnostic message is never empty.

## Status enums

Several subsystems expose their own deterministic status enum alongside the diagnostic, each with a `…Text` function returning a stable lowercase name. They are useful when you want to branch on a reason rather than parse a message.

| Enum | Domain |
|---|---|
| `StableTypeKeyStatus` | validity of a user-defined leaf key |
| `ParameterShapeStatus` | a declared parameter shape, with the offending one-based position |
| `Luna::Detail::ConstantValueStatus` (internal) | normalization of a declared constant |
| `ConversionStatus`, `WriteStatus` | a committing read, a reservation or publication |
| `SemanticVersionStatus`, `VersionConstraintStatus`, `ModuleManifestStatus` | module metadata parsing and validation |
| `GenerationStatus` | a documentation or declaration generation attempt |
| `PublicationStatus` | an atomic artifact publication |
| `AsyncStage` | a suspended call: `Pending`, `Ready`, `Failed`, `Cancelled` |
| `DelegateStatus` | one call through a subscribed handler: `Ready`, `Released`, `ForeignThread`, `HandlerFailed`, `ResultMismatch` |

Generation and publication carry their status *and* an `ErrorDiagnostic`, through `GeneratedArtifact::Status()`/`Diagnostic()` and `ArtifactPublication::Status()`/`Diagnostic()`.

## Diagnostic precedence

When more than one problem exists, Luna reports one deterministic result rather than the first one it happened to notice. Root-scope callable registration orders its refusals as: invalid name, foreign thread, State not ready, frozen State, null callable, duplicate name.

Invocation orders its refusals by the validation sequence in [values and validation](values-and-validation.md): metadata, then the receiver for a member, then arity, then candidate selection, then arguments left to right. So a bad receiver is always reported as a receiver refusal, never as a shifted argument.

## Native exceptions

A `std::exception` thrown by a registered callable is caught at the private callback boundary. Its message and the symbol name are included in the resulting runtime diagnostic. Unknown C++ exceptions become a non-empty internal callback message. No C++ exception is allowed to cross the C ABI. A property or field's optional on-change callback is contained the same way: it runs after its write already succeeded, so a thrown exception is reported but never undoes the value already written.

The same containment applies to a module registration callback and to an allocator step: the loader or the release path contains the exception, restores the pre-attempt state, and reports it, rather than letting it escape through a garbage collector or a State destructor.

Diagnostics own their strings, so they remain inspectable after the operation returns. Results are copyable and movable without changing the success/diagnostic relationship.

---

[← Previous: Executing Luau](executing-luau.md) · [Documentation index](README.md) · [Next: Architecture →](architecture.md)
