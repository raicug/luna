// Canonical overload sets and Pareto resolution.
//
// Two halves are checked here. The first is the pure selection model: rank
// sequences compared as Pareto dimensions, with no score, no registration-order
// tie break, and no candidate-count limit. The second is one overload set
// through the real compiler and virtual machine: candidates grouped under one
// qualified name, ordered canonically, resolved by side-effect-free probing,
// and refused with canonical no-match or ambiguity diagnostics.

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/invocation/overload/resolution.hpp"
#include "state/registration/overload_group.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using Luna::ConversionRank;
using Luna::Detail::CandidateRankSequence;
using Luna::Detail::CompareRankSequences;
using Luna::Detail::DominanceOrdering;
using Luna::Detail::OverloadSelectionStatus;
using Luna::Detail::SelectByDominance;
using Luna::Detail::SignatureShapeRank;
using Luna::Detail::ViableCandidate;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "overload resolution check failed: " << Description << '\n';
}

// Declared shapes used by the resolution cases: one trailing optional
// parameter, one final variadic tail, and one plain fixed arity.
[[nodiscard]] int ScaledByFactor(int Value, std::optional<int> Factor) {
  return Value * (Factor ? *Factor : 1);
}

[[nodiscard]] int CountedArguments(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] int AddPair(int Left, int Right) { return Left + Right; }

[[nodiscard]] CandidateRankSequence
Ranks(std::vector<ConversionRank> Positions,
      SignatureShapeRank Shape = SignatureShapeRank::ExactArity) {
  CandidateRankSequence Sequence;
  Sequence.Positions = std::move(Positions);
  Sequence.Shape = Shape;
  return Sequence;
}

[[nodiscard]] std::string
FailureMessage(const Luna::RegistrationResult &Result) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string();
}

[[nodiscard]] std::string
ExecutionMessage(const Luna::ExecutionResult &Result) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string();
}

[[nodiscard]] bool ExecutionSucceeds(Luna::State &Owner,
                                     std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  std::cerr << "overload resolution source failed: " << ExecutionMessage(Result)
            << '\n';
  return false;
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

void CheckDominanceComparesEveryPositionIndependently() {
  const auto Exact = Ranks({ConversionRank::Exact});
  const auto Safe = Ranks({ConversionRank::SafeBuiltIn});
  const auto User = Ranks({ConversionRank::User});

  Check(CompareRankSequences(Exact, Safe) == DominanceOrdering::Better,
        "an exact match outranks a safe built-in conversion");
  Check(CompareRankSequences(Safe, User) == DominanceOrdering::Better,
        "a safe built-in conversion outranks a user conversion");
  Check(CompareRankSequences(User, Exact) == DominanceOrdering::Worse,
        "a user conversion never outranks an exact match");
  Check(CompareRankSequences(Exact, Exact) == DominanceOrdering::Equivalent,
        "identical rank sequences are equivalent, not dominating");

  // One better and one worse position stays incomparable. A weighted sum would
  // have picked the first sequence here; Pareto comparison must not.
  const auto Mixed = Ranks({ConversionRank::Exact, ConversionRank::User});
  const auto Opposite =
      Ranks({ConversionRank::SafeBuiltIn, ConversionRank::SafeBuiltIn});
  Check(CompareRankSequences(Mixed, Opposite) ==
            DominanceOrdering::Incomparable,
        "a sequence better in one position and worse in another is "
        "incomparable");

  // The shape element is one more dimension, never a summed score.
  const auto ExactArity = Ranks({ConversionRank::Exact});
  const auto Omitted =
      Ranks({ConversionRank::Exact}, SignatureShapeRank::OmittedParameters);
  const auto Variadic =
      Ranks({ConversionRank::Exact}, SignatureShapeRank::VariadicConsumption);
  Check(
      CompareRankSequences(ExactArity, Omitted) == DominanceOrdering::Better &&
          CompareRankSequences(Omitted, Variadic) == DominanceOrdering::Better,
      "the signature shape orders exact arity before omitted and variadic "
      "shapes");
  Check(CompareRankSequences(Ranks({ConversionRank::SafeBuiltIn}), Omitted) ==
            DominanceOrdering::Incomparable,
        "a better shape with a worse argument rank stays incomparable");

  // Sequences describing different received counts belong to different calls.
  Check(CompareRankSequences(
            Exact, Ranks({ConversionRank::Exact, ConversionRank::Exact})) ==
            DominanceOrdering::Incomparable,
        "rank sequences of different lengths are never comparable");
}

void CheckSelectionRequiresDominatingEveryOtherCandidate() {
  Check(SelectByDominance({}).Status ==
            OverloadSelectionStatus::NoViableCandidate,
        "no viable candidate reports no match");

  const std::vector<ViableCandidate> Single{
      ViableCandidate{7, Ranks({ConversionRank::User})}};
  const auto Selected = SelectByDominance(Single);
  Check(Selected.Status == OverloadSelectionStatus::Selected &&
            Selected.SelectedCandidate == 7,
        "one viable candidate is selected whatever its rank");

  const std::vector<ViableCandidate> Ordered{
      ViableCandidate{0, Ranks({ConversionRank::SafeBuiltIn})},
      ViableCandidate{1, Ranks({ConversionRank::Exact})},
      ViableCandidate{2, Ranks({ConversionRank::User})}};
  const auto Dominating = SelectByDominance(Ordered);
  Check(Dominating.Status == OverloadSelectionStatus::Selected &&
            Dominating.SelectedCandidate == 1,
        "the candidate that dominates every other viable candidate is "
        "selected");
  Check(Dominating.Frontier.size() == 1 && Dominating.Frontier.front() == 1,
        "a selection reports exactly one non-dominated candidate");

  // Two equivalent candidates are ambiguous: neither dominates the other.
  const std::vector<ViableCandidate> Equivalent{
      ViableCandidate{0, Ranks({ConversionRank::Exact})},
      ViableCandidate{1, Ranks({ConversionRank::Exact})}};
  const auto Tied = SelectByDominance(Equivalent);
  Check(Tied.Status == OverloadSelectionStatus::Ambiguous &&
            Tied.Frontier.size() == 2,
        "equivalent rank sequences stay ambiguous instead of tie-breaking");

  // An incomparable frontier reports every non-dominated candidate and drops
  // the dominated one.
  const std::vector<ViableCandidate> Frontier{
      ViableCandidate{0, Ranks({ConversionRank::Exact, ConversionRank::User})},
      ViableCandidate{1, Ranks({ConversionRank::User, ConversionRank::Exact})},
      ViableCandidate{2, Ranks({ConversionRank::User, ConversionRank::User})}};
  const auto Ambiguous = SelectByDominance(Frontier);
  Check(Ambiguous.Status == OverloadSelectionStatus::Ambiguous &&
            Ambiguous.Frontier.size() == 2 && Ambiguous.Frontier.front() == 0 &&
            Ambiguous.Frontier.back() == 1,
        "an ambiguity reports the non-dominated frontier in canonical order");

  // Selection is independent of the order candidates are supplied in.
  const std::vector<ViableCandidate> Reversed{
      ViableCandidate{2, Ranks({ConversionRank::User})},
      ViableCandidate{1, Ranks({ConversionRank::Exact})},
      ViableCandidate{0, Ranks({ConversionRank::SafeBuiltIn})}};
  Check(SelectByDominance(Reversed).SelectedCandidate == 1,
        "selection never depends on the order candidates are supplied in");
}

void CheckDistinguishabilityIgnoresTheReturnType() {
  using Luna::Detail::CallableSignatureDescriptor;
  using Luna::Detail::SignaturesAreDistinguishable;

  const auto Integer =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32);
  const auto Number =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
  const auto Void = Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Void);

  CallableSignatureDescriptor First;
  First.ReturnType = Integer;
  First.ParameterTypes = {Integer};
  First.RequiredParameterCount = 1;

  CallableSignatureDescriptor OtherReturn = First;
  OtherReturn.ReturnType = Void;
  Check(!SignaturesAreDistinguishable(First, OtherReturn),
        "a different return type alone never distinguishes two candidates");

  CallableSignatureDescriptor OtherParameter = First;
  OtherParameter.ParameterTypes = {Number};
  Check(SignaturesAreDistinguishable(First, OtherParameter),
        "a different parameter type distinguishes two candidates");

  CallableSignatureDescriptor LongerArity = First;
  LongerArity.ParameterTypes = {Integer, Integer};
  LongerArity.RequiredParameterCount = 2;
  Check(SignaturesAreDistinguishable(First, LongerArity),
        "a different arity distinguishes two candidates");

  CallableSignatureDescriptor WithReceiver = First;
  WithReceiver.ReceiverType = Number;
  Check(SignaturesAreDistinguishable(First, WithReceiver),
        "a receiver distinguishes a member candidate from a static one");
}

void CheckOverloadSetGroupsCandidatesCanonically() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  // Registration order is deliberately not canonical order.
  const auto Text = Registry.RegisterFunction(
      "Measure", [](std::string Value) { return int(Value.size()); });
  const auto Pair = Registry.RegisterFunction(
      "Measure", [](int Left, int Right) { return Left * Right; });
  const auto Integer =
      Registry.RegisterFunction("Measure", [](int Value) { return Value * 2; });
  Check(Text.IsSuccess() && Pair.IsSuccess() && Integer.IsSuccess(),
        "distinguishable candidates of one name form one overload set");

  Check(Hooks::BindingCount(Owner) == 1,
        "one qualified name owns exactly one installed callable");
  Check(Hooks::OverloadCandidateCount(Owner, "Measure") == 3,
        "every candidate of the name joins one overload set");
  Check(Hooks::StagedOverloadCandidateCount(Owner, "Measure") == 0,
        "a published overload set stages nothing");
  Check(StackDepth(Owner) == EntryDepth,
        "grouping candidates restores the exact entry stack depth");

  // Canonical order follows the encoded signature, never registration order.
  const std::vector<std::string> Signatures =
      Hooks::OverloadCandidateSignatures(Owner, "Measure");
  Check(Signatures.size() == 3, "every committed candidate is enumerable");

  Luna::State Other;
  Luna::BindingRegistry OtherRegistry = Other.Bindings();
  static_cast<void>(OtherRegistry.RegisterFunction(
      "Measure", [](int Value) { return Value * 2; }));
  static_cast<void>(OtherRegistry.RegisterFunction(
      "Measure", [](std::string Value) { return int(Value.size()); }));
  static_cast<void>(OtherRegistry.RegisterFunction(
      "Measure", [](int Left, int Right) { return Left * Right; }));
  Check(Hooks::OverloadCandidateSignatures(Other, "Measure") == Signatures,
        "candidate order is canonical and never registration order");
}

void CheckIndistinguishableCandidateStaysADuplicate() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto First =
      Registry.RegisterFunction("Scale", [](int Value) { return Value * 2; });
  Check(First.IsSuccess(), "the first candidate registers");

  // The same parameter shape with a different return type: no call could ever
  // select between the two, so the foundation's duplicate diagnostic stands.
  const auto Duplicate =
      Registry.Register("Scale", [](int Value) { static_cast<void>(Value); });
  Check(!Duplicate.IsSuccess() && Duplicate.Diagnostic() &&
            Duplicate.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName,
        "an indistinguishable candidate keeps the duplicate diagnostic");
  Check(Contains(FailureMessage(Duplicate), "already registered"),
        "the duplicate diagnostic keeps the foundation's wording");
  Check(Hooks::OverloadCandidateCount(Owner, "Scale") == 1 &&
            Hooks::StagedOverloadCandidateCount(Owner, "Scale") == 0,
        "a refused candidate leaves the overload set untouched");
  Check(ExecutionSucceeds(Owner, "assert(Scale(4) == 8)"),
        "the published candidate keeps its behavior after a refused duplicate");
}

void CheckResolutionSelectsTheDominatingCandidate() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  static_cast<void>(Registry.RegisterFunction(
      "Describe", [](int Value) { return Value * 10; }));
  static_cast<void>(Registry.RegisterFunction(
      "Describe", [](double Value) { return int(Value * 100.0); }));
  static_cast<void>(Registry.RegisterFunction(
      "Describe", [](std::string Value) { return int(Value.size()); }));
  static_cast<void>(Registry.RegisterFunction(
      "Describe", [](int Left, int Right) { return Left + Right; }));

  // An integral number is exactly a signed 32-bit integer, so the integer
  // candidate dominates the number candidate. A fractional number is exactly a
  // number, so the number candidate wins instead.
  Check(ExecutionSucceeds(Owner, "assert(Describe(3) == 30)\n"
                                 "assert(Describe(2.5) == 250)\n"
                                 "assert(Describe('abcd') == 4)\n"
                                 "assert(Describe(2, 3) == 5)\n"),
        "each call selects the candidate that dominates every other one");
  Check(StackDepth(Owner) == EntryDepth,
        "resolved calls restore the exact entry stack depth");
}

void CheckNoMatchReportsEverySignatureAndItsFirstRejection() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  static_cast<void>(
      Registry.RegisterFunction("Join", [](int Value) { return Value; }));
  static_cast<void>(Registry.RegisterFunction(
      "Join", [](int Left, int Right) { return Left + Right; }));

  const Luna::ExecutionResult Refused = Owner.Execute("return Join(true)");
  const std::string Message = ExecutionMessage(Refused);
  Check(!Refused.IsSuccess(), "a call no candidate accepts fails");
  Check(Contains(Message, "Join"), "the diagnostic names the qualified name");
  Check(Contains(Message, "received (boolean)"),
        "the diagnostic summarizes the received arguments");
  Check(Contains(Message, "expected signed 32-bit integer but received "
                          "boolean"),
        "the diagnostic reports the first rejection of the matching arity");
  Check(Contains(Message, "expects 2 arguments but received 1"),
        "the diagnostic reports the arity rejection of every other signature");

  Check(ExecutionSucceeds(Owner, "assert(Join(2, 3) == 5)"),
        "the State stays usable after a refused resolution");
  Check(StackDepth(Owner) == EntryDepth,
        "a refused resolution restores the exact entry stack depth");
}

void CheckAmbiguousFrontierIsReportedInCanonicalOrder() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  // Two candidates whose rank sequences cannot dominate each other: each one is
  // exact in one position and a safe built-in conversion in the other.
  static_cast<void>(Registry.RegisterFunction(
      "Blend", [](int Left, double Right) { return int(Right) + Left; }));
  static_cast<void>(Registry.RegisterFunction(
      "Blend", [](double Left, int Right) { return int(Left) + Right; }));

  const Luna::ExecutionResult Ambiguous = Owner.Execute("return Blend(1, 2)");
  const std::string Message = ExecutionMessage(Ambiguous);
  Check(!Ambiguous.IsSuccess(), "an ambiguous call fails instead of guessing");
  Check(Contains(Message, "ambiguous"), "the diagnostic reports ambiguity");
  Check(Contains(Message, "(signed 32-bit integer, number)") &&
            Contains(Message, "(number, signed 32-bit integer)"),
        "the diagnostic lists the non-dominated signatures");

  // Both candidates remain callable where the call is not ambiguous.
  Check(ExecutionSucceeds(Owner, "assert(Blend(1, 2.5) == 3)\n"
                                 "assert(Blend(1.5, 2) == 3)\n"),
        "an unambiguous call still resolves after an ambiguous one");
}

void CheckRankingCommitsNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  int IntegerCalls = 0;
  int NumberCalls = 0;
  int TextCalls = 0;
  static_cast<void>(
      Registry.RegisterFunction("Count", [&IntegerCalls](int Value) {
        ++IntegerCalls;
        return Value;
      }));
  static_cast<void>(
      Registry.RegisterFunction("Count", [&NumberCalls](double Value) {
        ++NumberCalls;
        return int(Value);
      }));
  static_cast<void>(
      Registry.RegisterFunction("Count", [&TextCalls](std::string Value) {
        ++TextCalls;
        return int(Value.size());
      }));

  Check(ExecutionSucceeds(Owner, "assert(Count(5) == 5)"),
        "the selected candidate runs");
  Check(IntegerCalls == 1 && NumberCalls == 0 && TextCalls == 0,
        "only the selected candidate is invoked, and it runs exactly once");

  const Luna::ExecutionResult Refused = Owner.Execute("return Count(nil)");
  Check(!Refused.IsSuccess(), "a call no candidate accepts fails");
  Check(IntegerCalls == 1 && NumberCalls == 0 && TextCalls == 0,
        "ranking invokes no native target at all");
}

void CheckScopedOverloadSetPublishesOneCallable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Measure", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.RegisterFunction(
      "Measure", [](int Left, int Right) { return Left * Right; }));
  const auto Result = Studio.Commit();
  Check(Result.IsSuccess(),
        "one plan may declare two distinguishable candidates of one name");
  Check(Hooks::OverloadCandidateCount(Owner, "Studio.Measure") == 2,
        "a scoped overload set publishes every candidate");
  Check(ExecutionSucceeds(Owner, "assert(Studio.Measure(4) == 8)\n"
                                 "assert(Studio.Measure(4, 3) == 12)\n"),
        "every scoped candidate is invocable through one path");

  // A candidate may still join a committed scoped overload set afterwards.
  Luna::NamespaceBuilder Reopened = Registry.RegisterNamespace("Studio");
  static_cast<void>(Reopened.RegisterFunction(
      "Measure", [](std::string Value) { return int(Value.size()); }));
  const auto Joined = Reopened.Commit();
  Check(Joined.IsSuccess(),
        "a later candidate joins a committed scoped overload set");
  Check(Hooks::OverloadCandidateCount(Owner, "Studio.Measure") == 3,
        "the joined candidate is published into the same set");
  Check(ExecutionSucceeds(Owner, "assert(Studio.Measure('abc') == 3)\n"
                                 "assert(Studio.Measure(4, 3) == 12)\n"),
        "joining a candidate preserves every earlier candidate");
}

void CheckRefusedCandidateLeavesTheCommittedSetIntact() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  static_cast<void>(
      Registry.RegisterFunction("Total", [](int Value) { return Value; }));
  const auto Address = Hooks::BindingRecordAddress(Owner, "Total");

  // A candidate that joins a committed overload set and then fails before
  // publication must leave that set exactly as it was.
  Hooks::InjectFault(Owner, FaultPoint::TransactionConsistency);
  const auto Refused = Registry.RegisterFunction(
      "Total", [](int Left, int Right) { return Left + Right; });
  Check(!Refused.IsSuccess(),
        "a joining candidate rejected before publication fails");
  Check(Hooks::OverloadCandidateCount(Owner, "Total") == 1 &&
            Hooks::StagedOverloadCandidateCount(Owner, "Total") == 0,
        "a failed attempt discards exactly the candidate it staged");
  Check(Hooks::BindingRecordAddress(Owner, "Total") == Address,
        "the committed overload set keeps its record");
  Check(ExecutionSucceeds(Owner, "assert(Total(7) == 7)"),
        "the committed candidate stays invocable after a failed attempt");
  Check(StackDepth(Owner) == EntryDepth,
        "a failed joining attempt restores the exact entry stack depth");

  const auto Retry = Registry.RegisterFunction(
      "Total", [](int Left, int Right) { return Left + Right; });
  Check(Retry.IsSuccess(), "the State stays reusable after a failed attempt");
  Check(Hooks::OverloadCandidateCount(Owner, "Total") == 2,
        "the retried candidate joins the committed overload set");
  Check(ExecutionSucceeds(Owner, "assert(Total(2, 3) == 5)\n"
                                 "assert(Total(7) == 7)\n"),
        "every candidate of the joined set is invocable");
}

void CheckSingleCandidateKeepsFoundationDiagnostics() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  static_cast<void>(Registry.RegisterFunction(
      "Add", [](int Left, int Right) { return Left + Right; }));

  const Luna::ExecutionResult WrongCount = Owner.Execute("return Add(1)");
  Check(!WrongCount.IsSuccess() &&
            Contains(ExecutionMessage(WrongCount),
                     "expected 2 arguments but received 1"),
        "a single candidate keeps the foundation's arity diagnostic");

  const Luna::ExecutionResult WrongType = Owner.Execute("return Add(1, 'two')");
  Check(!WrongType.IsSuccess() &&
            Contains(ExecutionMessage(WrongType),
                     "argument 2 expected signed 32-bit integer but received "
                     "string"),
        "a single candidate keeps the foundation's type diagnostic");
  Check(ExecutionSucceeds(Owner, "assert(Add(2, 3) == 5)"),
        "the single-candidate callable stays invocable");
}

// One overload set publishes one reflected set record and one reflected
// candidate per declaration, each naming the set it belongs to.
void CheckOverloadSetIsReflectedWithItsCandidates() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  static_cast<void>(Registry.RegisterFunction(
      "Area", [](int Value) { return Value * Value; }));
  static_cast<void>(Registry.RegisterFunction(
      "Area", [](int Width, int Height) { return Width * Height; }));
  static_cast<void>(Registry.RegisterFunction("Area", [](std::string Value) {
    return static_cast<int>(Value.size());
  }));

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Set = Snapshot.Find("Area");
  Check(Set.IsValid() && Set.Kind() == Luna::SymbolKind::OverloadSet,
        "one qualified name reflects exactly one overload set");
  Check(Set.Name() == "Area" && Set.QualifiedName() == "Area" &&
            Set.Scope().IsRoot(),
        "a root overload set is reflected in the root scope");

  const Luna::ReflectionRecordRange Candidates =
      Snapshot.Symbols(Luna::ScopeId(Set.Id()));
  Check(Candidates.Size() == 3,
        "every candidate of the set is reflected inside it");

  std::vector<std::string> Signatures;
  bool EveryCandidateNamesTheSet = true;
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Candidates.At(Index);
    if (Candidate.Kind() != Luna::SymbolKind::FunctionCandidate ||
        Candidate.OverloadSet() != Set.Id() ||
        Candidate.QualifiedName() != "Area" || Candidate.Signature().empty())
      EveryCandidateNamesTheSet = false;
    Signatures.emplace_back(Candidate.Signature());
  }
  Check(EveryCandidateNamesTheSet,
        "each reflected candidate names the overload set it belongs to");

  // Canonical order, and one distinct reflected signature per candidate.
  std::vector<std::string> Sorted = Signatures;
  std::sort(Sorted.begin(), Sorted.end());
  Check(Sorted == Signatures, "candidate records are enumerated canonically");
  Check(std::unique(Sorted.begin(), Sorted.end()) == Sorted.end(),
        "no two candidates of one set reflect the same signature");

  // The ordered parameters and the return shape of one candidate.
  const Luna::ReflectionRecord Pair = Snapshot.Find(Candidates.At(0).Id());
  Check(Snapshot.Symbols(Luna::SymbolKind::FunctionCandidate).Size() == 3 &&
            Snapshot.Symbols(Luna::SymbolKind::OverloadSet).Size() == 1,
        "the generation enumerates the set and its candidates by kind");
  bool EveryParameterIsRequired = true;
  for (std::size_t Index = 0; Index < Pair.ParameterCount(); ++Index) {
    const Luna::ParameterRecord Parameter = Pair.Parameter(Index);
    if (Parameter.Disposition() != Luna::ParameterDisposition::Required ||
        Parameter.HasDefault() || !Parameter.Type().IsValid())
      EveryParameterIsRequired = false;
  }
  Check(EveryParameterIsRequired,
        "a foundation candidate reflects required parameters only");
  Check(Pair.Returns() == Luna::ReturnShape::Scalar &&
            Pair.ReturnCount() == 1 && Pair.Return(0).Type().IsValid(),
        "a scalar candidate reflects exactly one returned value");
}

void CheckVoidCandidateReflectsZeroReturns() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  static_cast<void>(Registry.RegisterFunction(
      "Sink", [](int Value) { static_cast<void>(Value); }));

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecordRange Candidates =
      Snapshot.Symbols(Luna::ScopeId(Snapshot.Find("Sink").Id()));
  Check(Candidates.Size() == 1, "one declaration reflects one candidate");
  const Luna::ReflectionRecord Candidate = Candidates.At(0);
  Check(Candidate.Returns() == Luna::ReturnShape::Zero &&
            Candidate.ReturnCount() == 0,
        "a void candidate reflects zero returned values");
  Check(Candidate.ParameterCount() == 1,
        "a void candidate still reflects its ordered parameters");
}

void CheckScopedOverloadSetIsReflectedInsideItsNamespace() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Scale", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.RegisterFunction(
      "Scale", [](int Value, int Factor) { return Value * Factor; }));
  Check(Studio.Commit().IsSuccess(), "the scoped overload set publishes");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Set = Snapshot.Find("Studio.Scale");
  Check(Set.IsValid() && Set.Kind() == Luna::SymbolKind::OverloadSet &&
            Set.Scope().Owner() == Snapshot.Find("Studio").Id(),
        "a scoped overload set is reflected inside its namespace");
  Check(Snapshot.Symbols(Luna::ScopeId(Set.Id())).Size() == 2,
        "both scoped candidates are reflected inside the set");

  // A candidate joining the published set adds one candidate record and no
  // second set record.
  Luna::NamespaceBuilder Reopened = Registry.RegisterNamespace("Studio");
  static_cast<void>(Reopened.RegisterFunction(
      "Scale", [](std::string Value) { return int(Value.size()); }));
  Check(Reopened.Commit().IsSuccess(), "a later candidate joins the set");

  const Luna::ReflectionSnapshot Joined = Registry.Reflection();
  Check(Joined.Symbols(Luna::SymbolKind::OverloadSet).Size() == 1,
        "joining a candidate reflects no second overload set");
  Check(Joined.Find("Studio.Scale").Id() == Set.Id(),
        "the overload set keeps its identity when a candidate joins it");
  Check(Joined.Symbols(Luna::ScopeId(Set.Id())).Size() == 3,
        "the joined candidate is reflected inside the same set");
}

void CheckDeclaredShapeIsReflected() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Check(Registry
            .RegisterFunction("Scaled", Luna::WithDefaults(&ScaledByFactor, 3))
            .IsSuccess(),
        "a defaulted declared shape registers");
  Check(Registry.RegisterFunction("Counted", &CountedArguments).IsSuccess(),
        "a variadic declared shape registers");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecordRange Scaled =
      Snapshot.Symbols(Luna::ScopeId(Snapshot.Find("Scaled").Id()));
  Check(Scaled.Size() == 1, "the declared shape reflects one candidate");
  const Luna::ReflectionRecord Defaulted = Scaled.At(0);
  Check(Defaulted.ParameterCount() == 2,
        "every declared parameter is reflected in declared order");
  Check(Defaulted.Parameter(0).Disposition() ==
                Luna::ParameterDisposition::Required &&
            !Defaulted.Parameter(0).HasDefault(),
        "the leading required parameter keeps its disposition");
  Check(Defaulted.Parameter(1).Disposition() ==
                Luna::ParameterDisposition::Defaulted &&
            Defaulted.Parameter(1).HasDefault() &&
            Defaulted.Parameter(1).DefaultText() == "3",
        "the defaulted parameter reflects its immutable default");

  const Luna::ReflectionRecordRange Counted =
      Snapshot.Symbols(Luna::ScopeId(Snapshot.Find("Counted").Id()));
  Check(Counted.Size() == 1, "the variadic shape reflects one candidate");
  const Luna::ReflectionRecord Variadic = Counted.At(0);
  Check(Variadic.ParameterCount() == 1 &&
            Variadic.Parameter(0).Disposition() ==
                Luna::ParameterDisposition::Variadic,
        "the final variadic parameter is reflected as variadic");
  Check(Contains(Variadic.Signature(), "..."),
        "a variadic candidate reflects its tail in its signature");
  Check(Contains(Defaulted.Signature(), "?"),
        "an omittable parameter is marked in the reflected signature");
}

// Resolution ranks the declared shape, not only the fixed parameter types: a
// variadic candidate participates, and a fixed candidate still wins the calls
// it accepts exactly.
void CheckDeclaredShapeParticipatesInResolution() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  static_cast<void>(Registry.RegisterFunction("Sum", &AddPair));
  static_cast<void>(Registry.RegisterFunction("Sum", &CountedArguments));
  Check(Hooks::OverloadCandidateCount(Owner, "Sum") == 2,
        "a fixed and a variadic candidate share one overload set");

  // Two arguments: both candidates are viable with equal argument ranks, and
  // the exact fixed arity dominates the variadic tail on the shape dimension.
  Check(ExecutionSucceeds(Owner, "assert(Sum(2, 3) == 5)"),
        "an exact fixed arity outranks a variadic tail");

  // Three arguments and one string argument: only the variadic candidate
  // accepts them, and it consumes the whole tail.
  Check(ExecutionSucceeds(Owner, "assert(Sum(1, 2, 3) == 3)\n"
                                 "assert(Sum('one') == 1)\n"
                                 "assert(Sum() == 0)\n"),
        "a variadic candidate accepts the calls no fixed candidate accepts");
  Check(StackDepth(Owner) == EntryDepth,
        "resolving a declared shape restores the exact entry stack depth");

  const Luna::ExecutionResult Refused = Owner.Execute("return Sum({})");
  Check(!Refused.IsSuccess() &&
            Contains(ExecutionMessage(Refused), "variadic value"),
        "a value outside the variadic domain is refused by position");
  Check(Contains(ExecutionMessage(Refused), "..."),
        "the no-match diagnostic renders the variadic candidate's shape");
}

void CheckOmittableParametersParticipateInResolution() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  static_cast<void>(Registry.RegisterFunction("Pick", &ScaledByFactor));
  static_cast<void>(Registry.RegisterFunction(
      "Pick", [](std::string Value) { return int(Value.size()); }));

  // The optional parameter accepts both omission and an explicit nil, and
  // neither call is confused with the string candidate.
  Check(ExecutionSucceeds(Owner, "assert(Pick(4) == 4)\n"
                                 "assert(Pick(4, nil) == 4)\n"
                                 "assert(Pick(4, 3) == 12)\n"
                                 "assert(Pick('abc') == 3)\n"),
        "an omittable declared parameter is ranked by its declared shape");

  const Luna::ExecutionResult Refused = Owner.Execute("return Pick(4, 'two')");
  Check(!Refused.IsSuccess(),
        "an argument no candidate accepts is still refused");
  Check(Contains(ExecutionMessage(Refused), "(optional)"),
        "the no-match diagnostic renders the omittable parameter");
}

// Requirements 6.11, 6.8: only the selected candidate commits work, it commits
// it once, and a refused resolution commits none at all.
void CheckSelectedCandidateCommitsExactlyOnce() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  int FixedCalls = 0;
  int VariadicCalls = 0;
  int TextCalls = 0;
  static_cast<void>(
      Registry.RegisterFunction("Track", [&FixedCalls](int Left, int Right) {
        ++FixedCalls;
        return Left + Right;
      }));
  static_cast<void>(Registry.RegisterFunction(
      "Track", [&VariadicCalls](Luna::ArgumentView Arguments) {
        ++VariadicCalls;
        return static_cast<int>(Arguments.Size());
      }));
  static_cast<void>(
      Registry.RegisterFunction("Track", [&TextCalls](std::string Value) {
        ++TextCalls;
        return int(Value.size());
      }));

  Check(ExecutionSucceeds(Owner, "assert(Track(2, 3) == 5)"),
        "the dominating candidate is invoked");
  Check(FixedCalls == 1 && VariadicCalls == 0 && TextCalls == 0,
        "the selected native target runs exactly once and no other runs");

  Check(ExecutionSucceeds(Owner, "assert(Track(1, 2, 3) == 3)"),
        "the variadic candidate is invoked when it is the only viable one");
  Check(FixedCalls == 1 && VariadicCalls == 1 && TextCalls == 0,
        "each selected candidate runs exactly once per call");

  // A refused resolution commits nothing: no conversion result is published and
  // no native target runs.
  const Luna::ExecutionResult Refused = Owner.Execute("return Track({}, {})");
  Check(!Refused.IsSuccess(), "a call no candidate accepts fails");
  Check(FixedCalls == 1 && VariadicCalls == 1 && TextCalls == 0,
        "a refused resolution invokes no native target at all");
  Check(StackDepth(Owner) == EntryDepth,
        "a refused resolution restores the exact entry stack depth");
  Check(ExecutionSucceeds(Owner, "assert(Track('four') == 4)"),
        "the State stays usable after a refused resolution");
  Check(TextCalls == 1, "the later selected candidate runs exactly once");
}

void CheckAmbiguousCallCommitsNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  int LeftCalls = 0;
  int RightCalls = 0;
  static_cast<void>(
      Registry.RegisterFunction("Mix", [&LeftCalls](int Left, double Right) {
        ++LeftCalls;
        return Left + int(Right);
      }));
  static_cast<void>(
      Registry.RegisterFunction("Mix", [&RightCalls](double Left, int Right) {
        ++RightCalls;
        return int(Left) + Right;
      }));

  const Luna::ExecutionResult Ambiguous = Owner.Execute("return Mix(1, 2)");
  Check(!Ambiguous.IsSuccess() &&
            Contains(ExecutionMessage(Ambiguous), "ambiguous"),
        "an incomparable frontier fails as ambiguous");
  Check(LeftCalls == 0 && RightCalls == 0,
        "an ambiguous call invokes no candidate at all");
  Check(ExecutionSucceeds(Owner, "assert(Mix(1, 2.5) == 3)"),
        "an unambiguous call still resolves afterwards");
  Check(LeftCalls == 1 && RightCalls == 0,
        "the selected candidate of the later call runs exactly once");
}

} // namespace

int RunOverloadResolutionTests() {
  FailureCount = 0;
  CheckDominanceComparesEveryPositionIndependently();
  CheckSelectionRequiresDominatingEveryOtherCandidate();
  CheckDistinguishabilityIgnoresTheReturnType();
  CheckOverloadSetGroupsCandidatesCanonically();
  CheckIndistinguishableCandidateStaysADuplicate();
  CheckResolutionSelectsTheDominatingCandidate();
  CheckNoMatchReportsEverySignatureAndItsFirstRejection();
  CheckAmbiguousFrontierIsReportedInCanonicalOrder();
  CheckRankingCommitsNothing();
  CheckScopedOverloadSetPublishesOneCallable();
  CheckRefusedCandidateLeavesTheCommittedSetIntact();
  CheckSingleCandidateKeepsFoundationDiagnostics();
  CheckOverloadSetIsReflectedWithItsCandidates();
  CheckVoidCandidateReflectsZeroReturns();
  CheckScopedOverloadSetIsReflectedInsideItsNamespace();
  CheckDeclaredShapeIsReflected();
  CheckDeclaredShapeParticipatesInResolution();
  CheckOmittableParametersParticipateInResolution();
  CheckSelectedCandidateCommitsExactlyOnce();
  CheckAmbiguousCallCommitsNothing();
  return FailureCount == 0 ? 0 : 1;
}
