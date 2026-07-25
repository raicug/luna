# Values and validation

The first Luna milestone supports four value types. Parameters are passed by value, and returns may use the same types or `void`.

| C++ type | Luau value | Notes |
|---|---|---|
| `bool` | boolean | No truthiness conversion |
| `int` | number | Must be a signed 32-bit integral value |
| `double` | number | Finite values, signed zero, infinities, and NaN are accepted |
| `std::string` | string | Byte-preserving, including embedded NUL bytes |
| `void` | no return | Return type only |

Luna does not coerce strings to numbers, numbers to booleans, or other near matches. The Luau type must agree with the registered C++ type.

## Validation order

Invocation checks are deliberately predictable:

1. required callable metadata must exist and be consistent
2. argument count must match
3. arguments are inspected from left to right
4. type compatibility is checked before value-specific rules
5. validation stops permanently at the first failure

An `int` argument is checked for numeric type, finiteness, the inclusive range `[-2147483648, 2147483647]`, and then integrality. This order determines which diagnostic is returned.

Strings use length-aware Luau APIs. Bytes are copied exactly, including zero bytes, with a maximum length of 1,048,576 bytes in either direction. Oversized arguments are rejected before user code runs; oversized returns become internal return-conversion failures.

## Invocation and returns

The callable runs exactly once, and only after every argument validates. A supported value return produces exactly one Luau result. `void` produces none. Multiple return values are not supported yet.

Validation failures never invoke the callable. Return-writing failures restore the callback stack and expose no partial result.

---

[← Previous: Registering functions](registering-functions.md) · [Documentation index](README.md) · [Next: Executing Luau →](executing-luau.md)
