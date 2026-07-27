// clang-format off
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/detail/canonical_type.hpp>
#include <luna/luna.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CallableSignatureDescriptor;
using Luna::Detail::ModuleProvenance;
using Luna::Detail::SymbolDescriptor;
using Luna::Detail::SymbolIdentityRegistry;
using Luna::Detail::TypeIdentityRegistry;

constexpr std::array FixedKeyChoices{
    Luna::FixedTypeKey::Void,       Luna::FixedTypeKey::Boolean,
    Luna::FixedTypeKey::Int32,      Luna::FixedTypeKey::Float,
    Luna::FixedTypeKey::Double,     Luna::FixedTypeKey::String,
    Luna::FixedTypeKey::StringView, Luna::FixedTypeKey::CString,
    Luna::FixedTypeKey::Null};

constexpr std::array QualificationChoices{
    Luna::CvQualification::None, Luna::CvQualification::Const,
    Luna::CvQualification::Volatile, Luna::CvQualification::ConstVolatile};

constexpr std::array SingleChildKinds{
    Luna::TypeKind::Optional, Luna::TypeKind::Sequence,
    Luna::TypeKind::SharedOwnership, Luna::TypeKind::BorrowedReference};

constexpr std::array MultiChildKinds{
    Luna::TypeKind::Map, Luna::TypeKind::Pair, Luna::TypeKind::Tuple,
    Luna::TypeKind::ArgumentPack, Luna::TypeKind::ReturnPack};

constexpr std::array<std::string_view, 4> UserLeafKeyTexts{
    "studio.ui.Widget", "studio.ui.Color", "engine.core.Vector", "app.Model"};

constexpr std::array<std::string_view, 4> QualifiedNameTexts{
    "Studio", "Studio.Add", "engine.core.Step", "app.Model.Field"};

constexpr std::array<std::string_view, 3> ModuleIdentityTexts{
    "studio.physics", "engine.core", "app.tools"};

constexpr std::array<std::string_view, 3> ModuleVersionTexts{"1.0.0", "1.2.0",
                                                             "2.0.0-beta.1"};

constexpr std::array<std::string_view, 3> EnumeratorTexts{"Red", "Green",
                                                          "Blue"};

class ByteCursor final {
public:
  explicit ByteCursor(std::span<const std::uint8_t> Bytes) noexcept
      : BytesValue(Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue.empty())
      return static_cast<std::uint8_t>(Index * 37U + 11U);
    return BytesValue[Index % BytesValue.size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  std::span<const std::uint8_t> BytesValue;
  std::size_t IndexValue = 0;
};

[[nodiscard]] Luna::TypeDescriptor MakeType(ByteCursor &Cursor,
                                            std::size_t Depth) {
  const std::size_t Choice = Depth == 0 ? Cursor.Pick(6) : Cursor.Pick(10);
  switch (Choice) {
  case 0:
  case 1:
  case 2:
  case 3:
    return Luna::TypeDescriptor::ForFixed(
        FixedKeyChoices[Cursor.Pick(FixedKeyChoices.size())]);
  case 4:
    return Luna::TypeDescriptor::ForEnumeration(Luna::StableTypeKey(
        UserLeafKeyTexts[Cursor.Pick(UserLeafKeyTexts.size())]));
  case 5:
    return Luna::TypeDescriptor::ForClass(Luna::StableTypeKey(
        UserLeafKeyTexts[Cursor.Pick(UserLeafKeyTexts.size())]));
  case 6: {
    const Luna::CvQualification Qualification =
        QualificationChoices[Cursor.Pick(QualificationChoices.size())];
    return Luna::TypeDescriptor::ForPointer(
        MakeType(Cursor, Depth - 1).WithQualification(Qualification));
  }
  case 7: {
    const std::size_t Extent = 1U + Cursor.Pick(4);
    const Luna::CvQualification Qualification =
        QualificationChoices[Cursor.Pick(QualificationChoices.size())];
    return Luna::TypeDescriptor::ForArray(
        MakeType(Cursor, Depth - 1).WithQualification(Qualification), Extent);
  }
  case 8: {
    const Luna::TypeKind Kind =
        SingleChildKinds[Cursor.Pick(SingleChildKinds.size())];
    std::vector<Luna::TypeDescriptor> Children;
    Children.push_back(MakeType(Cursor, Depth - 1));
    return Luna::TypeDescriptor::ForStructure(Kind, std::move(Children));
  }
  default: {
    const Luna::TypeKind Kind =
        MultiChildKinds[Cursor.Pick(MultiChildKinds.size())];
    const std::size_t ChildCount =
        (Kind == Luna::TypeKind::Map || Kind == Luna::TypeKind::Pair)
            ? 2U
            : 1U + Cursor.Pick(3);
    std::vector<Luna::TypeDescriptor> Children;
    for (std::size_t Index = 0; Index < ChildCount; ++Index)
      Children.push_back(MakeType(Cursor, Depth - 1));
    return Luna::TypeDescriptor::ForStructure(Kind, std::move(Children));
  }
  }
}

[[nodiscard]] Luna::TypeDescriptor MakeUserLeaf(ByteCursor &Cursor,
                                                bool Enumeration) {
  const Luna::StableTypeKey Key(
      UserLeafKeyTexts[Cursor.Pick(UserLeafKeyTexts.size())]);
  return Enumeration ? Luna::TypeDescriptor::ForEnumeration(Key)
                     : Luna::TypeDescriptor::ForClass(Key);
}

[[nodiscard]] CallableSignatureDescriptor MakeSignature(ByteCursor &Cursor,
                                                        bool WithReceiver) {
  CallableSignatureDescriptor Signature;
  Signature.ReturnType = MakeType(Cursor, 1);
  const std::size_t ParameterCount = Cursor.Pick(3);
  for (std::size_t Index = 0; Index < ParameterCount; ++Index)
    Signature.ParameterTypes.push_back(MakeType(Cursor, 1));
  Signature.RequiredParameterCount = Cursor.Pick(ParameterCount + 1);
  Signature.IsVariadic = Cursor.Pick(2) == 0;
  if (WithReceiver) {
    Signature.ReceiverType = MakeUserLeaf(Cursor, false);
    Signature.ReceiverIsConst = Cursor.Pick(2) == 0;
  }
  return Signature;
}

[[nodiscard]] SymbolDescriptor
MakeSymbol(ByteCursor &Cursor, const std::vector<Luna::SymbolId> &Parents) {
  const Luna::SymbolId Parent = Parents[Cursor.Pick(Parents.size())];
  std::string Name(QualifiedNameTexts[Cursor.Pick(QualifiedNameTexts.size())]);
  switch (Cursor.Pick(8)) {
  case 0:
    return Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Namespace,
                                         std::move(Name), Parent);
  case 1:
    return Luna::Detail::MakeOverloadSetSymbol(std::move(Name), Parent);
  case 2: {
    ModuleProvenance Provenance;
    Provenance.Identity = std::string(
        ModuleIdentityTexts[Cursor.Pick(ModuleIdentityTexts.size())]);
    Provenance.Version =
        std::string(ModuleVersionTexts[Cursor.Pick(ModuleVersionTexts.size())]);
    return Luna::Detail::MakeModuleSymbol(std::move(Name), Parent,
                                          std::move(Provenance));
  }
  case 3:
    return Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Constant,
                                         std::move(Name), Parent);
  case 4:
    return Luna::Detail::MakeCallableCandidateSymbol(
        Luna::SymbolKind::FunctionCandidate, std::move(Name), Parent,
        MakeSignature(Cursor, false));
  case 5:
    return Luna::Detail::MakeCallableCandidateSymbol(
        Luna::SymbolKind::Method, std::move(Name), Parent,
        MakeSignature(Cursor, true));
  case 6:
    return Luna::Detail::MakeClassMemberSymbol(Luna::SymbolKind::Field,
                                               std::move(Name), Parent,
                                               MakeUserLeaf(Cursor, false));
  default:
    return Luna::Detail::MakeEnumeratorAliasSymbol(
        std::move(Name), Parent, MakeUserLeaf(Cursor, true),
        std::string(EnumeratorTexts[Cursor.Pick(EnumeratorTexts.size())]));
  }
}

void AppendUnsigned(std::string &Text, std::uint64_t Value) {
  Text.append(std::to_string(Value));
  Text.push_back(';');
}

void AppendText(std::string &Text, std::string_view Value) {
  Text.append(std::to_string(Value.size()));
  Text.push_back(':');
  Text.append(Value);
  Text.push_back(';');
}

void AppendModelType(std::string &Text, const Luna::TypeDescriptor &Type) {
  Text.push_back('{');
  AppendUnsigned(Text, static_cast<std::uint64_t>(Type.Kind()));
  const auto FixedKey = Type.FixedKey();
  AppendUnsigned(Text,
                 FixedKey ? static_cast<std::uint64_t>(*FixedKey) + 1U : 0U);
  AppendText(Text, Type.Key().Text());
  AppendUnsigned(Text, static_cast<std::uint64_t>(Type.Qualification()));
  AppendUnsigned(Text, static_cast<std::uint64_t>(Type.ArrayExtent()));
  AppendUnsigned(Text, static_cast<std::uint64_t>(Type.ChildCount()));
  for (const Luna::TypeDescriptor &Child : Type.Children())
    AppendModelType(Text, Child);
  Text.push_back('}');
}

[[nodiscard]] std::string ModelTypeKey(const Luna::TypeDescriptor &Type) {
  if (!Type.IsValid())
    return std::string();
  std::string Text("type;");
  AppendModelType(Text, Type);
  return Text;
}

[[nodiscard]] std::string ModelSymbolKey(const SymbolDescriptor &Symbol) {
  if (!Symbol.IsValid())
    return std::string();
  std::string Text("symbol;");
  AppendUnsigned(Text, static_cast<std::uint64_t>(Symbol.Kind));
  AppendText(Text, Symbol.QualifiedName);
  AppendText(Text, Symbol.Parent.ToString());
  if (Symbol.Module) {
    Text.append("module;");
    AppendText(Text, Symbol.Module->Identity);
    AppendText(Text, Symbol.Module->Version);
  } else {
    Text.append("no-module;");
  }
  if (Symbol.Signature) {
    Text.append("signature;");
    AppendModelType(Text, Symbol.Signature->ReturnType);
    AppendUnsigned(Text, static_cast<std::uint64_t>(
                             Symbol.Signature->ParameterTypes.size()));
    for (const Luna::TypeDescriptor &Parameter :
         Symbol.Signature->ParameterTypes)
      AppendModelType(Text, Parameter);
    if (Symbol.Signature->ReceiverType) {
      Text.append("receiver;");
      AppendModelType(Text, *Symbol.Signature->ReceiverType);
    } else {
      Text.append("no-receiver;");
    }
    AppendUnsigned(Text, Symbol.Signature->ReceiverIsConst ? 1U : 0U);
    AppendUnsigned(Text, Symbol.Signature->IsVariadic ? 1U : 0U);
    AppendUnsigned(Text, static_cast<std::uint64_t>(
                             Symbol.Signature->RequiredParameterCount));
  } else {
    Text.append("no-signature;");
  }
  if (Symbol.AssociatedType) {
    Text.append("associated;");
    AppendModelType(Text, *Symbol.AssociatedType);
  } else {
    Text.append("no-associated;");
  }
  AppendText(Text, Symbol.TargetName);
  return Text;
}

[[nodiscard]] std::vector<std::size_t>
MakePermutation(std::size_t Count, std::span<const std::uint8_t> Bytes) {
  std::vector<std::size_t> Order(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Order[Index] = Index;
  ByteCursor Cursor(Bytes);
  for (std::size_t Index = Count; Index > 1; --Index)
    std::swap(Order[Index - 1], Order[Cursor.Pick(Index)]);
  return Order;
}

[[nodiscard]] std::size_t DistinctCount(const std::vector<std::string> &Keys) {
  const std::set<std::string> Distinct(Keys.begin(), Keys.end());
  return Distinct.size();
}

template <class Identity>
[[nodiscard]] std::vector<std::string>
HashOrderedModelKeys(const std::vector<Identity> &Identities,
                     const std::vector<std::string> &Keys,
                     const std::vector<std::size_t> &Order) {
  std::unordered_map<Identity, std::string, Luna::CanonicalHash> Container;
  for (const std::size_t Index : Order)
    Container.emplace(Identities[Index], Keys[Index]);
  std::vector<std::string> Contents;
  Contents.reserve(Container.size());
  for (const auto &Entry : Container)
    Contents.push_back(Entry.second);
  std::sort(Contents.begin(), Contents.end());
  return Contents;
}

} // namespace

namespace {

[[nodiscard]] std::vector<Luna::TypeId>
VerifyTypeResolution(const std::vector<Luna::TypeDescriptor> &Types,
                     std::span<const std::uint8_t> PermutationBytes) {
  std::vector<std::string> ModelKeys;
  ModelKeys.reserve(Types.size());
  for (const Luna::TypeDescriptor &Type : Types) {
    RC_ASSERT(Type.IsValid());
    ModelKeys.push_back(ModelTypeKey(Type));
    RC_ASSERT(!ModelKeys.back().empty());
  }

  TypeIdentityRegistry Forward;
  std::vector<Luna::TypeId> ForwardIdentities;
  ForwardIdentities.reserve(Types.size());
  for (const Luna::TypeDescriptor &Type : Types) {
    const auto Resolution = Forward.Resolve(Type);
    RC_ASSERT(Resolution.IsSuccess());
    RC_ASSERT(!Resolution.Diagnostic.has_value());
    RC_ASSERT(Resolution.Value->IsValid());
    RC_ASSERT(Resolution.Value == TypeIdentityRegistry::ComputeIdentity(Type));
    ForwardIdentities.push_back(*Resolution.Value);
  }

  const std::vector<std::size_t> Order =
      MakePermutation(Types.size(), PermutationBytes);
  TypeIdentityRegistry Permuted;
  std::vector<Luna::TypeId> PermutedIdentities(Types.size());
  for (const std::size_t Index : Order) {
    const auto Resolution = Permuted.Resolve(Types[Index]);
    RC_ASSERT(Resolution.IsSuccess());
    PermutedIdentities[Index] = *Resolution.Value;
  }

  const std::size_t Distinct = DistinctCount(ModelKeys);
  RC_ASSERT(Forward.Size() == Distinct);
  RC_ASSERT(Permuted.Size() == Distinct);

  for (std::size_t Index = 0; Index < Types.size(); ++Index) {
    RC_ASSERT(PermutedIdentities[Index] == ForwardIdentities[Index]);
    RC_ASSERT(Forward.Matches(ForwardIdentities[Index], Types[Index]));
    const Luna::TypeDescriptor *Stored = Forward.Find(ForwardIdentities[Index]);
    RC_ASSERT(Stored != nullptr);
    RC_ASSERT(*Stored == Types[Index]);
    for (std::size_t Other = 0; Other < Types.size(); ++Other) {
      const bool Equivalent = ModelKeys[Index] == ModelKeys[Other];
      RC_ASSERT((ForwardIdentities[Index] == ForwardIdentities[Other]) ==
                Equivalent);
      RC_ASSERT(Forward.Matches(ForwardIdentities[Index], Types[Other]) ==
                Equivalent);
    }
  }

  RC_ASSERT(HashOrderedModelKeys(ForwardIdentities, ModelKeys,
                                 MakePermutation(Types.size(), {})) ==
            HashOrderedModelKeys(ForwardIdentities, ModelKeys, Order));
  RC_ASSERT(HashOrderedModelKeys(ForwardIdentities, ModelKeys, Order).size() ==
            Distinct);

  return ForwardIdentities;
}

[[nodiscard]] std::vector<Luna::SymbolId>
VerifySymbolResolution(const std::vector<SymbolDescriptor> &Symbols,
                       std::span<const std::uint8_t> PermutationBytes) {
  std::vector<std::string> ModelKeys;
  ModelKeys.reserve(Symbols.size());
  for (const SymbolDescriptor &Symbol : Symbols) {
    RC_ASSERT(Symbol.IsValid());
    ModelKeys.push_back(ModelSymbolKey(Symbol));
    RC_ASSERT(!ModelKeys.back().empty());
  }

  SymbolIdentityRegistry Forward;
  std::vector<Luna::SymbolId> ForwardIdentities;
  ForwardIdentities.reserve(Symbols.size());
  for (const SymbolDescriptor &Symbol : Symbols) {
    const auto Resolution = Forward.Resolve(Symbol);
    RC_ASSERT(Resolution.IsSuccess());
    RC_ASSERT(!Resolution.Diagnostic.has_value());
    RC_ASSERT(Resolution.Value->IsValid());
    RC_ASSERT(Resolution.Value ==
              SymbolIdentityRegistry::ComputeIdentity(Symbol));
    ForwardIdentities.push_back(*Resolution.Value);
  }

  const std::vector<std::size_t> Order =
      MakePermutation(Symbols.size(), PermutationBytes);
  SymbolIdentityRegistry Permuted;
  std::vector<Luna::SymbolId> PermutedIdentities(Symbols.size());
  for (const std::size_t Index : Order) {
    const auto Resolution = Permuted.Resolve(Symbols[Index]);
    RC_ASSERT(Resolution.IsSuccess());
    PermutedIdentities[Index] = *Resolution.Value;
  }

  const std::size_t Distinct = DistinctCount(ModelKeys);
  RC_ASSERT(Forward.Size() == Distinct);
  RC_ASSERT(Permuted.Size() == Distinct);

  for (std::size_t Index = 0; Index < Symbols.size(); ++Index) {
    RC_ASSERT(PermutedIdentities[Index] == ForwardIdentities[Index]);
    RC_ASSERT(Forward.Matches(ForwardIdentities[Index], Symbols[Index]));
    for (std::size_t Other = 0; Other < Symbols.size(); ++Other) {
      const bool Equivalent = ModelKeys[Index] == ModelKeys[Other];
      RC_ASSERT((ForwardIdentities[Index] == ForwardIdentities[Other]) ==
                Equivalent);
      RC_ASSERT(Forward.Matches(ForwardIdentities[Index], Symbols[Other]) ==
                Equivalent);
    }
  }

  RC_ASSERT(HashOrderedModelKeys(ForwardIdentities, ModelKeys,
                                 MakePermutation(Symbols.size(), {})) ==
            HashOrderedModelKeys(ForwardIdentities, ModelKeys, Order));

  return ForwardIdentities;
}

void VerifyStateAndExecutionIndependence(
    const std::vector<Luna::TypeDescriptor> &Types,
    const std::vector<Luna::TypeId> &ExpectedTypes,
    const std::vector<SymbolDescriptor> &Symbols,
    const std::vector<Luna::SymbolId> &ExpectedSymbols) {
  {
    Luna::State First;
    Luna::State Second;
    RC_ASSERT(First.IsReady());
    RC_ASSERT(Second.IsReady());
    int Calls = 0;
    RC_ASSERT(First.Bindings()
                  .Register("Touch", [&Calls]() { ++Calls; })
                  .IsSuccess());
    RC_ASSERT(First.Execute("Touch()").IsSuccess());
    RC_ASSERT(Calls == 1);

    TypeIdentityRegistry FirstTypes;
    TypeIdentityRegistry SecondTypes;
    for (std::size_t Index = 0; Index < Types.size(); ++Index) {
      const auto InFirst = FirstTypes.Resolve(Types[Index]);
      const auto InSecond =
          SecondTypes.Resolve(Types[Types.size() - 1 - Index]);
      RC_ASSERT(InFirst.IsSuccess());
      RC_ASSERT(InSecond.IsSuccess());
      RC_ASSERT(*InFirst.Value == ExpectedTypes[Index]);
      RC_ASSERT(*InSecond.Value == ExpectedTypes[Types.size() - 1 - Index]);
    }

    SymbolIdentityRegistry FirstSymbols;
    for (std::size_t Index = 0; Index < Symbols.size(); ++Index) {
      const auto Resolution = FirstSymbols.Resolve(Symbols[Index]);
      RC_ASSERT(Resolution.IsSuccess());
      RC_ASSERT(*Resolution.Value == ExpectedSymbols[Index]);
    }
  }

  TypeIdentityRegistry AfterDestruction;
  for (std::size_t Index = 0; Index < Types.size(); ++Index) {
    const auto Resolution = AfterDestruction.Resolve(Types[Index]);
    RC_ASSERT(Resolution.IsSuccess());
    RC_ASSERT(*Resolution.Value == ExpectedTypes[Index]);
  }
  SymbolIdentityRegistry SymbolsAfterDestruction;
  for (std::size_t Index = 0; Index < Symbols.size(); ++Index) {
    const auto Resolution = SymbolsAfterDestruction.Resolve(Symbols[Index]);
    RC_ASSERT(Resolution.IsSuccess());
    RC_ASSERT(*Resolution.Value == ExpectedSymbols[Index]);
  }
}

void VerifyTypeCollisionRejection(const Luna::TypeDescriptor &First,
                                  const Luna::TypeDescriptor &Equivalent) {
  RC_ASSERT(First == Equivalent);

  std::vector<Luna::TypeDescriptor> Children;
  Children.push_back(First);
  const Luna::TypeDescriptor Unequal = Luna::TypeDescriptor::ForStructure(
      Luna::TypeKind::Optional, std::move(Children));
  RC_ASSERT(Unequal.IsValid());
  RC_ASSERT(ModelTypeKey(Unequal) != ModelTypeKey(First));

  TypeIdentityRegistry Registry;
  const auto Stored = Registry.Resolve(First);
  RC_ASSERT(Stored.IsSuccess());
  Registry.CollisionInjection().InjectIdentity(Stored.Value->Bytes());

  const auto Collided = Registry.Resolve(Unequal);
  RC_ASSERT(!Collided.IsSuccess());
  RC_ASSERT(Collided.Diagnostic.has_value());
  RC_ASSERT(Collided.Diagnostic->Category() == Luna::ErrorCategory::Internal);
  RC_ASSERT(!Collided.Diagnostic->Message().empty());
  RC_ASSERT(Registry.Size() == 1);
  RC_ASSERT(Registry.Matches(*Stored.Value, First));
  const auto NaturalIdentity = TypeIdentityRegistry::ComputeIdentity(Unequal);
  RC_ASSERT(NaturalIdentity.has_value());
  RC_ASSERT(*NaturalIdentity != *Stored.Value);
  RC_ASSERT(Registry.Find(*NaturalIdentity) == nullptr);

  const auto Recovered = Registry.Resolve(Unequal);
  RC_ASSERT(Recovered.IsSuccess());
  RC_ASSERT(Recovered.Value == NaturalIdentity);
  RC_ASSERT(Registry.Size() == 2);

  TypeIdentityRegistry Shared;
  Shared.CollisionInjection().Inject(2);
  const auto Once = Shared.Resolve(First);
  const auto Twice = Shared.Resolve(Equivalent);
  RC_ASSERT(Once.IsSuccess());
  RC_ASSERT(Twice.IsSuccess());
  RC_ASSERT(Once.Value == Twice.Value);
  RC_ASSERT(Shared.Size() == 1);
}

void VerifySymbolCollisionRejection(const SymbolDescriptor &First,
                                    const SymbolDescriptor &Equivalent) {
  RC_ASSERT(First == Equivalent);

  SymbolDescriptor Unequal = First;
  Unequal.QualifiedName = "collision." + First.QualifiedName;
  RC_ASSERT(Unequal.IsValid());
  RC_ASSERT(ModelSymbolKey(Unequal) != ModelSymbolKey(First));

  SymbolIdentityRegistry Registry;
  const auto Stored = Registry.Resolve(First);
  RC_ASSERT(Stored.IsSuccess());
  Registry.CollisionInjection().InjectIdentity(Stored.Value->Bytes());

  const auto Collided = Registry.Resolve(Unequal);
  RC_ASSERT(!Collided.IsSuccess());
  RC_ASSERT(Collided.Diagnostic.has_value());
  RC_ASSERT(Collided.Diagnostic->Category() == Luna::ErrorCategory::Internal);
  RC_ASSERT(!Collided.Diagnostic->Message().empty());
  RC_ASSERT(Registry.Size() == 1);
  RC_ASSERT(Registry.Matches(*Stored.Value, First));
  const auto NaturalIdentity = SymbolIdentityRegistry::ComputeIdentity(Unequal);
  RC_ASSERT(NaturalIdentity.has_value());
  RC_ASSERT(*NaturalIdentity != *Stored.Value);
  RC_ASSERT(Registry.Find(*NaturalIdentity) == nullptr);

  const auto Recovered = Registry.Resolve(Unequal);
  RC_ASSERT(Recovered.IsSuccess());
  RC_ASSERT(Recovered.Value == NaturalIdentity);
  RC_ASSERT(Registry.Size() == 2);

  SymbolIdentityRegistry Shared;
  Shared.CollisionInjection().Inject(2);
  const auto Once = Shared.Resolve(First);
  const auto Twice = Shared.Resolve(Equivalent);
  RC_ASSERT(Once.IsSuccess());
  RC_ASSERT(Twice.IsSuccess());
  RC_ASSERT(Once.Value == Twice.Value);
  RC_ASSERT(Shared.Size() == 1);
}

enum class ModelColor { Red, Green };
struct ModelWidget {
  int Field = 0;
};

void VerifyNormalizationEquivalence() {
  const std::vector<Luna::StableTypeKey> WidgetKeys{
      Luna::StableTypeKey("studio.ui.Widget")};
  const std::vector<Luna::StableTypeKey> ColorKeys{
      Luna::StableTypeKey("studio.ui.Color")};

  const std::vector<std::pair<Luna::TypeDescriptor, Luna::TypeDescriptor>>
      Equivalences{
          {Luna::Detail::CanonicalDescriptorFor<int>(),
           Luna::Detail::CanonicalDescriptorFor<const int &>()},
          {Luna::Detail::CanonicalDescriptorFor<std::vector<int>>(),
           Luna::Detail::CanonicalDescriptorFor<const std::vector<int> &&>()},
          {Luna::Detail::CanonicalDescriptorFor<const int *>(),
           Luna::Detail::CanonicalDescriptorFor<const int *const &>()},
          {Luna::Detail::CanonicalDescriptorFor<ModelWidget>(WidgetKeys),
           Luna::Detail::CanonicalDescriptorFor<const ModelWidget &>(
               WidgetKeys)},
          {Luna::Detail::CanonicalDescriptorFor<ModelColor>(ColorKeys),
           Luna::Detail::CanonicalDescriptorFor<ModelColor &&>(ColorKeys)}};

  TypeIdentityRegistry Registry;
  for (const auto &Equivalence : Equivalences) {
    RC_ASSERT(Equivalence.first.IsValid());
    RC_ASSERT(Equivalence.second.IsValid());
    RC_ASSERT(ModelTypeKey(Equivalence.first) ==
              ModelTypeKey(Equivalence.second));
    const auto Left = Registry.Resolve(Equivalence.first);
    const auto Right = Registry.Resolve(Equivalence.second);
    RC_ASSERT(Left.IsSuccess());
    RC_ASSERT(Right.IsSuccess());
    RC_ASSERT(Left.Value == Right.Value);
  }
  RC_ASSERT(Registry.Size() == Equivalences.size());

  const std::vector<Luna::TypeDescriptor> Distinct{
      Luna::Detail::CanonicalDescriptorFor<int *>(),
      Luna::Detail::CanonicalDescriptorFor<const int *>(),
      Luna::Detail::CanonicalDescriptorFor<int **>(),
      Luna::Detail::CanonicalDescriptorFor<int[3]>(),
      Luna::Detail::CanonicalDescriptorFor<int[4]>(),
      Luna::Detail::CanonicalDescriptorFor<ModelWidget>(WidgetKeys),
      Luna::Detail::CanonicalDescriptorFor<ModelColor>(ColorKeys)};
  for (std::size_t Index = 0; Index < Distinct.size(); ++Index) {
    for (std::size_t Other = Index + 1; Other < Distinct.size(); ++Other) {
      RC_ASSERT(ModelTypeKey(Distinct[Index]) != ModelTypeKey(Distinct[Other]));
      RC_ASSERT(TypeIdentityRegistry::ComputeIdentity(Distinct[Index]) !=
                TypeIdentityRegistry::ComputeIdentity(Distinct[Other]));
    }
  }
}

} // namespace

int RunStableCanonicalIdentityProperties() {
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 18: Stable type and symbol identities follow canonical descriptors
  const bool Passed = rc::check(
      // clang-format on
      "Stable type and symbol identities follow canonical descriptors",
      [](const std::vector<std::uint8_t> &TypeShape,
         const std::vector<std::uint8_t> &SymbolShape,
         const std::vector<std::uint8_t> &PermutationShape) {
        const std::size_t SeedCount = 1U + TypeShape.size() % 4U;
        std::vector<std::span<const std::uint8_t>> Seeds;
        Seeds.reserve(SeedCount);
        for (std::size_t Index = 0; Index < SeedCount; ++Index) {
          const std::span<const std::uint8_t> Bytes(TypeShape);
          const std::size_t Offset =
              Bytes.empty() ? 0 : (Index * 3U) % Bytes.size();
          Seeds.push_back(Bytes.subspan(Offset));
        }

        std::vector<Luna::TypeDescriptor> Types;
        Types.reserve(SeedCount * 2U);
        for (std::size_t Pass = 0; Pass < 2U; ++Pass) {
          for (const std::span<const std::uint8_t> Seed : Seeds) {
            ByteCursor Cursor(Seed);
            Types.push_back(MakeType(Cursor, 3));
          }
        }

        const std::vector<Luna::TypeId> TypeIdentities =
            VerifyTypeResolution(Types, PermutationShape);

        std::vector<Luna::SymbolId> Parents{Luna::SymbolId()};
        for (const std::string_view Name : {"Studio", "engine.core"}) {
          const auto Parent = SymbolIdentityRegistry::ComputeIdentity(
              Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Namespace,
                                            std::string(Name),
                                            Luna::SymbolId()));
          RC_ASSERT(Parent.has_value());
          Parents.push_back(*Parent);
        }

        const std::size_t SymbolSeedCount = 1U + SymbolShape.size() % 4U;
        std::vector<SymbolDescriptor> Symbols;
        Symbols.reserve(SymbolSeedCount * 2U);
        for (std::size_t Pass = 0; Pass < 2U; ++Pass) {
          for (std::size_t Index = 0; Index < SymbolSeedCount; ++Index) {
            const std::span<const std::uint8_t> Bytes(SymbolShape);
            const std::size_t Offset =
                Bytes.empty() ? 0 : (Index * 5U) % Bytes.size();
            ByteCursor Cursor(Bytes.subspan(Offset));
            Symbols.push_back(MakeSymbol(Cursor, Parents));
          }
        }

        const std::vector<Luna::SymbolId> SymbolIdentities =
            VerifySymbolResolution(Symbols, PermutationShape);

        VerifyStateAndExecutionIndependence(Types, TypeIdentities, Symbols,
                                            SymbolIdentities);
        VerifyTypeCollisionRejection(Types.front(), Types[Seeds.size()]);
        VerifySymbolCollisionRejection(Symbols.front(),
                                       Symbols[SymbolSeedCount]);
        VerifyNormalizationEquivalence();
      });

  return Passed ? 0 : 1;
}
