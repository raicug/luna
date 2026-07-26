// Integration coverage of callable richness through the real Luau compiler and
// virtual machine.
//
// What the earlier cases cover one at a time - overload sets, declared
// parameter shapes, and return packs - is exercised here together, at a nested
// qualified name, and against injected internal failures:
//
//   * One nested overload set whose candidates declare fixed, optional,
//     defaulted, and variadic parameters, plus one nested set whose candidates
//     publish zero, one, and many return values.
//   * The read, write, and target failures of one multi-candidate call: the
//     selected candidate is the only one that ever commits anything, a refused
//     call publishes zero values, the callback checkpoint is restored exactly,
//     and the State stays reusable afterwards.
//   * Deterministic diagnostic ordering: the no-match diagnostic of one call
//     renders its candidates in canonical order, so two States that registered
//     the same candidates in different orders report one identical message.
//
// Every call is driven from script source, so the whole path is under test:
// registration, canonical grouping, side-effect-free probing, Pareto selection,
// declared-shape binding, atomic publication, and error recovery.

// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "callable richness integration check failed: " << Description
            << '\n';
}

// -- the nested model under test --------------------------------------------

int DoubledCalls = 0;
int ProductCalls = 0;
int TextLengthCalls = 0;
int TailCalls = 0;
int PadCalls = 0;
int OffsetCalls = 0;
int PairCalls = 0;
int VoidCalls = 0;
int RepeatCalls = 0;
int DescribeCalls = 0;
int ExplodeCalls = 0;
int BlendLeftCalls = 0;
int BlendRightCalls = 0;

void ResetCalls() {
  DoubledCalls = 0;
  ProductCalls = 0;
  TextLengthCalls = 0;
  TailCalls = 0;
  PadCalls = 0;
  OffsetCalls = 0;
  PairCalls = 0;
  VoidCalls = 0;
  RepeatCalls = 0;
  DescribeCalls = 0;
  ExplodeCalls = 0;
  BlendLeftCalls = 0;
  BlendRightCalls = 0;
}

// `Studio.Math.Combine`: one nested overload set of four candidates whose
// shapes differ in arity, in parameter type, and in the variadic tail.
[[nodiscard]] int Doubled(int Value) {
  ++DoubledCalls;
  return Value * 2;
}

[[nodiscard]] int Product(int Left, int Right) {
  ++ProductCalls;
  return Left * Right;
}

[[nodiscard]] int TextLength(std::string Value) {
  ++TextLengthCalls;
  return static_cast<int>(Value.size());
}

[[nodiscard]] int TailCount(Luna::ArgumentView Arguments) {
  ++TailCalls;
  return static_cast<int>(Arguments.Size());
}

// `Studio.Text`: the omittable shapes at a nested path.
[[nodiscard]] std::string Pad(std::string Text, std::optional<int> Width) {
  ++PadCalls;
  const std::size_t Target =
      Width && *Width > 0 ? static_cast<std::size_t>(*Width) : Text.size();
  while (Text.size() < Target)
    Text.push_back(' ');
  return Text;
}

[[nodiscard]] int Offset(int Value, int Amount) {
  ++OffsetCalls;
  return Value + Amount;
}

// `Studio.Math.Emit`: one nested overload set whose candidates publish zero,
// one, and many return values, so every publication path is reachable through
// one resolved call.
[[nodiscard]] std::pair<int, int> Divide(int Left, int Right) {
  ++PairCalls;
  if (Right == 0)
    return {0, 0};
  return {Left / Right, Left % Right};
}

void Clear(int Value) {
  ++VoidCalls;
  static_cast<void>(Value);
}

[[nodiscard]] Luna::ReturnPack Repeat(std::string Text, int Count) {
  ++RepeatCalls;
  Luna::ReturnPack Pack;
  for (int Index = 0; Index < Count; ++Index)
    Pack.AppendText(Text);
  return Pack;
}

[[nodiscard]] std::tuple<bool, double, std::string> Describe(std::string Text) {
  ++DescribeCalls;
  return {!Text.empty(), static_cast<double>(Text.size()) / 2.0, Text};
}

[[nodiscard]] int Explode(bool Flag) {
  ++ExplodeCalls;
  if (Flag)
    throw std::runtime_error("nested candidate failure");
  return 0;
}

// `Studio.Math.Blend`: two candidates whose rank sequences can never dominate
// each other, because each one is exact in the position the other converts.
[[nodiscard]] int BlendLeft(int Left, double Right) {
  ++BlendLeftCalls;
  return Left + static_cast<int>(Right);
}

[[nodiscard]] int BlendRight(double Left, int Right) {
  ++BlendRightCalls;
  return static_cast<int>(Left) + Right;
}

// One plan publishes the whole nested model, so every candidate below belongs
// to a namespace the same transaction installed.
[[nodiscard]] bool RegisterNestedModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::NamespaceBuilder Math = Studio.RegisterNamespace("Math");
  static_cast<void>(Math.RegisterFunction("Combine", &Doubled)
                        .RegisterFunction("Combine", &Product)
                        .RegisterFunction("Combine", &TextLength)
                        .RegisterFunction("Combine", &TailCount));
  static_cast<void>(Math.RegisterFunction("Emit", &Divide)
                        .RegisterFunction("Emit", &Clear)
                        .RegisterFunction("Emit", &Repeat)
                        .RegisterFunction("Emit", &Describe)
                        .RegisterFunction("Emit", &Explode));
  static_cast<void>(Math.RegisterFunction("Blend", &BlendLeft)
                        .RegisterFunction("Blend", &BlendRight));

  Luna::NamespaceBuilder Text = Studio.RegisterNamespace("Text");
  static_cast<void>(
      Text.RegisterFunction("Pad", &Pad)
          .RegisterFunction("Offset", Luna::WithDefaults(&Offset, 5)));

  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "callable richness source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Failure(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

// The callback checkpoint a refused call restores exactly: the stack returns to
// its entry depth and carries only the one error value the failure reports,
// which is also what "published zero return values" means at this boundary.
[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

// -- nested overload sets with rich shapes ----------------------------------

void CheckNestedOverloadSetsResolveRichShapes() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterNestedModel(Owner),
        "one plan publishes the whole nested rich model");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Hooks::OverloadCandidateCount(Owner, "Studio.Math.Combine") == 4 &&
            Hooks::OverloadCandidateCount(Owner, "Studio.Math.Emit") == 5,
        "every nested candidate joins the overload set of its qualified name");

  Check(Succeeds(Owner,
                 "assert(Studio.Math.Combine(5) == 10, 'fixed one')\n"
                 "assert(Studio.Math.Combine(2, 3) == 6, 'fixed two')\n"
                 "assert(Studio.Math.Combine('abc') == 3, 'string')\n"
                 "assert(Studio.Math.Combine(1, 2, 3) == 3, 'variadic')\n"
                 "assert(Studio.Math.Combine() == 0, 'empty variadic')\n"),
        "a nested overload set selects the dominating candidate per call");
  Check(DoubledCalls == 1 && ProductCalls == 1 && TextLengthCalls == 1 &&
            TailCalls == 2,
        "each nested call invokes exactly the selected candidate, once");

  Check(Succeeds(Owner,
                 "assert(Studio.Text.Pad('x') == 'x', 'omitted optional')\n"
                 "assert(Studio.Text.Pad('x', nil) == 'x', 'explicit nil')\n"
                 "assert(Studio.Text.Pad('x', 3) == 'x  ', 'present')\n"
                 "assert(Studio.Text.Offset(1) == 6, 'omitted default')\n"
                 "assert(Studio.Text.Offset(1, 2) == 3, 'supplied')\n"),
        "nested optional and defaulted shapes keep their declared behavior");
  Check(PadCalls == 3 && OffsetCalls == 2,
        "each nested declared-shape call invokes its target exactly once");

  Check(Succeeds(Owner,
                 "local Quotient, Remainder = Studio.Math.Emit(7, 2)\n"
                 "assert(Quotient == 3 and Remainder == 1, 'pair')\n"
                 "assert(select('#', Studio.Math.Emit(4)) == 0, 'void')\n"
                 "assert(select('#', Studio.Math.Emit('a', 3)) == 3, 'pack')\n"
                 "local Flag, Half, Text = Studio.Math.Emit('ab')\n"
                 "assert(Flag == true and Half == 1.0 and Text == 'ab')\n"),
        "one nested set publishes zero, one, and many values per candidate");
  Check(PairCalls == 1 && VoidCalls == 1 && RepeatCalls == 1 &&
            DescribeCalls == 1 && ExplodeCalls == 0,
        "each return shape is produced by exactly its own candidate");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every resolved nested call restores the exact root stack depth");
}

// -- canonical diagnostic ordering ------------------------------------------

void CheckNoMatchDiagnosticsAreOrderedCanonically() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterNestedModel(Owner),
        "the nested model registers before the refusal matrix");

  const std::string NoMatch = Failure(Owner, "return Studio.Math.Combine({})");
  Check(Contains(NoMatch, "Studio.Math.Combine") &&
            Contains(NoMatch, "no overload accepts those arguments"),
        "a nested no-match diagnostic names the qualified name and the call");
  Check(Contains(NoMatch, "received (table)"),
        "a nested no-match diagnostic summarizes the received arguments");
  Check(Contains(NoMatch, "...") &&
            Contains(NoMatch, "expects 2 arguments but received 1"),
        "every candidate contributes its own first rejection");
  Check(DoubledCalls == 0 && ProductCalls == 0 && TextLengthCalls == 0 &&
            TailCalls == 0,
        "a refused resolution invokes no nested candidate at all");
  Check(RestoredCheckpoint(Owner),
        "a refused nested resolution restores the callback checkpoint");

  // The same candidates declared in another order report one identical
  // message: candidate order is canonical, never registration order.
  Luna::State Permuted;
  Luna::BindingRegistry Registry = Permuted.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Math = Studio.RegisterNamespace("Math");
  static_cast<void>(Math.RegisterFunction("Combine", &TailCount)
                        .RegisterFunction("Combine", &TextLength)
                        .RegisterFunction("Combine", &Product)
                        .RegisterFunction("Combine", &Doubled));
  Check(Studio.Commit().IsSuccess(),
        "the permuted registration order publishes the same candidates");
  Check(Failure(Permuted, "return Studio.Math.Combine({})") == NoMatch,
        "the no-match diagnostic never depends on registration order");
}

// -- incomparable candidates at a nested path -------------------------------

void CheckNestedAmbiguityCommitsNothing() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterNestedModel(Owner),
        "the nested model registers before the ambiguity matrix");

  const std::string Ambiguous =
      Failure(Owner, "return Studio.Math.Blend(1, 2)");
  Check(Contains(Ambiguous, "Studio.Math.Blend") &&
            Contains(Ambiguous, "ambiguous"),
        "an incomparable frontier fails instead of guessing a candidate");
  Check(Contains(Ambiguous, "(signed 32-bit integer, number)") &&
            Contains(Ambiguous, "(number, signed 32-bit integer)"),
        "the ambiguity names every non-dominated candidate signature");
  Check(BlendLeftCalls == 0 && BlendRightCalls == 0,
        "an ambiguous nested call invokes no candidate at all");
  Check(RestoredCheckpoint(Owner),
        "an ambiguous nested call restores the callback checkpoint");

  // Each candidate still wins the calls it alone accepts exactly.
  Check(Succeeds(Owner,
                 "assert(Studio.Math.Blend(1, 2.5) == 3, 'left exact')\n"
                 "assert(Studio.Math.Blend(1.5, 2) == 3, 'right exact')\n"),
        "an unambiguous nested call still resolves after an ambiguous one");
  Check(BlendLeftCalls == 1 && BlendRightCalls == 1,
        "each unambiguous call invokes exactly its own candidate, once");
}

// -- injected read, write, and target failures ------------------------------

void CheckInjectedFailuresDuringMultiCandidateCalls() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterNestedModel(Owner),
        "the nested model registers before the fault matrix");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  // Probing commits nothing, so it never reaches the committing reader: one
  // injected read fault survives resolution of a multi-candidate call and is
  // consumed by the selected candidate's own conversion instead.
  Hooks::InjectFault(Owner, FaultPoint::ArgumentInspection);
  const std::string FixedRead =
      Failure(Owner, "return Studio.Math.Combine(2, 3)");
  Check(Contains(FixedRead, "Internal error while inspecting") &&
            Contains(FixedRead, "Studio.Math.Combine") &&
            Contains(FixedRead, "argument 1"),
        "an injected read failure of the selected candidate is internal");
  Check(Hooks::PendingFaults(Owner, FaultPoint::ArgumentInspection) == 0,
        "resolution consumes no read fault; the selected conversion does");
  Check(ProductCalls == 0 && TailCalls == 0,
        "an injected read failure invokes no candidate at all");
  Check(RestoredCheckpoint(Owner),
        "an injected read failure restores the callback checkpoint");

  // The same fault while the selected candidate is the variadic one, whose
  // first call position is the one the failure names.
  Hooks::InjectFault(Owner, FaultPoint::ArgumentInspection);
  const std::string VariadicRead =
      Failure(Owner, "return Studio.Math.Combine(1, 2, 3)");
  Check(Contains(VariadicRead, "Internal error while inspecting") &&
            Contains(VariadicRead, "argument 1"),
        "an injected variadic read failure names its first call position");
  Check(TailCalls == 0, "an injected variadic read failure invokes no target");

  // A publication that cannot reserve its resources refuses after the selected
  // target ran exactly once, and publishes zero values.
  Hooks::InjectFault(Owner, FaultPoint::ReturnStackCapacity);
  const std::string Unreserved =
      Failure(Owner, "return Studio.Math.Emit(7, 2)");
  Check(Contains(Unreserved, "Internal error for callable 'Studio.Math.Emit'"),
        "a refused pack publication is reported against the nested callable");
  Check(PairCalls == 1,
        "the selected multiple-return candidate ran exactly once");
  Check(RestoredCheckpoint(Owner),
        "a refused pack publication restores the callback checkpoint");

  // An injected write failure of a dynamic pack: the same atomicity applies to
  // a shape whose element count the invocation decided.
  Hooks::InjectFault(Owner, FaultPoint::ReturnWrite);
  const std::string Written = Failure(Owner, "return Studio.Math.Emit('a', 3)");
  Check(Contains(Written, "Injected internal return-writer failure") &&
            Contains(Written, "Studio.Math.Emit"),
        "an injected write failure publishes nothing and names the callable");
  Check(RepeatCalls == 1, "the selected dynamic-pack candidate ran once");
  Check(RestoredCheckpoint(Owner),
        "an injected write failure restores the callback checkpoint");

  // The zero-value candidate of the same set fails its finalization the same
  // deterministic way.
  Hooks::InjectFault(Owner, FaultPoint::VoidFinalization);
  const std::string Finalized = Failure(Owner, "return Studio.Math.Emit(4)");
  Check(Contains(Finalized, "Injected internal void-finalization failure") &&
            Contains(Finalized, "Studio.Math.Emit"),
        "an injected finalization failure is reported for the void candidate");
  Check(VoidCalls == 1, "the selected zero-return candidate ran exactly once");

  // A selected candidate that throws keeps the foundation's translation, and
  // no other candidate of the set ever ran.
  const std::string Thrown = Failure(Owner, "return Studio.Math.Emit(true)");
  Check(Contains(Thrown, "Runtime error:") &&
            Contains(Thrown, "callable 'Studio.Math.Emit'") &&
            Contains(Thrown, "threw: nested candidate failure"),
        "a throwing selected candidate keeps the foundation's translation");
  Check(ExplodeCalls == 1 && PairCalls == 1 && RepeatCalls == 1 &&
            VoidCalls == 1 && DescribeCalls == 0,
        "a throwing candidate never runs another candidate of its set");
  Check(RestoredCheckpoint(Owner),
        "a translated exception restores the callback checkpoint");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every injected failure restores the exact root stack depth");

  // The State keeps resolving, converting, and publishing afterwards.
  Check(Succeeds(Owner,
                 "assert(Studio.Math.Combine(2, 3) == 6, 'fixed')\n"
                 "assert(Studio.Math.Combine(1, 2, 3) == 3, 'variadic')\n"
                 "local Quotient, Remainder = Studio.Math.Emit(9, 4)\n"
                 "assert(Quotient == 2 and Remainder == 1, 'pair')\n"
                 "assert(select('#', Studio.Math.Emit('b', 2)) == 2, 'pack')\n"
                 "assert(select('#', Studio.Math.Emit(1)) == 0, 'void')\n"
                 "assert(Studio.Text.Offset(1) == 6, 'default')\n"),
        "the State stays reusable after every injected failure");
  Check(ProductCalls == 1 && TailCalls == 1 && PairCalls == 2 &&
            RepeatCalls == 2 && VoidCalls == 2 && OffsetCalls == 1,
        "each recovered call invokes its selected candidate exactly once");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "recovered calls restore the exact root stack depth");
}

} // namespace

int RunCallableRichnessIntegrationTests() {
  FailureCount = 0;
  CheckNestedOverloadSetsResolveRichShapes();
  CheckNoMatchDiagnosticsAreOrderedCanonically();
  CheckNestedAmbiguityCommitsNothing();
  CheckInjectedFailuresDuringMultiCandidateCalls();
  return FailureCount == 0 ? 0 : 1;
}
