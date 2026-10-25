# Testing and contributing

## Running the checks

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

| CTest entry | What it runs |
| --- | --- |
| `luna_tests` | `tests/tests.cpp`: every behaviour in these docs, from the public API |
| `luna_boundary` | `tests/boundary.cpp`: each public header compiles alone, and a consumer never needs a Luau type |
| `luna_demo` | `LunaDemoSelfTest`: the demo's harness with no window and no GPU |

Run `LunaBench` from a Release build for the performance gates (see [Performance](performance.md)).

### The test file

`tests.cpp` is a plain program. `Check(condition, "what")` records a case and prints a line only on failure; the program prints `N checks, M failures` and exits non-zero on any failure. Output is unbuffered so the last line survives a crash.

Each test is a function that builds a `Fixture` (one `State`) and exercises one area:

| Test | Area |
| --- | --- |
| `TestFunctions`, `TestValues`, `TestContainers` | Callables, values, conversion |
| `TestNamespaces`, `TestEnumObjects`, `TestAliases` | Hierarchy and enums |
| `TestClasses`, `TestOwnership`, `TestBaseClasses`, `TestIndexFallback`, `TestIteration` | Classes |
| `TestOverloads`, `TestOverloadsAreStrict` | Overload selection |
| `TestSuspension`, `TestScheduler`, `TestChunks`, `TestChunkYields`, `TestNestedSchedulerEntry` | Tasks |
| `TestSignals`, `TestSignalWaitValues`, `TestHandlerOutlivesTask` | Events and delegates |
| `TestInterrupt`, `TestInterruptWithoutBudget`, `TestLimits` | Limits |
| `TestValidation`, `TestFailedCommitLeavesNothing`, `TestRegistrationNeverThrows` | Registration failure |
| `TestBindingsAreLocked`, `TestUnregisteredClassRefused`, `TestNameBelongsToOneType` | Safety |
| `TestTaskFailuresAreReported`, `TestOutOfMemoryAtTaskCreation` | Error channels |
| `TestStateMovesAndGoes`, `TestStateLifetime`, `TestSymbolsAreSafeToRead` | Lifetime and threads |

When you fix a bug, add a test that fails without the fix. Several tests here are the minimal reproduction of a defect that was found by reading the code and confirmed by running it.

### The demo

`LunaDemo` is an interactive ImGui host: an editor, a scene of bound `Shape` objects, a scheduler panel, a halting panel and a symbol browser. Every edit in the scene is posted through the VM as a property write, so the write hook fires exactly as it does for a script. `LunaDemoSelfTest` runs the same harness headless, so the demo's behaviour is covered by CTest.

## Code style

The repository ships a `.clang-format`: LLVM style, 2-space indent, 80 columns, includes left in the order written.

Find `clang-format` on the path, or in the Visual Studio LLVM component at `...\VC\Tools\Llvm\x64\bin\clang-format.exe`, and run it over the source:

```powershell
clang-format -i include/luna/*.hpp src/*.cpp src/*.hpp tests/*.cpp app/src/*.cpp bench/*.cpp demo/src/*.cpp demo/src/*.hpp
```

Conventions the code already follows:

- **Names.** Types, functions and members are `PascalCase`; locals and parameters in new code are `camelCase` or `PascalCase` to match the file they are in. Constants that are compile-time are `kPascalCase`.
- **Comments say why.** A comment explains a constraint, an invariant or a decision that the code cannot show. It does not restate the line below it.
- **No Luau type in a public declaration.** The one exception is `lua_State *` inside codec templates.
- **No exceptions across the VM.** Catch at the boundary and raise a Luau error.
- **Fail loudly at compile time** with a `static_assert` that says what to do, rather than with a template error.

## Making a change

1. **Write the failing test first** in `tests/tests.cpp`, in the area it belongs to, and add the call to `main`.
2. **Keep registration atomic.** Anything that writes a VM table or the model during a commit must go through `RememberSlot` or another logged mutator, so a rollback restores it.
3. **Mind re-entrancy.** A callable can start a task or run source. Do not hold a reference into `Runtime::Tasks` across a resume, and reclaim entries only through `CompactTasks`.
4. **Keep the boundary.** Add a header to `tests/boundary.cpp` if you add one.
5. **Update these docs** when a behaviour, a message or an option changes.

## Debugging a crash

A test binary that dies with `0x80000003` has hit a debug-heap check or an assert. Running `tests.cpp` unbuffered shows the last case printed. Luau's own `api_check`s are on in a Debug build, so pushing to a full stack from a hook fails loudly there: call `lua_checkstack` first.

## Layout of the repository

```text
include/luna/     public headers
src/              private implementation
app/src/          smallest consumer
bench/            performance gates
demo/             ImGui host and headless self-test
tests/            behaviour and boundary tests
cmake/            Luau and ImGui dependency setup
docs/             these pages
```

---

[← Previous: Architecture](architecture.md) · [Documentation index](README.md) · [Project README](../README.md)
