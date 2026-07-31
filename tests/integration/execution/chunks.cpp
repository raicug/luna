// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "chunk check failed: " << Description << '\n';
}

Luna::State *Host = nullptr;

int NestedCalls = 0;
int DeepestNesting = 0;
int CurrentNesting = 0;

[[nodiscard]] Luna::Chunk LoadString(std::string Source) {
  return Host ? Host->Load(Source, "loadstring") : Luna::Chunk();
}

[[nodiscard]] double RunNested(std::string Source, double Argument) {
  if (!Host)
    return -1.0;

  ++NestedCalls;
  ++CurrentNesting;
  if (CurrentNesting > DeepestNesting)
    DeepestNesting = CurrentNesting;

  const Luna::Chunk Loaded = Host->Load(Source, "nested");
  Luna::ValuePack Arguments;
  Arguments.Append(Luna::OwnedValue::Number(Argument));

  const Luna::ChunkResult Produced = Loaded.Invoke(Arguments);
  --CurrentNesting;

  if (!Produced.IsSuccess() || Produced.Size() != 1)
    return -1.0;
  const std::optional<double> Value = Produced.At(0).ToNumber();
  return Value ? *Value : -1.0;
}

[[nodiscard]] std::string RunNestedDiagnostic(std::string Source) {
  if (!Host)
    return "no host";
  const Luna::Chunk Loaded = Host->Load(Source, "nested");
  const Luna::ChunkResult Produced = Loaded.Invoke();
  if (Produced.IsSuccess())
    return "delivered";
  const Luna::ErrorDiagnostic *Diagnostic = Produced.Diagnostic();
  return Diagnostic != nullptr ? Diagnostic->Message() : std::string("refused");
}

std::string DeepestRefusal;

[[nodiscard]] double Recurse(double Depth) {
  if (!Host || Depth <= 0.0)
    return 0.0;
  const Luna::Chunk Loaded =
      Host->Load("local Depth = ... \nreturn Recurse(Depth - 1) + 1", "deep");
  Luna::ValuePack Arguments;
  Arguments.Append(Luna::OwnedValue::Number(Depth));
  const Luna::ChunkResult Produced = Loaded.Invoke(Arguments);
  if (!Produced.IsSuccess() || Produced.Size() != 1) {
    if (DeepestRefusal.empty() && Produced.Diagnostic() != nullptr)
      DeepestRefusal = Produced.Diagnostic()->Message();
    return -1.0;
  }
  const std::optional<double> Value = Produced.At(0).ToNumber();
  return Value ? *Value : -1.0;
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "chunk source failed: " << Diagnostic->Message() << '\n';
  return false;
}

void CheckLoadReportsCompilationFailure() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  const Luna::Chunk Bad = Owner.Load("local =");
  Check(!Bad.IsLoaded(), "a chunk that does not compile is not loaded");
  Check(Bad.Diagnostic() != nullptr &&
            Bad.Diagnostic()->Category() == Luna::ErrorCategory::Compilation,
        "a compile failure is reported through the usual result type");

  const Luna::ChunkResult Refused = Bad.Invoke();
  Check(!Refused.IsSuccess() && Refused.Diagnostic() != nullptr &&
            Refused.Diagnostic()->Category() ==
                Luna::ErrorCategory::Compilation,
        "invoking a refused chunk repeats its compile diagnostic and runs "
        "nothing");

  const Luna::Chunk Good = Owner.Load("return 1");
  Check(Good.IsLoaded() && Good.Diagnostic() == nullptr,
        "a chunk that compiles owns validated bytecode");
  Check(!Good.Bytecode().empty(), "the chunk owns its bytecode");
  Check(Good.Name() == "LunaChunk", "a chunk carries its declared name");
}

void CheckChunkInvocationCarriesArgumentsAndResults() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const Luna::Chunk Sum =
      Owner.Load("local Left, Right = ... \nreturn Left + Right, 'ok'", "sum");
  Check(Sum.IsLoaded(), "the chunk loads");

  Luna::ValuePack Arguments;
  Arguments.Append(Luna::OwnedValue::Number(19.0));
  Arguments.Append(Luna::OwnedValue::Number(23.0));

  const Luna::ChunkResult Produced = Sum.Invoke(Arguments);
  Check(Produced.IsSuccess(), "the chunk delivers");
  Check(Produced.Size() == 2, "every value the chunk returned is reported");
  Check(Produced.At(0).ToNumber().value_or(0.0) == 42.0,
        "an argument reaches the chunk and its result comes back");
  Check(Produced.At(1).ToText().value_or(std::string()) == "ok",
        "a second returned value keeps its position");

  const Luna::Chunk Table =
      Owner.Load("return { alpha = 1, beta = { 2, 3 } }", "table");
  const Luna::ChunkResult Structured = Table.Invoke();
  Check(Structured.IsSuccess() && Structured.Size() == 1,
        "a chunk returning a table delivers one value");
  Check(Structured.At(0).Field("alpha").ToNumber().value_or(0.0) == 1.0 &&
            Structured.At(0).Field("beta").Element(1).ToNumber().value_or(
                0.0) == 3.0,
        "a returned table is reported in the shape an owned value uses");

  const Luna::Chunk Failing = Owner.Load("error('nope')", "failing");
  const Luna::ChunkResult Failed = Failing.Invoke();
  Check(!Failed.IsSuccess() && Failed.Diagnostic() != nullptr &&
            Failed.Diagnostic()->Category() == Luna::ErrorCategory::Runtime,
        "a chunk that raises reports a runtime failure rather than throwing");

  Check(Succeeds(Owner, "assert(true)"),
        "the State stays reusable after a failed chunk");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every chunk invocation restores the entry stack depth");
}

void CheckChunkRunsFromInsideANativeCall() {
  Luna::State Owner;
  Host = &Owner;
  NestedCalls = 0;
  DeepestNesting = 0;
  CurrentNesting = 0;

  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(
      Registry.RegisterFunction("RunNested", &RunNested).IsSuccess() &&
          Registry.RegisterFunction("RunNestedDiagnostic", &RunNestedDiagnostic)
              .IsSuccess(),
      "the nested runners register");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local Doubled = RunNested('local V = ... \\n"
                        "return V * 2', 21)\n"
                        "assert(Doubled == 42)"),
        "a chunk runs from inside a native call reached by the outer chunk");
  Check(NestedCalls == 1, "the native call ran once");

  Check(Succeeds(Owner, "Marker = 5\n"
                        "local Seen = RunNested('return Marker', 0)\n"
                        "assert(Seen == 5)"),
        "a nested chunk shares the State's globals with the calling chunk");

  Check(Succeeds(Owner,
                 "local Reported = RunNestedDiagnostic(\"error('deep')\")\n"
                 "assert(type(Reported) == 'string')\n"
                 "assert(string.find(Reported, 'deep') ~= nil)"),
        "a nested chunk that errors reports the failure to the host rather "
        "than unwinding the calling chunk");

  Check(Succeeds(Owner,
                 "local Ok, Message = pcall(function()\n"
                 "  local Reported = RunNestedDiagnostic(\"error('x')\")\n"
                 "  error('outer: ' .. Reported)\n"
                 "end)\n"
                 "assert(not Ok)\n"
                 "assert(string.find(Message, 'outer:') ~= nil)"),
        "the calling chunk keeps deciding what to do with a nested failure");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "nested chunk invocation restores the entry stack depth");
  Host = nullptr;
}

void CheckNestingLimitIsRefusedDeterministically() {
  Luna::State Owner;
  Host = &Owner;
  DeepestRefusal.clear();

  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("Recurse", &Recurse).IsSuccess(),
        "the recursive runner registers");

  const Luna::Chunk Enter =
      Owner.Load("local Depth = ... \nreturn Recurse(Depth)", "enter");

  Luna::ValuePack Shallow;
  Shallow.Append(Luna::OwnedValue::Number(4.0));
  const Luna::ChunkResult Nested = Enter.Invoke(Shallow);
  Check(Nested.IsSuccess() && Nested.At(0).ToNumber().value_or(-1.0) == 4.0,
        "nesting within the limit completes");
  Check(DeepestRefusal.empty(), "nesting within the limit refuses nothing");

  Luna::ValuePack Deep;
  Deep.Append(Luna::OwnedValue::Number(64.0));
  const Luna::ChunkResult Bounded = Enter.Invoke(Deep);
  Check(Bounded.IsSuccess(),
        "the call that started the recursion still settles rather than "
        "exhausting the host stack");
  Check(DeepestRefusal.find("nests deeper") != std::string::npos,
        "the nested call past the limit is refused deterministically, and the "
        "refusal names the nesting limit");

  const Luna::Chunk Again = Owner.Load("return 7", "again");
  const Luna::ChunkResult Recovered = Again.Invoke();
  Check(Recovered.IsSuccess() &&
            Recovered.At(0).ToNumber().value_or(0.0) == 7.0,
        "the State stays usable after the nesting limit refused a call");
  Host = nullptr;
}

void CheckChunkPublishesAsALuauFunction() {
  Luna::State Owner;
  Host = &Owner;

  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("loadstring", &LoadString).IsSuccess(),
        "a function returning a chunk registers");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local F = loadstring('local A, B = ... \\n"
                        "return A + B')\n"
                        "assert(type(F) == 'function')\n"
                        "assert(F(1, 2) == 3)\n"
                        "assert(F(10, 20) == 30)"),
        "a chunk returned to a script is a Luau function the script calls "
        "directly, more than once");

  Check(Succeeds(Owner, "Shared = 9\n"
                        "local F = loadstring('return Shared')\n"
                        "assert(F() == 9)"),
        "a published chunk sees the State's globals");

  Check(Succeeds(Owner, "local F = loadstring(\"error('inside')\")\n"
                        "local Ok, Message = pcall(F)\n"
                        "assert(not Ok)\n"
                        "assert(string.find(Message, 'inside') ~= nil)"),
        "a published chunk that errors is observed by the caller's pcall as "
        "an ordinary error");

  Check(Succeeds(Owner, "local F = loadstring('return 1, 2, 3')\n"
                        "local A, B, C = F()\n"
                        "assert(A == 1 and B == 2 and C == 3)"),
        "a published chunk publishes as many values as it returns");

  const Luna::ExecutionResult Refused =
      Owner.Execute("local F = loadstring('local =')");
  Check(!Refused.IsSuccess(),
        "a source the compiler rejects refuses the call that produced it "
        "rather than handing back a broken function");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing a chunk restores the entry stack depth");
  Host = nullptr;
}

void CheckInterruptStopsAChunk() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Owner.RequestInterrupt("stop the chunk");
  const Luna::Chunk Endless = Owner.Load("while true do end", "endless");
  const Luna::ChunkResult Stopped = Endless.Invoke();
  Check(Stopped.IsInterrupted(),
        "an interrupt stops an invoked chunk and reports it as interrupted");

  Owner.ClearInterrupt();
  const Luna::Chunk Quick = Owner.Load("return 3", "quick");
  const Luna::ChunkResult Fine = Quick.Invoke();
  Check(Fine.IsSuccess() && Fine.At(0).ToNumber().value_or(0.0) == 3.0,
        "the State stays usable once the interrupt is cleared");
}

} // namespace

int RunChunkTests();

int RunChunkTests() {
  FailureCount = 0;
  CheckLoadReportsCompilationFailure();
  CheckChunkInvocationCarriesArgumentsAndResults();
  CheckChunkRunsFromInsideANativeCall();
  CheckNestingLimitIsRefusedDeterministically();
  CheckChunkPublishesAsALuauFunction();
  CheckInterruptStopsAChunk();
  return FailureCount == 0 ? 0 : 1;
}
