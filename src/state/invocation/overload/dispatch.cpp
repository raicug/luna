// clang-format off
#include "state/invocation/overload/dispatch.hpp"

#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/invocation/members/receiver.hpp"
#include "state/invocation/overload/probe.hpp"
#include "state/invocation/overload/resolution.hpp"
#include "state/registration/record.hpp"
#include "state/type/conversion_frame.hpp"
#include "state/type/owned_value_bridge.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

struct DeclaredShape final {
  std::span<const ParameterDescriptor> Parameters;
  bool IsRich = false;
};

[[nodiscard]] DeclaredShape ShapeOf(const CallableMetadata &Metadata) {
  DeclaredShape Shape;
  Shape.IsRich = Metadata.HasRichParameters();
  Shape.Parameters = Metadata.Parameters();
  return Shape;
}

struct ConsideredCandidate final {
  std::size_t Index = 0;
  const CallableSignatureDescriptor *Signature = nullptr;
  DeclaredShape Shape;
  std::string SignatureText;
  bool IsViable = false;
  CandidateRankSequence Ranks;
  std::string Rejection;
};

[[nodiscard]] std::string TypeText(const TypeGeneration &Types,
                                   const TypeDescriptor &Type) {
  const std::string_view Public = Types.PublicNameOf(Type);
  if (!Public.empty())
    return std::string(Public);
  return CanonicalTypeText(Type);
}

[[nodiscard]] std::string
DescribeSignature(const TypeGeneration &Types,
                  const CallableSignatureDescriptor &Signature) {
  std::string Text = "(";
  for (std::size_t Index = 0; Index < Signature.ParameterTypes.size();
       ++Index) {
    if (Index != 0)
      Text += ", ";
    Text += TypeText(Types, Signature.ParameterTypes[Index]);
    if (Index >= Signature.RequiredParameterCount)
      Text += " (optional)";
  }
  if (Signature.IsVariadic) {
    if (!Signature.ParameterTypes.empty())
      Text += ", ";
    Text += "...";
  }
  Text += ")";
  return Text;
}

[[nodiscard]] std::string DescribeReceived(lua_State *State, int ArgumentBase,
                                           int Count) {
  std::string Text = "(";
  for (int Position = 1; Position <= Count; ++Position) {
    if (Position != 1)
      Text += ", ";
    Text += ReceivedTypeName(State, ArgumentBase + Position - 1);
  }
  Text += ")";
  return Text;
}

struct ArityWindow final {
  std::size_t Minimum = 0;
  std::size_t Maximum = 0;
  bool IsUnbounded = false;
};

[[nodiscard]] ArityWindow
DescribeArity(const CallableSignatureDescriptor &Signature,
              const DeclaredShape &Shape) {
  if (Shape.IsRich) {
    const ParameterArity Declared = ArityOf(Shape.Parameters);
    ArityWindow Window;
    Window.Minimum = Declared.Minimum;
    Window.Maximum = Declared.FixedCount;
    Window.IsUnbounded = Declared.IsVariadic;
    return Window;
  }

  ArityWindow Window;
  Window.Maximum = Signature.ParameterTypes.size();
  Window.Minimum = Signature.RequiredParameterCount <= Window.Maximum
                       ? Signature.RequiredParameterCount
                       : Window.Maximum;
  Window.IsUnbounded = Signature.IsVariadic;
  return Window;
}

[[nodiscard]] std::string DescribeArityRejection(const ArityWindow &Window,
                                                 std::size_t Received) {
  const std::string Got = std::to_string(Received);
  if (Window.IsUnbounded)
    return "expects at least " + std::to_string(Window.Minimum) +
           " arguments but received " + Got;
  if (Window.Minimum != Window.Maximum)
    return "expects between " + std::to_string(Window.Minimum) + " and " +
           std::to_string(Window.Maximum) + " arguments but received " + Got;
  return "expects " + std::to_string(Window.Maximum) +
         " arguments but received " + Got;
}

[[nodiscard]] const TypeDescriptor *
ParameterTypeAt(const CallableSignatureDescriptor &Signature,
                std::size_t Position) {
  if (Position < Signature.ParameterTypes.size())
    return &Signature.ParameterTypes[Position];
  return nullptr;
}

[[nodiscard]] ArgumentProbe
ProbeConvertedPosition(lua_State *State, const ParameterDescriptor &Parameter,
                       int StackIndex) {
  const ConvertedParameterShape *Declared = Parameter.ConvertedSignature();
  if (!Declared || !Declared->Probe) {
    ArgumentProbe Probe;
    Probe.Rejection = "names no canonical converted type";
    return Probe;
  }

  const OwnedValue Source = BuildOwnedValueFromStack(State, StackIndex);
  ConversionFrame Frame(Luna::ConversionDirection::Read, std::string(), 0);
  const ValueView Root = Frame.Open(Source);
  const ConversionContext Probing = Frame.ProbeContext();

  const ConversionProbe Probed = Declared->Probe(Root, Probing);
  ArgumentProbe Probe;
  Probe.IsViable = Probed.IsViable;
  Probe.Rank = Probed.Rank;
  Probe.Rejection = Probed.Rejection;
  return Probe;
}

[[nodiscard]] ArgumentProbe
ProbeInstancePosition(const TypeGeneration &Types, lua_State *State,
                      const ParameterDescriptor &Parameter, int StackIndex) {
  const StableTypeKey *Key = Parameter.InstanceKey();
  if (!Key || !Key->IsValid()) {
    ArgumentProbe Probe;
    Probe.Rejection = "names no registered class";
    return Probe;
  }
  return ProbeArgument(Types, State, StackIndex,
                       TypeDescriptor::ForClass(*Key));
}

[[nodiscard]] ArgumentProbe
ProbeDeclaredPosition(const TypeGeneration &Types, lua_State *State,
                      const DeclaredShape &Shape, std::size_t FixedCount,
                      std::size_t Position, int ArgumentBase) {
  const int StackIndex = static_cast<int>(Position) + ArgumentBase;
  if (Position < FixedCount) {
    const ParameterDescriptor &Parameter = Shape.Parameters[Position];
    if (Parameter.IsConverted())
      return ProbeConvertedPosition(State, Parameter, StackIndex);
    if (Parameter.IsInstance())
      return ProbeInstancePosition(Types, State, Parameter, StackIndex);
    const ValueKind *Kind = Parameter.Kind();
    if (!Kind) {
      ArgumentProbe Probe;
      Probe.Rejection = "names no canonical Luna type";
      return Probe;
    }
    if (Parameter.AcceptsNil() && lua_type(State, StackIndex) == LUA_TNIL) {
      ArgumentProbe Probe;
      Probe.IsViable = true;
      Probe.Rank = ConversionRank::Exact;
      return Probe;
    }
    return ProbeArgument(Types, State, StackIndex, CanonicalValueType(*Kind));
  }

  switch (lua_type(State, StackIndex)) {
  case LUA_TNIL:
  case LUA_TBOOLEAN:
  case LUA_TNUMBER:
  case LUA_TSTRING:
    break;
  default: {
    ArgumentProbe Probe;
    Probe.Rejection = "expected a boolean, number, string, or nil variadic "
                      "value but received " +
                      ReceivedTypeName(State, StackIndex);
    return Probe;
  }
  }

  ArgumentProbe Probe;
  Probe.IsViable = true;
  Probe.Rank = ConversionRank::Exact;
  return Probe;
}

[[nodiscard]] SignatureShapeRank DescribeShape(const ArityWindow &Window,
                                               std::size_t Received) {
  if (Window.IsUnbounded && Received >= Window.Maximum)
    return SignatureShapeRank::VariadicConsumption;
  if (Received < Window.Maximum)
    return SignatureShapeRank::OmittedParameters;
  return SignatureShapeRank::ExactArity;
}

[[nodiscard]] bool RankReceiver(const TypeGeneration &Types,
                                const InstanceReceiver *Receiver,
                                ConsideredCandidate &Candidate) {
  const CallableSignatureDescriptor &Signature = *Candidate.Signature;
  if (!Signature.ReceiverType) {
    if (!Receiver)
      return true;
    Candidate.Rejection = "declares no instance receiver";
    return false;
  }

  if (!Receiver) {
    Candidate.Rejection =
        ReceiverAbsenceRejectionText(Types, *Signature.ReceiverType);
    return false;
  }

  if (!Signature.ReceiverIsConst && !Receiver->PermitsMutation()) {
    Candidate.Rejection =
        ReceiverConstRejectionText(Types, *Signature.ReceiverType);
    return false;
  }

  const bool IsExact = Signature.ReceiverIsConst != Receiver->PermitsMutation();
  Candidate.Ranks.Positions.push_back(IsExact ? ConversionRank::Exact
                                              : ConversionRank::SafeBuiltIn);
  return true;
}

void Consider(const TypeGeneration &Types, lua_State *State,
              std::size_t Received, int ArgumentBase,
              const InstanceReceiver *Receiver,
              ConsideredCandidate &Candidate) {
  const CallableSignatureDescriptor &Signature = *Candidate.Signature;

  Candidate.Ranks.Positions.reserve(Received + 1);
  if (!RankReceiver(Types, Receiver, Candidate)) {
    Candidate.Ranks.Positions.clear();
    return;
  }

  const ArityWindow Window = DescribeArity(Signature, Candidate.Shape);
  if (Received < Window.Minimum ||
      (!Window.IsUnbounded && Received > Window.Maximum)) {
    Candidate.Rejection = DescribeArityRejection(Window, Received);
    Candidate.Ranks.Positions.clear();
    return;
  }

  for (std::size_t Position = 0; Position < Received; ++Position) {
    ArgumentProbe Probe;
    if (Candidate.Shape.IsRich) {
      Probe = ProbeDeclaredPosition(Types, State, Candidate.Shape,
                                    Window.Maximum, Position, ArgumentBase);
    } else if (const TypeDescriptor *Parameter =
                   ParameterTypeAt(Signature, Position)) {
      Probe = ProbeArgument(
          Types, State, static_cast<int>(Position) + ArgumentBase, *Parameter);
    } else {
      Candidate.Rejection =
          "expects " + std::to_string(Signature.ParameterTypes.size()) +
          " arguments but received " + std::to_string(Received);
      Candidate.Ranks.Positions.clear();
      return;
    }

    if (!Probe.IsViable) {
      Candidate.Rejection =
          "argument " + std::to_string(Position + 1) + " " + Probe.Rejection;
      Candidate.Ranks.Positions.clear();
      return;
    }
    Candidate.Ranks.Positions.push_back(Probe.Rank);
  }

  Candidate.Ranks.Shape = DescribeShape(Window, Received);
  Candidate.IsViable = true;
}

[[nodiscard]] bool DeclaresReceiver(const BindingRecord &Record) {
  for (std::size_t Index = 0; Index < Record.CandidateCount(); ++Index) {
    const OverloadCandidate *Candidate = Record.CandidateAt(Index);
    if (Candidate && Candidate->IsCommitted &&
        Candidate->Signature.ReceiverType)
      return true;
  }
  return false;
}

[[nodiscard]] std::string SetSubjectText(const BindingRecord &Record) {
  const ConversionSubject Subject =
      SubjectForCallable(Record.GlobalName(), DeclaresReceiver(Record));
  return DescribeConversionSubject(Subject);
}

[[nodiscard]] std::string SetContextText(const BindingRecord &Record) {
  const ConversionSubject Subject =
      SubjectForCallable(Record.GlobalName(), DeclaresReceiver(Record));
  return DescribeConversionSubjectContext(Subject);
}

[[nodiscard]] std::string
NoMatchDiagnostic(const BindingRecord &Record, const std::string &ReceivedText,
                  const std::vector<ConsideredCandidate> &Candidates) {
  std::string Message = SetSubjectText(Record) + " received " + ReceivedText +
                        " and no overload accepts those arguments:";
  for (const ConsideredCandidate &Candidate : Candidates) {
    Message += " candidate " + Candidate.SignatureText + " " +
               Candidate.Rejection + ";";
  }
  if (!Message.empty() && Message.back() == ';')
    Message.back() = '.';
  return Message;
}

[[nodiscard]] std::string
AmbiguityDiagnostic(const BindingRecord &Record,
                    const std::string &ReceivedText,
                    const std::vector<ConsideredCandidate> &Candidates,
                    const std::vector<std::size_t> &Frontier) {
  std::string Message = SetSubjectText(Record) + " received " + ReceivedText +
                        " and the call is ambiguous between the equally "
                        "viable candidates";
  for (const std::size_t Selected : Frontier) {
    for (const ConsideredCandidate &Candidate : Candidates) {
      if (Candidate.Index != Selected)
        continue;
      Message += " " + Candidate.SignatureText + ",";
      break;
    }
  }
  if (!Message.empty() && Message.back() == ',')
    Message.back() = '.';
  return Message;
}

} // namespace

OverloadDispatchResult ResolveOverloadedCall(const BindingRecord &Record,
                                             lua_State *State,
                                             const TypeGeneration &Types,
                                             const InstanceReceiver *Receiver) {
  OverloadDispatchResult Result;
  if (!State) {
    Result.Diagnostic = "Internal error: the argument stack is unavailable for "
                        "callable '" +
                        Record.GlobalName() + "'.";
    return Result;
  }

  const int ArgumentBase = Receiver != nullptr ? 2 : 1;
  const int Supplied = lua_gettop(State) - (ArgumentBase - 1);
  const std::size_t Received =
      Supplied > 0 ? static_cast<std::size_t>(Supplied) : 0;

  std::vector<ConsideredCandidate> Candidates;
  Candidates.reserve(Record.CandidateCount());
  for (std::size_t Index = 0; Index < Record.CandidateCount(); ++Index) {
    const OverloadCandidate *Candidate = Record.CandidateAt(Index);
    if (!Candidate || !Candidate->IsCommitted)
      continue;
    ConsideredCandidate Considered;
    Considered.Index = Index;
    Considered.Signature = &Candidate->Signature;
    Considered.Shape = ShapeOf(Candidate->Descriptor.Metadata());
    Considered.SignatureText = DescribeSignature(Types, Candidate->Signature);
    Candidates.push_back(std::move(Considered));
  }

  Result.Considered = Candidates.size();
  if (Candidates.empty()) {
    Result.Diagnostic = "Internal error: " + SetContextText(Record) +
                        " has no available overload candidate.";
    return Result;
  }

  for (ConsideredCandidate &Candidate : Candidates)
    Consider(Types, State, Received, ArgumentBase, Receiver, Candidate);

  std::vector<ViableCandidate> Viable;
  Viable.reserve(Candidates.size());
  for (const ConsideredCandidate &Candidate : Candidates) {
    if (Candidate.IsViable)
      Viable.push_back(ViableCandidate{Candidate.Index, Candidate.Ranks});
  }
  Result.Viable = Viable.size();

  const OverloadSelection Selection = SelectByDominance(Viable);
  Result.Status = Selection.Status;
  const std::string ReceivedText =
      DescribeReceived(State, ArgumentBase, static_cast<int>(Received));
  switch (Selection.Status) {
  case OverloadSelectionStatus::Selected:
    Result.SelectedCandidate = Selection.SelectedCandidate;
    return Result;
  case OverloadSelectionStatus::NoViableCandidate:
    Result.Diagnostic = NoMatchDiagnostic(Record, ReceivedText, Candidates);
    return Result;
  case OverloadSelectionStatus::Ambiguous:
    Result.Diagnostic = AmbiguityDiagnostic(Record, ReceivedText, Candidates,
                                            Selection.Frontier);
    return Result;
  }

  Result.Diagnostic = "Internal error: " + SetContextText(Record) +
                      " could not resolve its overload set.";
  return Result;
}

} // namespace Luna::Detail
