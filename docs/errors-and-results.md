# Errors and results

Registration and execution report ordinary C++ values instead of exposing Luau errors or throwing them through consumer code.

```cpp
if (!Result.IsSuccess()) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  std::cerr << Diagnostic->Message() << '\n';
}
```

Both `RegistrationResult` and `ExecutionResult` follow the same invariant: success has no diagnostic, while failure owns exactly one non-empty diagnostic. `Diagnostic()` therefore returns `nullptr` on success and a pointer to result-owned data on failure.

## Categories

| Category | Meaning |
|---|---|
| `StateNotReady` | The State has no VM, usually after failed creation or a move |
| `InvalidGlobalName` | A registration name failed the ASCII identifier grammar |
| `DuplicateGlobalName` | The State already has a Luna binding with that name |
| `NullCallable` | A registered function pointer was null |
| `Compilation` | Source could not be compiled or loaded |
| `Runtime` | Protected Luau execution failed, including native caller errors |
| `Internal` | Luna could not complete required bookkeeping, conversion, or VM work |

Compilation, runtime, and internal execution messages use stable prefixes. Luna supplies a category-specific fallback whenever an underlying layer provides no usable text.

## Native exceptions

A `std::exception` thrown by a registered callable is caught at the private callback boundary. Its message and the global name are included in the resulting runtime diagnostic. Unknown C++ exceptions become a non-empty internal callback message. No C++ exception is allowed to cross the C ABI.

Diagnostics own their strings, so they remain inspectable after the operation returns. Results are copyable and movable without changing the success/diagnostic relationship.

---

[← Previous: Executing Luau](executing-luau.md) · [Documentation index](README.md) · [Next: Architecture →](architecture.md)
