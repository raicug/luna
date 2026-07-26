// Integration coverage of the canonical conversion registry through the real
// Luau compiler and virtual machine.
//
// Every read and write here goes through a real registered callable and real
// Luau tables: arguments are indexed out of script-created tables, returns are
// stored back into script-created tables, and each failure family is driven
// from script source rather than through a private conversion hook. After every
// attempt the same invariants are checked: one deterministic diagnostic, the
// exact root stack depth, the exact callback checkpoint (so a failure exposes
// one error value and zero return values), the native target invoked at most
// once, and a State that still converts, registers, and executes afterwards.
//
// Structural parameters and returns - sequences, maps, tuples, packs - are not
// reachable from a registered callable yet: the trampoline still validates
// through the pinned `ValueKind` callable metadata, and richer signatures
// arrive with the parameter and return milestones (tasks 9.4 and 9.5). Their
// reads and writes are exercised against real Luau tables through the private
// structural hooks in the focused suites until then.

// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "conversion registry integration check failed: " << Description
            << '\n';
}

int EchoCalls = 0;
int SumCalls = 0;
int ScaleCalls = 0;
int NegateCalls = 0;
int SilentCalls = 0;

[[nodiscard]] std::string Echo(std::string Text) {
  ++EchoCalls;
  return Text;
}

[[nodiscard]] int Sum(int Left, int Right) {
  ++SumCalls;
  return Left + Right;
}

[[nodiscard]] double Scale(double Value) {
  ++ScaleCalls;
  return Value * 2.0;
}

[[nodiscard]] bool Negate(bool Flag) {
  ++NegateCalls;
  return !Flag;
}

void Silent() { ++SilentCalls; }

void ResetCalls() {
  EchoCalls = 0;
  SumCalls = 0;
  ScaleCalls = 0;
  NegateCalls = 0;
  SilentCalls = 0;
}

[[nodiscard]] bool RegisterConverters(Luna::State &Owner) {
  return Owner.Bindings().Register("Echo", &Echo).IsSuccess() &&
         Owner.Bindings().Register("Sum", &Sum).IsSuccess() &&
         Owner.Bindings().Register("Scale", &Scale).IsSuccess() &&
         Owner.Bindings().Register("Negate", &Negate).IsSuccess() &&
         Owner.Bindings().Register("Silent", &Silent).IsSuccess();
}

// One representative pass that reads every argument out of a real Luau table
// and writes every return back into another one.
constexpr std::string_view RoundTripSource =
    "local Source = { Count = 20, Name = 'luna', Ratio = 0.5, Flag = true }\n"
    "local Results = {}\n"
    "Results.Sum = Sum(Source.Count, 22)\n"
    "Results.Echo = Echo(Source.Name)\n"
    "Results.Scaled = Scale(Source.Ratio)\n"
    "Results.Flag = Negate(Source.Flag)\n"
    "Silent()\n"
    "assert(Results.Sum == 42, 'integer round trip')\n"
    "assert(Results.Echo == 'luna', 'string round trip')\n"
    "assert(Results.Scaled == 1.0, 'number round trip')\n"
    "assert(Results.Flag == false, 'boolean round trip')\n"
    "return Results.Sum";

[[nodiscard]] bool
HasFragments(const Luna::ErrorDiagnostic *Diagnostic,
             const std::array<std::string_view, 4> &Fragments) {
  if (!Diagnostic || Diagnostic->Category() != Luna::ErrorCategory::Runtime ||
      Diagnostic->Message().empty())
    return false;
  for (const std::string_view Fragment : Fragments) {
    if (Fragment.empty())
      continue;
    if (Diagnostic->Message().find(Fragment) == std::string::npos)
      return false;
  }
  return true;
}

// The callback checkpoint a failed conversion must restore exactly: the stack
// returns to its entry depth and carries only the one error value the failure
// reports, so no partial return value survives.
[[nodiscard]] bool RestoredCallbackCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

void CheckRepresentativeReadsAndWrites() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterConverters(Owner),
        "every foundation converter registers through the registry");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const auto Execution = Owner.Execute(RoundTripSource);
  Check(Execution.IsSuccess(),
        "reading arguments from a Luau table and writing returns back "
        "round-trips through the registry");
  Check(EchoCalls == 1 && SumCalls == 1 && ScaleCalls == 1 &&
            NegateCalls == 1 && SilentCalls == 1,
        "each selected callable is invoked exactly once");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a successful pass restores the exact root stack depth");

  // Repeating the pass keeps converting through the same captured generation.
  Check(Owner.Execute(RoundTripSource).IsSuccess() && EchoCalls == 2 &&
            SumCalls == 2,
        "the registry keeps converting on every later invocation");
}

void CheckDeterministicFailuresAndRecovery() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterConverters(Owner),
        "every foundation converter registers before the failure matrix");

  // Each case names one representation the registry must refuse, sourced from a
  // real Luau table wherever a table can carry it.
  const struct FailureCase final {
    std::string_view Description;
    std::string_view Source;
    std::array<std::string_view, 4> Fragments;
  } Cases[]{
      {"a table where a string is expected",
       "local Source = { Name = {} }\nreturn Echo(Source.Name)",
       {"Echo", "argument 1", "expected string", "received table"}},
      {"the table itself where a string is expected",
       "local Source = { 1, 2 }\nreturn Echo(Source)",
       {"Echo", "argument 1", "expected string", "received table"}},
      {"a missing table field where a string is expected",
       "local Source = { Name = 'luna' }\nreturn Echo(Source.Missing)",
       {"Echo", "argument 1", "expected string", "received nil"}},
      {"nil where an integer is expected",
       "return Sum(1, nil)",
       {"Sum", "argument 2", "expected signed 32-bit integer", "received nil"}},
      {"a boolean where an integer is expected",
       "local Source = { Flag = true }\nreturn Sum(1, Source.Flag)",
       {"Sum", "argument 2", "expected signed 32-bit integer",
        "received boolean"}},
      {"a function where a number is expected",
       "return Scale(Silent)",
       {"Scale", "argument 1", "expected number", "received function"}},
      {"a fractional value where an integer is expected",
       "local Source = { Count = 2.5 }\nreturn Sum(Source.Count, 1)",
       {"Sum", "argument 1", "expected an integral value", "2.5"}},
      {"a table where a boolean is expected",
       "local Source = { Flag = {} }\nreturn Negate(Source.Flag)",
       {"Negate", "argument 1", "expected boolean", "received table"}}};

  for (const FailureCase &Case : Cases) {
    ResetCalls();
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    const auto Failed = Owner.Execute(Case.Source);
    Check(!Failed.IsSuccess() &&
              HasFragments(Failed.Diagnostic(), Case.Fragments),
          Case.Description);
    Check(EchoCalls == 0 && SumCalls == 0 && ScaleCalls == 0 &&
              NegateCalls == 0,
          "a refused conversion never invokes the native target");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a refused conversion restores the exact root stack depth");
    Check(RestoredCallbackCheckpoint(Owner),
          "a refused conversion restores the exact callback checkpoint");

    // The same diagnostic every time, and the State converts again right after.
    const auto Repeated = Owner.Execute(Case.Source);
    Check(!Repeated.IsSuccess() && Repeated.Diagnostic() != nullptr &&
              Failed.Diagnostic() != nullptr &&
              Repeated.Diagnostic()->Message() ==
                  Failed.Diagnostic()->Message(),
          "a refusal is deterministic for the same value and generation");

    ResetCalls();
    Check(Owner.Execute(RoundTripSource).IsSuccess() && EchoCalls == 1 &&
              SumCalls == 1,
          "the State still converts after a refused conversion");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "recovery leaves the root stack at its entry depth");
  }

  // The State is still an ordinary State: it registers and invokes new
  // callables after the whole failure matrix.
  Check(Owner.Bindings()
            .Register(
                "Triple", +[](int Value) { return Value * 3; })
            .IsSuccess(),
        "the State still registers after the failure matrix");
  Check(Owner.Execute("assert(Triple(14) == 42)").IsSuccess(),
        "the State still executes after the failure matrix");
}

void CheckReturnReservationFaults() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterConverters(Owner),
        "every foundation converter registers before the reservation faults");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  // A writer that cannot reserve the resources its publication needs refuses
  // before publishing anything, after the native target has run exactly once.
  Hooks::InjectFault(Owner, FaultPoint::ReturnStackCapacity);
  const auto Unreserved = Owner.Execute("return Echo('luna')");
  Check(!Unreserved.IsSuccess() && HasFragments(Unreserved.Diagnostic(),
                                                {"Echo", "Internal error",
                                                 "reserve stack capacity", ""}),
        "a failed return reservation is one deterministic internal refusal");
  Check(EchoCalls == 1,
        "a failed return reservation still invokes the native target at most "
        "once");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a failed return reservation restores the exact root stack depth");
  Check(RestoredCallbackCheckpoint(Owner),
        "a failed return reservation restores the exact callback checkpoint");
  Check(Hooks::PendingFaults(Owner, FaultPoint::ReturnStackCapacity) == 0,
        "the injected reservation fault is consumed exactly once");

  // What the script observes is zero return values and one error, never a
  // partially published result.
  ResetCalls();
  Hooks::InjectFault(Owner, FaultPoint::ReturnStackCapacity);
  const auto Observed = Owner.Execute(
      "local Results = table.pack(pcall(Echo, 'luna'))\n"
      "assert(Results[1] == false, 'the refused call must not succeed')\n"
      "assert(Results.n == 2, 'a refused call exposes no return value')\n"
      "assert(string.find(Results[2], 'Echo') ~= nil, 'named diagnostic')\n"
      "return 0");
  Check(Observed.IsSuccess(),
        "a refused publication exposes zero return values and one diagnostic");
  Check(EchoCalls == 1,
        "the refused publication invoked the native target exactly once");

  // A publication failure after the value was written is contained the same
  // way, and the State keeps converting afterwards.
  ResetCalls();
  Hooks::InjectFault(Owner, FaultPoint::ReturnWrite);
  const auto Unwritten = Owner.Execute("return Sum(1, 2)");
  Check(!Unwritten.IsSuccess() &&
            HasFragments(Unwritten.Diagnostic(),
                         {"Sum", "Internal error", "return-writer", ""}),
        "a failed return publication is one deterministic internal refusal");
  Check(SumCalls == 1 && RestoredCallbackCheckpoint(Owner),
        "a failed return publication restores the exact callback checkpoint");

  ResetCalls();
  Check(Owner.Execute(RoundTripSource).IsSuccess() && EchoCalls == 1 &&
            SumCalls == 1,
        "the State still converts after every reservation and publication "
        "fault");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "recovery leaves the root stack at its entry depth");
}

void CheckInheritedStringPolicyThroughCallables() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterConverters(Owner),
        "every foundation converter registers before the string policy");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  // The inherited per-string policy applies to a real Luau string in both
  // directions: a string of exactly the permitted size round-trips.
  const auto AtLimit =
      Owner.Execute("local Source = { Text = string.rep('a', 1048576) }\n"
                    "assert(#Echo(Source.Text) == 1048576)\n"
                    "return 0");
  Check(AtLimit.IsSuccess() && EchoCalls == 1,
        "a string of exactly the permitted size converts in both directions");

  ResetCalls();
  const auto OverLimit = Owner.Execute("return Echo(string.rep('a', 1048577))");
  Check(!OverLimit.IsSuccess() &&
            HasFragments(OverLimit.Diagnostic(),
                         {"Echo", "argument 1", "1048577 string bytes",
                          "maximum is 1048576"}),
        "an oversized argument reports the received and permitted size");
  Check(EchoCalls == 0,
        "an oversized argument never reaches the native target");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            RestoredCallbackCheckpoint(Owner),
        "an oversized argument restores both stack checkpoints exactly");

  ResetCalls();
  Check(Owner.Execute(RoundTripSource).IsSuccess() && EchoCalls == 1,
        "the State still converts after the string policy refusal");
}

} // namespace

int RunConversionRegistryIntegrationTests() {
  FailureCount = 0;

  CheckRepresentativeReadsAndWrites();
  CheckDeterministicFailuresAndRecovery();
  CheckReturnReservationFaults();
  CheckInheritedStringPolicyThroughCallables();

  ResetCalls();
  return FailureCount == 0 ? 0 : 1;
}
