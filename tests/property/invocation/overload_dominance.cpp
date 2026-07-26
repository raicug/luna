// Property 24: overload resolution agrees with Pareto dominance.
//
// Two halves are generated together. The first drives the pure selection model
// with generated rank matrices - arities, incomparable frontiers, and per
// candidate rejection reasons - and compares viability, unique selection, and
// the reported frontier with an independent Pareto reference model written
// here. The second registers one generated overload set through the real
// compiler and virtual machine in two different registration orders and
// compares the resolved call, the canonical no-match diagnostic, and the
// canonical ambiguity listing with the same reference model.
//
// The instrumentation is what makes the side-effect rules observable. Luna
// counts every side-effect-free argument probe and every committing argument
// conversion privately; each candidate counts its own invocations, records the
// converted values it received, and constructs one counted native object. So a
// refused resolution must report probes and no committing conversion, no target
// invocation, and no native construction, while a resolved call must report
// exactly one committing conversion per supplied argument of the selected
// candidate, exactly one invocation of that candidate, and none of any other.
//
// Lua-owned native objects arrive with userdata, so "ranking constructs no
// Lua-owned native object" is measured here as the counted native construction
// plus an unchanged virtual machine: the same globals, no published result, and
// the exact entry stack depth.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/state/state.hpp>

#include "state/invocation/overload/instrumentation.hpp"
#include "state/invocation/overload/resolution.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/type/conversion_frame.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::ConversionRank;
using Luna::Detail::CandidateRankSequence;
using Luna::Detail::CompareRankSequences;
using Luna::Detail::DominanceOrdering;
using Luna::Detail::Dominates;
using Luna::Detail::OverloadSelection;
using Luna::Detail::OverloadSelectionStatus;
using Luna::Detail::SelectByDominance;
using Luna::Detail::SignatureShapeRank;
using Luna::Detail::ViableCandidate;

// Deterministic byte source. Equal bytes always drive the equal scenario, so a
// shrunk counterexample rebuilds the exact same overload set and call.
class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 37U + 13U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

// ---------------------------------------------------------------------------
// The independent Pareto reference model.
// ---------------------------------------------------------------------------

enum class ReferenceOrdering { Better, Worse, Equivalent, Incomparable };

[[nodiscard]] int ReferenceRankOrder(ConversionRank Rank) noexcept {
  switch (Rank) {
  case ConversionRank::Exact:
    return 0;
  case ConversionRank::SafeBuiltIn:
    return 1;
  case ConversionRank::User:
    return 2;
  }
  return 3;
}

[[nodiscard]] int ReferenceShapeOrder(SignatureShapeRank Shape) noexcept {
  switch (Shape) {
  case SignatureShapeRank::ExactArity:
    return 0;
  case SignatureShapeRank::OmittedParameters:
    return 1;
  case SignatureShapeRank::VariadicConsumption:
    return 2;
  }
  return 3;
}

// Pareto comparison, written independently of Luna: every position and the
// shape element are their own dimension and nothing is ever summed.
[[nodiscard]] ReferenceOrdering
ReferenceCompare(const CandidateRankSequence &Left,
                 const CandidateRankSequence &Right) {
  if (Left.Positions.size() != Right.Positions.size())
    return ReferenceOrdering::Incomparable;

  std::vector<int> LeftDimensions;
  std::vector<int> RightDimensions;
  for (std::size_t Index = 0; Index < Left.Positions.size(); ++Index) {
    LeftDimensions.push_back(ReferenceRankOrder(Left.Positions[Index]));
    RightDimensions.push_back(ReferenceRankOrder(Right.Positions[Index]));
  }
  LeftDimensions.push_back(ReferenceShapeOrder(Left.Shape));
  RightDimensions.push_back(ReferenceShapeOrder(Right.Shape));

  bool AnyBetter = false;
  bool AnyWorse = false;
  for (std::size_t Index = 0; Index < LeftDimensions.size(); ++Index) {
    if (LeftDimensions[Index] < RightDimensions[Index])
      AnyBetter = true;
    else if (LeftDimensions[Index] > RightDimensions[Index])
      AnyWorse = true;
  }

  if (AnyBetter && AnyWorse)
    return ReferenceOrdering::Incomparable;
  if (AnyBetter)
    return ReferenceOrdering::Better;
  if (AnyWorse)
    return ReferenceOrdering::Worse;
  return ReferenceOrdering::Equivalent;
}

struct ReferenceSelection final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;
  std::size_t SelectedCandidate = 0;
  std::vector<std::size_t> Frontier;
};

// The reference resolver. A candidate is selected only when it is better than
// every other viable candidate; anything else is the non-dominated frontier in
// the order the candidates were supplied.
[[nodiscard]] ReferenceSelection
ReferenceSelect(const std::vector<ViableCandidate> &Viable) {
  ReferenceSelection Selection;
  if (Viable.empty())
    return Selection;

  for (std::size_t Index = 0; Index < Viable.size(); ++Index) {
    bool BetterThanEveryOther = true;
    for (std::size_t Other = 0; Other < Viable.size(); ++Other) {
      if (Other == Index)
        continue;
      if (ReferenceCompare(Viable[Index].Ranks, Viable[Other].Ranks) !=
          ReferenceOrdering::Better) {
        BetterThanEveryOther = false;
        break;
      }
    }
    if (!BetterThanEveryOther)
      continue;
    Selection.Status = OverloadSelectionStatus::Selected;
    Selection.SelectedCandidate = Viable[Index].Candidate;
    Selection.Frontier = {Viable[Index].Candidate};
    return Selection;
  }

  Selection.Status = OverloadSelectionStatus::Ambiguous;
  for (std::size_t Index = 0; Index < Viable.size(); ++Index) {
    bool IsDominated = false;
    for (std::size_t Other = 0; Other < Viable.size(); ++Other) {
      if (Other == Index)
        continue;
      if (ReferenceCompare(Viable[Other].Ranks, Viable[Index].Ranks) ==
          ReferenceOrdering::Better) {
        IsDominated = true;
        break;
      }
    }
    if (!IsDominated)
      Selection.Frontier.push_back(Viable[Index].Candidate);
  }
  return Selection;
}

[[nodiscard]] const CandidateRankSequence *
FindRanks(const std::vector<ViableCandidate> &Viable, std::size_t Candidate) {
  for (const ViableCandidate &Entry : Viable) {
    if (Entry.Candidate == Candidate)
      return &Entry.Ranks;
  }
  return nullptr;
}

[[nodiscard]] ConversionRank GeneratedRank(std::size_t Choice) noexcept {
  switch (Choice % 3) {
  case 0:
    return ConversionRank::Exact;
  case 1:
    return ConversionRank::SafeBuiltIn;
  default:
    return ConversionRank::User;
  }
}

[[nodiscard]] SignatureShapeRank GeneratedShape(std::size_t Choice) noexcept {
  switch (Choice % 3) {
  case 0:
    return SignatureShapeRank::ExactArity;
  case 1:
    return SignatureShapeRank::OmittedParameters;
  default:
    return SignatureShapeRank::VariadicConsumption;
  }
}

// One generated rank matrix: candidates that were rejected before ranking, and
// the ordered rank sequence of every candidate that survived.
void VerifyDominanceModel(ByteCursor &Cursor) {
  // No candidate-count limit exists, so the generated set is deliberately
  // allowed to be larger than any signature a call would realistically have.
  const std::size_t CandidateCount = 1U + Cursor.Pick(12);
  const std::size_t PositionCount = Cursor.Pick(4);

  std::vector<ViableCandidate> Viable;
  std::vector<std::string> Rejections;
  for (std::size_t Index = 0; Index < CandidateCount; ++Index) {
    // A rejected candidate contributes its first deterministic reason and no
    // rank sequence at all.
    if (Cursor.Pick(4) == 0) {
      Rejections.push_back("candidate " + std::to_string(Index) +
                           " rejected argument " +
                           std::to_string(1U + Cursor.Pick(4)));
      continue;
    }

    CandidateRankSequence Ranks;
    for (std::size_t Position = 0; Position < PositionCount; ++Position)
      Ranks.Positions.push_back(GeneratedRank(Cursor.Pick(3)));
    Ranks.Shape = GeneratedShape(Cursor.Pick(3));
    Viable.push_back(ViableCandidate{Index, std::move(Ranks)});
  }

  RC_ASSERT(Viable.size() + Rejections.size() == CandidateCount);

  // Every pairwise comparison agrees with the reference relation, and the
  // relation is antisymmetric.
  for (const ViableCandidate &Left : Viable) {
    for (const ViableCandidate &Right : Viable) {
      const DominanceOrdering Observed =
          CompareRankSequences(Left.Ranks, Right.Ranks);
      const ReferenceOrdering Expected =
          ReferenceCompare(Left.Ranks, Right.Ranks);
      switch (Expected) {
      case ReferenceOrdering::Better:
        RC_ASSERT(Observed == DominanceOrdering::Better);
        RC_ASSERT(CompareRankSequences(Right.Ranks, Left.Ranks) ==
                  DominanceOrdering::Worse);
        break;
      case ReferenceOrdering::Worse:
        RC_ASSERT(Observed == DominanceOrdering::Worse);
        break;
      case ReferenceOrdering::Equivalent:
        RC_ASSERT(Observed == DominanceOrdering::Equivalent);
        break;
      case ReferenceOrdering::Incomparable:
        RC_ASSERT(Observed == DominanceOrdering::Incomparable);
        break;
      }
      RC_ASSERT(Dominates(Left.Ranks, Right.Ranks) ==
                (Observed == DominanceOrdering::Better));
    }
  }

  const OverloadSelection Selected = SelectByDominance(Viable);
  const ReferenceSelection Expected = ReferenceSelect(Viable);
  RC_ASSERT(Selected.Status == Expected.Status);
  RC_ASSERT(Selected.Frontier == Expected.Frontier);
  RC_ASSERT((Selected.Status == OverloadSelectionStatus::NoViableCandidate) ==
            Viable.empty());

  if (Selected.Status == OverloadSelectionStatus::Selected) {
    RC_ASSERT(Selected.SelectedCandidate == Expected.SelectedCandidate);
    RC_ASSERT(Selected.Frontier.size() == 1);
    RC_ASSERT(Selected.Frontier.front() == Selected.SelectedCandidate);

    // The selected candidate really is better than every other viable one, so
    // no equivalent or incomparable rival was tie-broken away.
    const CandidateRankSequence *Winner =
        FindRanks(Viable, Selected.SelectedCandidate);
    RC_ASSERT(Winner != nullptr);
    for (const ViableCandidate &Other : Viable) {
      if (Other.Candidate == Selected.SelectedCandidate)
        continue;
      RC_ASSERT(Dominates(*Winner, Other.Ranks));
    }
  }

  if (Selected.Status == OverloadSelectionStatus::Ambiguous) {
    RC_ASSERT(!Selected.Frontier.empty());

    // Nothing on the reported frontier is dominated by any viable candidate.
    for (const std::size_t Reported : Selected.Frontier) {
      const CandidateRankSequence *Ranks = FindRanks(Viable, Reported);
      RC_ASSERT(Ranks != nullptr);
      for (const ViableCandidate &Other : Viable) {
        if (Other.Candidate == Reported)
          continue;
        RC_ASSERT(!Dominates(Other.Ranks, *Ranks));
      }
    }

    // And no viable candidate is better than every other one, which is exactly
    // why the call is ambiguous rather than resolved.
    for (const ViableCandidate &Candidate : Viable) {
      bool BetterThanEveryOther = true;
      for (const ViableCandidate &Other : Viable) {
        if (Other.Candidate == Candidate.Candidate)
          continue;
        if (!Dominates(Candidate.Ranks, Other.Ranks)) {
          BetterThanEveryOther = false;
          break;
        }
      }
      RC_ASSERT(!BetterThanEveryOther);
    }
  }

  // Selection never depends on the order candidates are supplied in, and the
  // reported frontier follows exactly that order.
  std::vector<ViableCandidate> Permuted = Viable;
  for (std::size_t Index = Permuted.size(); Index > 1; --Index) {
    const std::size_t Position = Cursor.Pick(Index);
    std::swap(Permuted[Index - 1], Permuted[Position]);
  }
  const OverloadSelection Reordered = SelectByDominance(Permuted);
  RC_ASSERT(Reordered.Status == Selected.Status);
  RC_ASSERT(Reordered.SelectedCandidate == Selected.SelectedCandidate);

  std::vector<std::size_t> ExpectedOrder;
  for (const ViableCandidate &Candidate : Permuted) {
    if (std::find(Selected.Frontier.begin(), Selected.Frontier.end(),
                  Candidate.Candidate) != Selected.Frontier.end())
      ExpectedOrder.push_back(Candidate.Candidate);
  }
  RC_ASSERT(Reordered.Frontier == ExpectedOrder);
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// One generated overload set through the real compiler and virtual machine.
// ---------------------------------------------------------------------------

enum class ParameterKind { Integer, Number, Text, Flag };

constexpr std::size_t CandidatePoolSize = 11;
constexpr std::string_view CallableName = "Resolve";

[[nodiscard]] std::string_view PublicTypeName(ParameterKind Kind) noexcept {
  switch (Kind) {
  case ParameterKind::Integer:
    return "signed 32-bit integer";
  case ParameterKind::Number:
    return "number";
  case ParameterKind::Text:
    return "string";
  case ParameterKind::Flag:
    return "boolean";
  }
  return "unknown";
}

// Pairwise distinguishable candidate shapes: every entry differs from every
// other in arity or in a parameter type, so all of them join one overload set.
[[nodiscard]] const std::vector<std::vector<ParameterKind>> &CandidatePool() {
  static const std::vector<std::vector<ParameterKind>> Pool{
      {ParameterKind::Integer},
      {ParameterKind::Number},
      {ParameterKind::Text},
      {ParameterKind::Flag},
      {ParameterKind::Integer, ParameterKind::Integer},
      {ParameterKind::Number, ParameterKind::Integer},
      {ParameterKind::Integer, ParameterKind::Number},
      {ParameterKind::Text, ParameterKind::Integer},
      {ParameterKind::Number, ParameterKind::Number},
      {},
      {ParameterKind::Integer, ParameterKind::Integer, ParameterKind::Integer}};
  return Pool;
}

[[nodiscard]] std::string IntegerText(int Value) {
  return std::to_string(Value);
}

[[nodiscard]] std::string NumberText(double Value) {
  std::ostringstream Stream;
  Stream << Value;
  return Stream.str();
}

[[nodiscard]] std::string FlagText(bool Value) {
  return Value ? "true" : "false";
}

enum class ArgumentKind { IntegralNumber, FractionalNumber, Text, Flag, Nil };

struct ArgumentSample final {
  std::string Literal;
  std::string LuauTypeName;
  ArgumentKind Kind = ArgumentKind::Nil;
  double Number = 0.0;
  std::string TextValue;
  bool Flag = false;
};

[[nodiscard]] const std::vector<ArgumentSample> &ArgumentPool() {
  static const std::vector<ArgumentSample> Pool{
      ArgumentSample{
          "7", "number", ArgumentKind::IntegralNumber, 7.0, {}, false},
      ArgumentSample{
          "-3", "number", ArgumentKind::IntegralNumber, -3.0, {}, false},
      ArgumentSample{
          "2.5", "number", ArgumentKind::FractionalNumber, 2.5, {}, false},
      ArgumentSample{"'abc'", "string", ArgumentKind::Text, 0.0, "abc", false},
      ArgumentSample{"''", "string", ArgumentKind::Text, 0.0, "", false},
      ArgumentSample{"true", "boolean", ArgumentKind::Flag, 0.0, {}, true},
      ArgumentSample{"false", "boolean", ArgumentKind::Flag, 0.0, {}, false},
      ArgumentSample{"nil", "nil", ArgumentKind::Nil, 0.0, {}, false}};
  return Pool;
}

// What every candidate of one State observed: how often its target ran, which
// converted values that target received, and how many counted native objects
// were constructed. Ranking must leave all of it untouched.
struct CallObservation final {
  std::vector<std::size_t> TargetCalls =
      std::vector<std::size_t>(CandidatePoolSize, 0);
  std::vector<std::vector<std::string>> Delivered =
      std::vector<std::vector<std::string>>(CandidatePoolSize);
  std::size_t NativeConstructions = 0;

  void Deliver(std::size_t Pool, std::vector<std::string> Values) {
    ++TargetCalls[Pool];
    for (std::string &Value : Values)
      Delivered[Pool].push_back(std::move(Value));
  }
};

// One native object only an invoked target constructs.
struct CountedNativeObject final {
  explicit CountedNativeObject(std::size_t &Counter) noexcept
      : CountValue(Counter) {
    ++CountValue;
  }

  std::size_t &CountValue;
};

[[nodiscard]] bool RegisterPoolCandidate(Luna::BindingRegistry &Registry,
                                         std::size_t Pool,
                                         CallObservation &Observed) {
  const int Marker = static_cast<int>(1000U + Pool);
  switch (Pool) {
  case 0:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](int First) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(0, {IntegerText(First)});
                            return Marker;
                          })
        .IsSuccess();
  case 1:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](double First) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(1, {NumberText(First)});
                            return Marker;
                          })
        .IsSuccess();
  case 2:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](std::string First) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(2, {First});
                            return Marker;
                          })
        .IsSuccess();
  case 3:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](bool First) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(3, {FlagText(First)});
                            return Marker;
                          })
        .IsSuccess();
  case 4:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](int First, int Second) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(
                                4, {IntegerText(First), IntegerText(Second)});
                            return Marker;
                          })
        .IsSuccess();
  case 5:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](double First, int Second) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(
                                5, {NumberText(First), IntegerText(Second)});
                            return Marker;
                          })
        .IsSuccess();
  case 6:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](int First, double Second) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(
                                6, {IntegerText(First), NumberText(Second)});
                            return Marker;
                          })
        .IsSuccess();
  case 7:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](std::string First, int Second) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(7, {First, IntegerText(Second)});
                            return Marker;
                          })
        .IsSuccess();
  case 8:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker](double First, double Second) {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(
                                8, {NumberText(First), NumberText(Second)});
                            return Marker;
                          })
        .IsSuccess();
  case 9:
    return Registry
        .RegisterFunction(CallableName,
                          [&Observed, Marker]() {
                            const CountedNativeObject Constructed(
                                Observed.NativeConstructions);
                            static_cast<void>(Constructed);
                            Observed.Deliver(9, {});
                            return Marker;
                          })
        .IsSuccess();
  default:
    break;
  }
  return Registry
      .RegisterFunction(
          CallableName,
          [&Observed, Marker](int First, int Second, int Third) {
            const CountedNativeObject Constructed(Observed.NativeConstructions);
            static_cast<void>(Constructed);
            Observed.Deliver(10, {IntegerText(First), IntegerText(Second),
                                  IntegerText(Third)});
            return Marker;
          })
      .IsSuccess();
}

} // namespace

namespace {

// The independent signature-shape model of one candidate against one call.

struct ModelPositionProbe final {
  bool IsViable = false;
  ConversionRank Rank = ConversionRank::Exact;
  std::string Rejection;
  std::string Delivered;
};

// The canonical rank of one supplied argument against one declared parameter:
// the canonical type of the received value is an exact match, an integral
// number read as a `double` is a safe built-in conversion, and anything else is
// the first deterministic rejection of that candidate.
[[nodiscard]] ModelPositionProbe ModelProbe(ParameterKind Declared,
                                            const ArgumentSample &Argument) {
  ModelPositionProbe Probe;
  const std::string Expected(PublicTypeName(Declared));
  switch (Declared) {
  case ParameterKind::Integer:
    if (Argument.Kind == ArgumentKind::IntegralNumber) {
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::Exact;
      Probe.Delivered = IntegerText(static_cast<int>(Argument.Number));
      return Probe;
    }
    if (Argument.Kind == ArgumentKind::FractionalNumber) {
      Probe.Rejection = "expected an integral value but received";
      return Probe;
    }
    break;
  case ParameterKind::Number:
    if (Argument.Kind == ArgumentKind::IntegralNumber) {
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::SafeBuiltIn;
      Probe.Delivered = NumberText(Argument.Number);
      return Probe;
    }
    if (Argument.Kind == ArgumentKind::FractionalNumber) {
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::Exact;
      Probe.Delivered = NumberText(Argument.Number);
      return Probe;
    }
    break;
  case ParameterKind::Text:
    if (Argument.Kind == ArgumentKind::Text) {
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::Exact;
      Probe.Delivered = Argument.TextValue;
      return Probe;
    }
    break;
  case ParameterKind::Flag:
    if (Argument.Kind == ArgumentKind::Flag) {
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::Exact;
      Probe.Delivered = FlagText(Argument.Flag);
      return Probe;
    }
    break;
  }

  Probe.Rejection =
      "expected " + Expected + " but received " + Argument.LuauTypeName;
  return Probe;
}

[[nodiscard]] std::string
SignatureTextOf(const std::vector<ParameterKind> &Parameters) {
  std::string Text = "(";
  for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
    if (Index != 0)
      Text += ", ";
    Text += PublicTypeName(Parameters[Index]);
  }
  Text += ")";
  return Text;
}

struct ModelCandidate final {
  std::size_t Pool = 0;
  std::string Signature;
  bool IsViable = false;
  CandidateRankSequence Ranks;

  // The exact arity rejection, or the modeled prefix of a type rejection.
  std::string Rejection;

  // How many side-effect-free probes this candidate costs: none when its arity
  // already refused the call, otherwise one per position up to and including
  // its first rejection.
  std::size_t Probes = 0;

  std::vector<std::string> Delivered;
};

[[nodiscard]] ModelCandidate
ModelOne(std::size_t Pool, const std::vector<ArgumentSample> &Arguments) {
  const std::vector<ParameterKind> &Parameters = CandidatePool()[Pool];
  ModelCandidate Modeled;
  Modeled.Pool = Pool;
  Modeled.Signature = SignatureTextOf(Parameters);

  if (Parameters.size() != Arguments.size()) {
    Modeled.Rejection = "expects " + std::to_string(Parameters.size()) +
                        " arguments but received " +
                        std::to_string(Arguments.size());
    return Modeled;
  }

  for (std::size_t Index = 0; Index < Arguments.size(); ++Index) {
    const ModelPositionProbe Probe =
        ModelProbe(Parameters[Index], Arguments[Index]);
    ++Modeled.Probes;
    if (!Probe.IsViable) {
      Modeled.Rejection =
          "argument " + std::to_string(Index + 1) + " " + Probe.Rejection;
      Modeled.Ranks.Positions.clear();
      Modeled.Delivered.clear();
      return Modeled;
    }
    Modeled.Ranks.Positions.push_back(Probe.Rank);
    Modeled.Delivered.push_back(Probe.Delivered);
  }

  // Every pooled candidate declares fixed required parameters only, so an
  // accepted call always matches its arity exactly.
  Modeled.Ranks.Shape = SignatureShapeRank::ExactArity;
  Modeled.IsViable = true;
  return Modeled;
}

struct GeneratedCall final {
  std::vector<std::size_t> Chosen;
  std::vector<ArgumentSample> Arguments;
  std::string ArgumentList;
  std::string ReceivedSummary;
};

struct ModelResolution final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;
  std::size_t SelectedPool = 0;
  std::vector<std::size_t> FrontierPools;
  std::size_t Probes = 0;
  std::vector<ModelCandidate> Candidates;
};

[[nodiscard]] ModelResolution ModelResolve(const GeneratedCall &Call) {
  ModelResolution Resolution;
  std::vector<ViableCandidate> Viable;
  for (const std::size_t Pool : Call.Chosen) {
    ModelCandidate Modeled = ModelOne(Pool, Call.Arguments);
    Resolution.Probes += Modeled.Probes;
    if (Modeled.IsViable)
      Viable.push_back(ViableCandidate{Pool, Modeled.Ranks});
    Resolution.Candidates.push_back(std::move(Modeled));
  }

  const ReferenceSelection Selected = ReferenceSelect(Viable);
  Resolution.Status = Selected.Status;
  Resolution.SelectedPool = Selected.SelectedCandidate;
  Resolution.FrontierPools = Selected.Frontier;
  return Resolution;
}

[[nodiscard]] const ModelCandidate *FindModelled(const ModelResolution &Model,
                                                 std::size_t Pool) {
  for (const ModelCandidate &Candidate : Model.Candidates) {
    if (Candidate.Pool == Pool)
      return &Candidate;
  }
  return nullptr;
}

} // namespace

namespace {

struct CallOutcome final {
  std::string Message;
  std::vector<std::string> CanonicalSignatures;
};

// Everything one generated call must report when it is resolved through the
// real virtual machine: the outcome, the diagnostic, the counted probes and
// committing conversions, the counted target invocations and native
// constructions, and the untouched virtual machine of a refused resolution.
[[nodiscard]] CallOutcome VerifyResolvedCall(Luna::State &Owner,
                                             const CallObservation &Observed,
                                             const GeneratedCall &Call,
                                             const ModelResolution &Model) {
  CallOutcome Outcome;
  const std::string MarkerName = "Marker";
  const std::string CanaryName = "Canary";

  // One qualified name owns one overload set holding every candidate, in
  // canonical order.
  RC_ASSERT(Hooks::OverloadCandidateCount(Owner, CallableName) ==
            Call.Chosen.size());
  RC_ASSERT(Hooks::StagedOverloadCandidateCount(Owner, CallableName) == 0);
  Outcome.CanonicalSignatures =
      Hooks::OverloadCandidateSignatures(Owner, CallableName);
  RC_ASSERT(Outcome.CanonicalSignatures.size() == Call.Chosen.size());

  std::vector<std::size_t> CanonicalPools;
  for (const std::string &Signature : Outcome.CanonicalSignatures) {
    for (const std::size_t Pool : Call.Chosen) {
      const ModelCandidate *Modelled = FindModelled(Model, Pool);
      RC_ASSERT(Modelled != nullptr);
      if (Modelled->Signature == Signature)
        CanonicalPools.push_back(Pool);
    }
  }
  RC_ASSERT(CanonicalPools.size() == Call.Chosen.size());

  RC_ASSERT(Hooks::SetIntegerGlobal(Owner, CanaryName, 11));
  RC_ASSERT(!Hooks::ObserveIntegerGlobal(Owner, MarkerName).has_value());
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(EntryDepth.has_value());

  Luna::Detail::ResetOverloadInstrumentation();
  Luna::Detail::ResetConversionBoundaryDiagnostics();

  const Luna::ExecutionResult Result =
      Owner.Execute(MarkerName + " = " + std::string(CallableName) + "(" +
                    Call.ArgumentList + ")");
  const Luna::Detail::OverloadInstrumentationCounts Counts =
      Luna::Detail::OverloadInstrumentationTotals();
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    Outcome.Message = Diagnostic->Message();

  // Ranking probed exactly the modeled positions and violated nothing.
  RC_ASSERT(Counts.ArgumentProbes == Model.Probes);
  RC_ASSERT(Luna::Detail::ProbeViolationCount() == 0);
  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);

  if (Model.Status == OverloadSelectionStatus::Selected) {
    const ModelCandidate *Selected = FindModelled(Model, Model.SelectedPool);
    RC_ASSERT(Selected != nullptr);
    RC_ASSERT(Result.IsSuccess());
    RC_ASSERT(Outcome.Message.empty());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, MarkerName) ==
              std::optional<int>(static_cast<int>(1000U + Model.SelectedPool)));

    // Exactly one committing conversion per supplied argument, all of them for
    // the selected candidate and none for any other.
    RC_ASSERT(Counts.CommittingArgumentReads == Call.Arguments.size());
    RC_ASSERT(Observed.NativeConstructions == 1);
    for (const std::size_t Pool : Call.Chosen) {
      if (Pool == Model.SelectedPool) {
        RC_ASSERT(Observed.TargetCalls[Pool] == 1);
        RC_ASSERT(Observed.Delivered[Pool] == Selected->Delivered);
        continue;
      }
      RC_ASSERT(Observed.TargetCalls[Pool] == 0);
      RC_ASSERT(Observed.Delivered[Pool].empty());
    }
    return Outcome;
  }

  // A refused resolution converts nothing, invokes nothing, constructs nothing,
  // publishes nothing, and leaves every other value exactly as it was.
  RC_ASSERT(!Result.IsSuccess());
  RC_ASSERT(!Outcome.Message.empty());
  RC_ASSERT(Counts.CommittingArgumentReads == 0);
  RC_ASSERT(Observed.NativeConstructions == 0);
  for (const std::size_t Pool : Call.Chosen) {
    RC_ASSERT(Observed.TargetCalls[Pool] == 0);
    RC_ASSERT(Observed.Delivered[Pool].empty());
  }
  RC_ASSERT(!Hooks::ObserveIntegerGlobal(Owner, MarkerName).has_value());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, CanaryName) ==
            std::optional<int>(11));
  RC_ASSERT(Outcome.Message.find(std::string(CallableName)) !=
            std::string::npos);
  RC_ASSERT(Outcome.Message.find("received " + Call.ReceivedSummary) !=
            std::string::npos);

  if (Model.Status == OverloadSelectionStatus::NoViableCandidate) {
    // Every available signature is listed with its first deterministic
    // rejection.
    RC_ASSERT(Outcome.Message.find("no overload accepts those arguments") !=
              std::string::npos);
    for (const ModelCandidate &Candidate : Model.Candidates) {
      RC_ASSERT(Outcome.Message.find("candidate " + Candidate.Signature + " " +
                                     Candidate.Rejection) != std::string::npos);
    }
    return Outcome;
  }

  // The ambiguity lists exactly the non-dominated signatures, in canonical
  // order, and nothing else.
  RC_ASSERT(Outcome.Message.find("ambiguous") != std::string::npos);
  const std::size_t Listing = Outcome.Message.find("equally viable candidates");
  RC_ASSERT(Listing != std::string::npos);
  const std::string Listed = Outcome.Message.substr(Listing);

  bool SawFirst = false;
  std::size_t Previous = 0;
  for (const std::size_t Pool : CanonicalPools) {
    const ModelCandidate *Candidate = FindModelled(Model, Pool);
    RC_ASSERT(Candidate != nullptr);
    const std::size_t Position = Listed.find(Candidate->Signature);
    const bool OnFrontier =
        std::find(Model.FrontierPools.begin(), Model.FrontierPools.end(),
                  Pool) != Model.FrontierPools.end();
    if (!OnFrontier) {
      RC_ASSERT(Position == std::string::npos);
      continue;
    }
    RC_ASSERT(Position != std::string::npos);
    if (SawFirst)
      RC_ASSERT(Previous < Position);
    Previous = Position;
    SawFirst = true;
  }
  RC_ASSERT(SawFirst);
  return Outcome;
}

// One argument that a declared parameter accepts, so generated calls resolve
// often instead of always refusing.
[[nodiscard]] ArgumentSample MatchingSample(ParameterKind Kind,
                                            ByteCursor &Cursor) {
  const std::vector<ArgumentSample> &Pool = ArgumentPool();
  switch (Kind) {
  case ParameterKind::Integer:
    return Pool[Cursor.Pick(2)];
  case ParameterKind::Number:
    return Cursor.Pick(2) == 0 ? Pool[2] : Pool[Cursor.Pick(2)];
  case ParameterKind::Text:
    return Pool[3 + Cursor.Pick(2)];
  case ParameterKind::Flag:
    return Pool[5 + Cursor.Pick(2)];
  }
  return Pool.back();
}

[[nodiscard]] GeneratedCall GenerateCall(ByteCursor &Cursor) {
  GeneratedCall Call;

  // Three generation modes: a deliberately incomparable frontier, arguments
  // shaped like one chosen candidate, and freely generated arguments, which is
  // what drives arity rejections and no-match diagnostics.
  const std::size_t Mode = Cursor.Pick(3);
  if (Mode == 0) {
    // The two candidates that cannot dominate each other for an integral
    // two-argument call: each is exact in one position and a safe built-in
    // conversion in the other. Every optional extra is either refused by arity
    // or dominated, so none of them can resolve the call either.
    Call.Chosen = {5, 6};
    std::vector<std::size_t> Extras{0, 2, 3, 8, 9, 10};
    const std::size_t ExtraCount = Cursor.Pick(3);
    for (std::size_t Index = 0; Index < ExtraCount; ++Index) {
      const std::size_t Position = Cursor.Pick(Extras.size());
      Call.Chosen.push_back(Extras[Position]);
      Extras.erase(Extras.begin() + static_cast<std::ptrdiff_t>(Position));
    }
    Call.Arguments.push_back(ArgumentPool()[Cursor.Pick(2)]);
    Call.Arguments.push_back(ArgumentPool()[Cursor.Pick(2)]);
  } else {
    std::vector<std::size_t> Available;
    for (std::size_t Index = 0; Index < CandidatePoolSize; ++Index)
      Available.push_back(Index);

    // Two to five candidates. Nothing caps a candidate count; this only keeps
    // one generated case small enough to shrink usefully.
    const std::size_t CandidateCount = 2U + Cursor.Pick(4);
    for (std::size_t Index = 0; Index < CandidateCount; ++Index) {
      const std::size_t Position = Cursor.Pick(Available.size());
      Call.Chosen.push_back(Available[Position]);
      Available.erase(Available.begin() +
                      static_cast<std::ptrdiff_t>(Position));
    }

    if (Mode == 1) {
      const std::vector<ParameterKind> &Shape =
          CandidatePool()[Call.Chosen[Cursor.Pick(Call.Chosen.size())]];
      for (const ParameterKind Kind : Shape)
        Call.Arguments.push_back(MatchingSample(Kind, Cursor));
    } else {
      const std::size_t ArgumentCount = Cursor.Pick(4);
      for (std::size_t Index = 0; Index < ArgumentCount; ++Index)
        Call.Arguments.push_back(
            ArgumentPool()[Cursor.Pick(ArgumentPool().size())]);
    }
  }

  Call.ReceivedSummary = "(";
  for (std::size_t Index = 0; Index < Call.Arguments.size(); ++Index) {
    if (Index != 0) {
      Call.ArgumentList += ", ";
      Call.ReceivedSummary += ", ";
    }
    Call.ArgumentList += Call.Arguments[Index].Literal;
    Call.ReceivedSummary += Call.Arguments[Index].LuauTypeName;
  }
  Call.ReceivedSummary += ")";
  return Call;
}

// The same overload set registered in two different orders resolves the same
// call identically, down to the byte-for-byte diagnostic, because candidate
// order is canonical and never registration order.
void VerifyRegistrationOrderInvariance(ByteCursor &Cursor) {
  const GeneratedCall Call = GenerateCall(Cursor);
  const ModelResolution Model = ModelResolve(Call);

  // The reported distribution shows that generated calls really do reach
  // selection, no match, and the ambiguous frontier.
  RC_TAG(std::string(Luna::Detail::OverloadSelectionStatusText(Model.Status)));

  std::vector<std::size_t> SecondOrder = Call.Chosen;
  for (std::size_t Index = SecondOrder.size(); Index > 1; --Index) {
    const std::size_t Position = Cursor.Pick(Index);
    std::swap(SecondOrder[Index - 1], SecondOrder[Position]);
  }

  CallObservation FirstObserved;
  CallObservation SecondObserved;
  CallOutcome First;
  CallOutcome Second;

  {
    Luna::State Owner;
    RC_ASSERT(Owner.IsReady());
    Luna::BindingRegistry Registry = Owner.Bindings();
    for (const std::size_t Pool : Call.Chosen)
      RC_ASSERT(RegisterPoolCandidate(Registry, Pool, FirstObserved));
    First = VerifyResolvedCall(Owner, FirstObserved, Call, Model);
  }

  {
    Luna::State Owner;
    RC_ASSERT(Owner.IsReady());
    Luna::BindingRegistry Registry = Owner.Bindings();
    for (const std::size_t Pool : SecondOrder)
      RC_ASSERT(RegisterPoolCandidate(Registry, Pool, SecondObserved));
    Second = VerifyResolvedCall(Owner, SecondObserved, Call, Model);
  }

  RC_ASSERT(First.CanonicalSignatures == Second.CanonicalSignatures);
  RC_ASSERT(First.Message == Second.Message);
  RC_ASSERT(FirstObserved.TargetCalls == SecondObserved.TargetCalls);
  RC_ASSERT(FirstObserved.Delivered == SecondObserved.Delivered);
  RC_ASSERT(FirstObserved.NativeConstructions ==
            SecondObserved.NativeConstructions);
}

} // namespace

int RunParetoOverloadResolutionProperties() {
  // **Validates: Requirements 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 6.10, 6.11**
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 24: Overload resolution agrees with Pareto dominance
  const bool Passed = rc::check(
      // clang-format on
      "Overload resolution agrees with Pareto dominance",
      [](const std::vector<std::uint8_t> &RankShape,
         const std::vector<std::uint8_t> &CallShape) {
        ByteCursor Ranks(RankShape);
        VerifyDominanceModel(Ranks);

        ByteCursor Calls(CallShape);
        VerifyRegistrationOrderInvariance(Calls);
      });
  return Passed ? 0 : 1;
}
