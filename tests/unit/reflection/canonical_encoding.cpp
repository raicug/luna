// clang-format off
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/detail/canonical_type.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/canonical_encoding.hpp"
#include "state/identity/digest.hpp"
#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CallableSignatureDescriptor;
using Luna::Detail::CanonicalDigest;
using Luna::Detail::EncodeCanonicalSymbol;
using Luna::Detail::EncodeCanonicalType;
using Luna::Detail::ModuleProvenance;
using Luna::Detail::SymbolDescriptor;
using Luna::Detail::SymbolIdentityRegistry;
using Luna::Detail::TypeIdentityRegistry;

enum class Color { Red, Green };
struct Widget {
  int Field = 0;
};

using KeyList = std::vector<Luna::StableTypeKey>;

template <class Type>
[[nodiscard]] Luna::TypeDescriptor Canonical(const KeyList &Keys = {}) {
  return Luna::Detail::CanonicalDescriptorFor<Type>(Keys);
}

[[nodiscard]] Luna::TypeDescriptor WidgetType() {
  return Canonical<Widget>(KeyList{Luna::StableTypeKey("studio.ui.Widget")});
}

[[nodiscard]] Luna::TypeDescriptor ColorType() {
  return Canonical<Color>(KeyList{Luna::StableTypeKey("studio.ui.Color")});
}

[[nodiscard]] std::string DigestText(std::string_view Message) {
  std::vector<std::uint8_t> Bytes;
  Bytes.reserve(Message.size());
  for (const char Character : Message)
    Bytes.push_back(static_cast<std::uint8_t>(Character));
  return Luna::TypeId::FromBytes(CanonicalDigest::Compute(Bytes)).ToString();
}

[[nodiscard]] CallableSignatureDescriptor
FreeSignature(Luna::TypeDescriptor ReturnType,
              std::vector<Luna::TypeDescriptor> Parameters) {
  CallableSignatureDescriptor Signature;
  Signature.ReturnType = std::move(ReturnType);
  Signature.ParameterTypes = std::move(Parameters);
  Signature.RequiredParameterCount = Signature.ParameterTypes.size();
  return Signature;
}

[[nodiscard]] SymbolDescriptor
Candidate(std::string QualifiedName, Luna::SymbolId Parent,
          CallableSignatureDescriptor Signature) {
  return Luna::Detail::MakeCallableCandidateSymbol(
      Luna::SymbolKind::FunctionCandidate, std::move(QualifiedName), Parent,
      std::move(Signature));
}

[[nodiscard]] bool VerifyPinnedDigestVectors() {
  if (CanonicalDigest::AlgorithmVersion != 1 ||
      CanonicalDigest::ByteCount != Luna::TypeId::ByteCount)
    return false;
  if (DigestText("") !=
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    return false;
  if (DigestText("abc") !=
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    return false;
  if (DigestText("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    return false;

  std::vector<std::uint8_t> Long(1000000, static_cast<std::uint8_t>('a'));
  const std::string LongText =
      Luna::TypeId::FromBytes(CanonicalDigest::Compute(Long)).ToString();
  return LongText ==
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
}

[[nodiscard]] bool VerifyVersionedRootTags() {
  const std::vector<std::uint8_t> TypeBytes =
      EncodeCanonicalType(Canonical<int>());
  const std::vector<std::uint8_t> SymbolBytes =
      EncodeCanonicalSymbol(Luna::Detail::MakeScopeSymbol(
          Luna::SymbolKind::Namespace, "Studio", Luna::SymbolId()));
  if (TypeBytes.size() < 9 || SymbolBytes.size() < 9)
    return false;
  for (std::size_t Index = 0; Index < 7; ++Index) {
    if (TypeBytes[Index] != 0 || SymbolBytes[Index] != 0)
      return false;
  }
  if (TypeBytes[7] !=
          static_cast<std::uint8_t>(Luna::Detail::CanonicalSchemaVersion) ||
      SymbolBytes[7] !=
          static_cast<std::uint8_t>(Luna::Detail::CanonicalSchemaVersion))
    return false;
  if (TypeBytes[8] !=
          static_cast<std::uint8_t>(Luna::Detail::CanonicalDomain::Type) ||
      SymbolBytes[8] !=
          static_cast<std::uint8_t>(Luna::Detail::CanonicalDomain::Symbol))
    return false;

  return TypeBytes != SymbolBytes;
}

[[nodiscard]] bool VerifyLengthDelimitedFraming() {
  const Luna::SymbolId Root;
  const SymbolDescriptor SplitLeft =
      Luna::Detail::MakeEnumeratorAliasSymbol("ab", Root, ColorType(), "c");
  const SymbolDescriptor SplitRight =
      Luna::Detail::MakeEnumeratorAliasSymbol("a", Root, ColorType(), "bc");
  if (EncodeCanonicalSymbol(SplitLeft) == EncodeCanonicalSymbol(SplitRight))
    return false;
  if (SymbolIdentityRegistry::ComputeIdentity(SplitLeft) ==
      SymbolIdentityRegistry::ComputeIdentity(SplitRight))
    return false;

  const Luna::TypeDescriptor Flat = Canonical<std::tuple<int, int>>();
  const Luna::TypeDescriptor Nested =
      Canonical<std::tuple<std::tuple<int, int>>>();
  if (EncodeCanonicalType(Flat) == EncodeCanonicalType(Nested))
    return false;
  if (TypeIdentityRegistry::ComputeIdentity(Flat) ==
      TypeIdentityRegistry::ComputeIdentity(Nested))
    return false;

  const SymbolDescriptor OneParameter = Candidate(
      "Studio.Add", Root, FreeSignature(Canonical<int>(), {Canonical<int>()}));
  const SymbolDescriptor TwoParameters = Candidate(
      "Studio.Add", Root,
      FreeSignature(Canonical<int>(), {Canonical<int>(), Canonical<int>()}));
  return SymbolIdentityRegistry::ComputeIdentity(OneParameter) !=
         SymbolIdentityRegistry::ComputeIdentity(TwoParameters);
}

[[nodiscard]] bool VerifyStableTypeResolution() {
  TypeIdentityRegistry First;
  TypeIdentityRegistry Second;

  const std::array Descriptors{Canonical<int>(),
                               Canonical<std::vector<int>>(),
                               WidgetType(),
                               Canonical<std::map<Color, Widget>>(KeyList{
                                   Luna::StableTypeKey("studio.ui.Color"),
                                   Luna::StableTypeKey("studio.ui.Widget")}),
                               Canonical<const int *>(),
                               Canonical<int[4]>()};

  std::vector<Luna::TypeId> Forward;
  for (const Luna::TypeDescriptor &Descriptor : Descriptors) {
    const auto Resolution = First.Resolve(Descriptor);
    if (!Resolution.IsSuccess() || Resolution.Diagnostic)
      return false;
    if (!Resolution.Value->IsValid())
      return false;
    if (TypeIdentityRegistry::ComputeIdentity(Descriptor) != Resolution.Value)
      return false;
    Forward.push_back(*Resolution.Value);
  }

  for (std::size_t Index = Descriptors.size(); Index > 0; --Index) {
    const auto Resolution = Second.Resolve(Descriptors[Index - 1]);
    if (!Resolution.IsSuccess() || *Resolution.Value != Forward[Index - 1])
      return false;
  }
  if (First.Size() != Descriptors.size() || Second.Size() != First.Size())
    return false;

  const auto Repeated = First.Resolve(Canonical<const int &>());
  if (!Repeated.IsSuccess() || *Repeated.Value != Forward[0] ||
      First.Size() != Descriptors.size())
    return false;

  const Luna::TypeDescriptor *Stored = First.Find(Forward[2]);
  if (Stored == nullptr || *Stored != WidgetType())
    return false;
  if (First.Find(Luna::TypeId()) != nullptr)
    return false;
  if (!First.Matches(Forward[2], WidgetType()) ||
      First.Matches(Forward[2], Canonical<int>()) ||
      First.Matches(Luna::TypeId(), Canonical<int>()))
    return false;

  for (std::size_t Left = 0; Left < Forward.size(); ++Left) {
    for (std::size_t Right = Left + 1; Right < Forward.size(); ++Right) {
      if (Forward[Left] == Forward[Right])
        return false;
    }
  }
  return true;
}

[[nodiscard]] bool VerifySymbolIdentityDerivation() {
  SymbolIdentityRegistry Registry;
  const Luna::SymbolId Root;

  const auto Scope = Registry.Resolve(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Studio", Root));
  if (!Scope.IsSuccess())
    return false;

  const auto OverloadSet = Registry.Resolve(
      Luna::Detail::MakeOverloadSetSymbol("Studio.Add", *Scope.Value));
  const auto IntegerCandidate = Registry.Resolve(
      Candidate("Studio.Add", *Scope.Value,
                FreeSignature(Canonical<int>(), {Canonical<int>()})));
  const auto DoubleCandidate = Registry.Resolve(
      Candidate("Studio.Add", *Scope.Value,
                FreeSignature(Canonical<double>(), {Canonical<int>()})));
  if (!OverloadSet.IsSuccess() || !IntegerCandidate.IsSuccess() ||
      !DoubleCandidate.IsSuccess())
    return false;
  if (*OverloadSet.Value == *IntegerCandidate.Value ||
      *IntegerCandidate.Value == *DoubleCandidate.Value)
    return false;

  const auto OtherScope = Registry.Resolve(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Engine", Root));
  const auto Rehomed = Registry.Resolve(
      Candidate("Studio.Add", *OtherScope.Value,
                FreeSignature(Canonical<int>(), {Canonical<int>()})));
  if (!Rehomed.IsSuccess() || *Rehomed.Value == *IntegerCandidate.Value)
    return false;

  CallableSignatureDescriptor Variadic =
      FreeSignature(Canonical<int>(), {Canonical<int>()});
  Variadic.IsVariadic = true;
  const auto VariadicCandidate =
      Registry.Resolve(Candidate("Studio.Add", *Scope.Value, Variadic));
  CallableSignatureDescriptor Defaulted =
      FreeSignature(Canonical<int>(), {Canonical<int>()});
  Defaulted.RequiredParameterCount = 0;
  const auto DefaultedCandidate =
      Registry.Resolve(Candidate("Studio.Add", *Scope.Value, Defaulted));
  if (!VariadicCandidate.IsSuccess() || !DefaultedCandidate.IsSuccess())
    return false;
  if (*VariadicCandidate.Value == *IntegerCandidate.Value ||
      *DefaultedCandidate.Value == *IntegerCandidate.Value ||
      *VariadicCandidate.Value == *DefaultedCandidate.Value)
    return false;

  CallableSignatureDescriptor MethodSignature =
      FreeSignature(Canonical<int>(), {});
  MethodSignature.ReceiverType = WidgetType();
  const auto Method = Registry.Resolve(Luna::Detail::MakeClassMemberSymbol(
      Luna::SymbolKind::Method, "studio.ui.Widget.Size", *Scope.Value,
      WidgetType(), MethodSignature));
  const auto InheritedView =
      Registry.Resolve(Luna::Detail::MakeClassMemberSymbol(
          Luna::SymbolKind::Method, "studio.ui.Widget.Size", *Scope.Value,
          WidgetType(), MethodSignature));
  CallableSignatureDescriptor OtherReceiver = MethodSignature;
  OtherReceiver.ReceiverIsConst = true;
  const auto ConstMethod = Registry.Resolve(Luna::Detail::MakeClassMemberSymbol(
      Luna::SymbolKind::Method, "studio.ui.Widget.Size", *Scope.Value,
      WidgetType(), OtherReceiver));
  const auto Field = Registry.Resolve(Luna::Detail::MakeClassMemberSymbol(
      Luna::SymbolKind::Field, "studio.ui.Widget.Size", *Scope.Value,
      WidgetType()));
  if (!Method.IsSuccess() || !InheritedView.IsSuccess() ||
      !ConstMethod.IsSuccess() || !Field.IsSuccess())
    return false;
  if (*Method.Value != *InheritedView.Value)
    return false;
  if (*Method.Value == *ConstMethod.Value || *Method.Value == *Field.Value)
    return false;

  const auto Alias = Registry.Resolve(Luna::Detail::MakeEnumeratorAliasSymbol(
      "studio.ui.Color.Crimson", *Scope.Value, ColorType(), "Red"));
  const auto OtherAlias =
      Registry.Resolve(Luna::Detail::MakeEnumeratorAliasSymbol(
          "studio.ui.Color.Crimson", *Scope.Value, ColorType(), "Green"));
  if (!Alias.IsSuccess() || !OtherAlias.IsSuccess() ||
      *Alias.Value == *OtherAlias.Value)
    return false;

  const auto Module = Registry.Resolve(Luna::Detail::MakeModuleSymbol(
      "studio.physics", Root, ModuleProvenance{"studio.physics", "1.2.0"}));
  const auto NewerModule = Registry.Resolve(Luna::Detail::MakeModuleSymbol(
      "studio.physics", Root, ModuleProvenance{"studio.physics", "1.3.0"}));
  const auto Owned = Registry.Resolve(Luna::Detail::WithModuleProvenance(
      Candidate("studio.physics.Step", *Module.Value,
                FreeSignature(Canonical<void>(), {})),
      ModuleProvenance{"studio.physics", "1.2.0"}));
  const auto OwnedByNewer = Registry.Resolve(Luna::Detail::WithModuleProvenance(
      Candidate("studio.physics.Step", *Module.Value,
                FreeSignature(Canonical<void>(), {})),
      ModuleProvenance{"studio.physics", "1.3.0"}));
  if (!Module.IsSuccess() || !NewerModule.IsSuccess() || !Owned.IsSuccess() ||
      !OwnedByNewer.IsSuccess())
    return false;
  return *Module.Value != *NewerModule.Value &&
         *Owned.Value != *OwnedByNewer.Value;
}

[[nodiscard]] bool VerifyIncompleteDescriptorRejection() {
  TypeIdentityRegistry Types;
  SymbolIdentityRegistry Symbols;
  const Luna::SymbolId Root;

  const auto Unsupported = Types.Resolve(Luna::TypeDescriptor::Unsupported());
  if (Unsupported.IsSuccess() || !Unsupported.Diagnostic)
    return false;
  if (Unsupported.Diagnostic->Category() != Luna::ErrorCategory::Internal ||
      Unsupported.Diagnostic->Message().empty())
    return false;
  if (Types.Size() != 0 ||
      TypeIdentityRegistry::ComputeIdentity(Luna::TypeDescriptor()).has_value())
    return false;
  if (!EncodeCanonicalType(Luna::TypeDescriptor::Unsupported()).empty())
    return false;

  std::vector<SymbolDescriptor> Incomplete;
  Incomplete.push_back(
      Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Namespace, "", Root));
  Incomplete.push_back(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Studio..Physics", Root));
  Incomplete.push_back(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "1Studio", Root));
  Incomplete.push_back(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::FunctionCandidate, "Studio.Add", Root));
  Incomplete.push_back(
      Candidate("Studio.Add", Root,
                FreeSignature(Canonical<int>(), {Canonical<long>()})));
  Incomplete.push_back(Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Field,
                                                     "Widget.Size", Root));
  Incomplete.push_back(Luna::Detail::MakeEnumeratorAliasSymbol(
      "studio.ui.Color.Crimson", Root, ColorType(), ""));
  Incomplete.push_back(Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Module,
                                                     "studio.physics", Root));
  Incomplete.push_back(Luna::Detail::MakeCallableCandidateSymbol(
      Luna::SymbolKind::Namespace, "Studio", Root,
      FreeSignature(Canonical<int>(), {})));

  for (const SymbolDescriptor &Descriptor : Incomplete) {
    if (Descriptor.IsValid())
      return false;
    if (!EncodeCanonicalSymbol(Descriptor).empty())
      return false;
    const auto Resolution = Symbols.Resolve(Descriptor);
    if (Resolution.IsSuccess() || !Resolution.Diagnostic)
      return false;
    if (Resolution.Diagnostic->Category() != Luna::ErrorCategory::Internal ||
        Resolution.Diagnostic->Message().empty())
      return false;
  }
  return Symbols.Size() == 0;
}

[[nodiscard]] bool VerifyCollisionRejection() {
  const Luna::TypeDescriptor FirstType = Canonical<int>();
  const Luna::TypeDescriptor SecondType = Canonical<std::vector<int>>();

  TypeIdentityRegistry Types;
  Types.CollisionInjection().Inject(2);
  if (Types.CollisionInjection().Pending() != 2)
    return false;

  const auto FirstResolution = Types.Resolve(FirstType);
  if (!FirstResolution.IsSuccess() ||
      *FirstResolution.Value !=
          Luna::TypeId::FromBytes(
              Luna::Detail::IdentityCollisionInjector::SharedIdentityBytes()))
    return false;

  const auto Collided = Types.Resolve(SecondType);
  if (Collided.IsSuccess() || !Collided.Diagnostic)
    return false;
  if (Collided.Diagnostic->Category() != Luna::ErrorCategory::Internal ||
      Collided.Diagnostic->Message().empty())
    return false;
  if (Types.Size() != 1)
    return false;
  const Luna::TypeDescriptor *Stored = Types.Find(*FirstResolution.Value);
  if (Stored == nullptr || *Stored != FirstType)
    return false;
  if (Types.CollisionInjection().Pending() != 0)
    return false;

  const auto Recovered = Types.Resolve(SecondType);
  if (!Recovered.IsSuccess() ||
      Recovered.Value != TypeIdentityRegistry::ComputeIdentity(SecondType) ||
      Types.Size() != 2)
    return false;

  TypeIdentityRegistry Idempotent;
  Idempotent.CollisionInjection().Inject(2);
  const auto Once = Idempotent.Resolve(FirstType);
  const auto Twice = Idempotent.Resolve(Canonical<const int &>());
  if (!Once.IsSuccess() || !Twice.IsSuccess() || Once.Value != Twice.Value ||
      Idempotent.Size() != 1)
    return false;

  TypeIdentityRegistry Replay;
  Replay.CollisionInjection().Inject(2);
  if (!Replay.Resolve(FirstType).IsSuccess())
    return false;
  const auto ReplayCollided = Replay.Resolve(SecondType);
  if (!ReplayCollided.Diagnostic ||
      ReplayCollided.Diagnostic->Message() != Collided.Diagnostic->Message())
    return false;

  SymbolIdentityRegistry Symbols;
  const Luna::SymbolId Root;
  Symbols.CollisionInjection().Inject(2);
  const auto Scope = Symbols.Resolve(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Studio", Root));
  const auto Clashing = Symbols.Resolve(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Engine", Root));
  if (!Scope.IsSuccess() || Clashing.IsSuccess() || !Clashing.Diagnostic)
    return false;
  if (Clashing.Diagnostic->Category() != Luna::ErrorCategory::Internal ||
      Clashing.Diagnostic->Message().empty())
    return false;
  if (Symbols.Size() != 1)
    return false;

  SymbolIdentityRegistry Targeted;
  const SymbolDescriptor Namespace = Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Studio", Root);
  const auto Natural = Targeted.Resolve(Namespace);
  if (!Natural.IsSuccess())
    return false;
  Targeted.CollisionInjection().InjectIdentity(Natural.Value->Bytes());
  const auto Forced = Targeted.Resolve(Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Engine", Root));
  return !Forced.IsSuccess() && Forced.Diagnostic &&
         Forced.Diagnostic->Category() == Luna::ErrorCategory::Internal &&
         Targeted.Size() == 1;
}

} // namespace

int RunCanonicalIdentityEncodingTests() {
  if (!VerifyPinnedDigestVectors())
    return 1;
  if (!VerifyVersionedRootTags())
    return 2;
  if (!VerifyLengthDelimitedFraming())
    return 3;
  if (!VerifyStableTypeResolution())
    return 4;
  if (!VerifySymbolIdentityDerivation())
    return 5;
  if (!VerifyIncompleteDescriptorRejection())
    return 6;
  if (!VerifyCollisionRejection())
    return 7;
  return 0;
}
