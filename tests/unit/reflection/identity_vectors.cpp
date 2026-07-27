// clang-format off
#include <luna/detail/canonical_type.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CallableSignatureDescriptor;
using Luna::Detail::ModuleProvenance;
using Luna::Detail::SymbolDescriptor;
using Luna::Detail::SymbolIdentityRegistry;
using Luna::Detail::TypeIdentityRegistry;

constexpr bool DumpVectors = false;

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

struct TypeVector final {
  std::string_view Label;
  Luna::TypeDescriptor Descriptor;
  std::string_view Identity;
};

struct SymbolVector final {
  std::string_view Label;
  SymbolDescriptor Descriptor;
  std::string_view Identity;
};

[[nodiscard]] std::vector<TypeVector> FixedTypeVectors() {
  const auto Fixed = [](Luna::FixedTypeKey Key) {
    return Luna::TypeDescriptor::ForFixed(Key);
  };
  return {
      {"luna.void", Fixed(Luna::FixedTypeKey::Void),
       "816fb565079ae9c5082e6d3dff534bef5b00a36b856a0562add4958b81648482"},
      {"luna.bool", Fixed(Luna::FixedTypeKey::Boolean),
       "84cd93079348be4d20105f2cc0d287592efd9d766f32c0aa08a3bc8373562c34"},
      {"luna.int32", Fixed(Luna::FixedTypeKey::Int32),
       "685200e14d400a775fa4ea499cdbe2edd349663927688b63c1791f0c93d30c98"},
      {"luna.float32", Fixed(Luna::FixedTypeKey::Float),
       "c9f6d9fc6a4dc628e16a34be7a67ec70b922f7540caee933087d4a4fc7bbb8cf"},
      {"luna.float64", Fixed(Luna::FixedTypeKey::Double),
       "cd1528cd7e5da5af7f9b76edfb7fb0842dc853439c53b276010d930f8f26ac86"},
      {"luna.string", Fixed(Luna::FixedTypeKey::String),
       "17a78771ff202694b29a02eec230f2f05336418bfc6bad7e95ea65c519c8fe1a"},
      {"luna.string_view", Fixed(Luna::FixedTypeKey::StringView),
       "3a759dec363af9228e6c8ba5fc3b8e82cdb44ab52eddf4ad367f663019642714"},
      {"luna.cstring", Fixed(Luna::FixedTypeKey::CString),
       "897b8d332d76dea7105419b8f69ea2c6d7a3def46ca6adc56324b243d78688ea"},
      {"luna.null", Fixed(Luna::FixedTypeKey::Null),
       "4faa55f29a58d438600dfb97cbc2304c9c7dd854d24807e19154a125c6b6f8df"},
      {"luna.value", Fixed(Luna::FixedTypeKey::Value),
       "bab10dd6a8b589fa35237752e3bbeff7c59ebbebf9b70e88a95450d829aeb673"},
      {"luna.value_pack", Fixed(Luna::FixedTypeKey::ValuePack),
       "acfeb07a74cb4f7f6c52a9c7d8a0e5a27f38162419c2626ac6cc2d6150045ae9"},
  };
}

[[nodiscard]] std::vector<TypeVector> StructuralTypeVectors() {
  const KeyList WidgetKey{Luna::StableTypeKey("studio.ui.Widget")};
  const KeyList Ordered{Luna::StableTypeKey("studio.ui.Color"),
                        Luna::StableTypeKey("studio.ui.Widget")};
  return {
      {"class studio.ui.Widget", WidgetType(),
       "f8c1cec8b7c153cc9413fccec7b650a872ad9d275e715dab003d77a7f7cc537e"},
      {"enum studio.ui.Color", ColorType(),
       "88bf8249b5bfb4dce255e7edfebe5e21e431765a8f5e357e1fd1e56634472ca8"},
      {"pointer const int*", Canonical<const int *>(),
       "9c566ab0e45db8f9ddd318a3d1533fd3412ce31a86dbac71a27d58b02014762a"},
      {"pointer int**", Canonical<int **>(),
       "03a52744da7af80c80f9d1278cf239a59fbfcb8a5e2d15bbe4bdba01a01d3a00"},
      {"array int[4]", Canonical<int[4]>(),
       "e19395dcbeab051dcddfec0db6ef36dedb1aa3e0cad38343a5ac8c5ac03bf913"},
      {"optional int", Canonical<std::optional<int>>(),
       "3fc2b8efd92f2ce8d65d8d7a0c38f5b55e97b0491d5bfdd6fdbfe137899a5b6f"},
      {"sequence int", Canonical<std::vector<int>>(),
       "d046daae18bf286d3dbbc72b12a8cb86f43a1ed656c7839841c495b29e683f38"},
      {"map string int", Canonical<std::map<std::string, int>>(),
       "1393c2c18435e2d20f76d365c1c05c226e47b1fe6cc57b229786d256a8e00d2e"},
      {"pair int double", Canonical<std::pair<int, double>>(),
       "6bed958aa8d43ee0d02cb7fdb40876080ab54e5012bd9bd410ae2ece1cf62a38"},
      {"tuple empty", Canonical<std::tuple<>>(),
       "8455c22c5eec81d91744b319f822e76c686bfe760cd7cccf115168c98d8ee635"},
      {"tuple int double", Canonical<std::tuple<int, double>>(),
       "ff700307b250a6a2d30b456e9232cdb9f2363290cd7ac5bd136a74cde4a4af34"},
      {"shared Widget", Canonical<std::shared_ptr<Widget>>(WidgetKey),
       "48adc155716b98ef70e523acdfd071c50955de46582933c7b22f2d87dec85c66"},
      {"borrowed int", Canonical<std::reference_wrapper<int>>(),
       "e7d27d44b13d4854ca7a9e714c2e1ccec0020c4099322ee9cb72540cd1b29401"},
      {"map Color Widget", Canonical<std::map<Color, Widget>>(Ordered),
       "ea5f2830662c5db79dd0ccedc80073e4fea45d744a1773fa2269a2eb590a23a1"},
      {"nested sequence",
       Canonical<std::vector<std::optional<std::pair<int, std::string>>>>(),
       "40961cffcce378dadf46db23a2b4232e41239d5e814dcb0646981ce9122cf0c8"},
  };
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

[[nodiscard]] Luna::SymbolId StudioNamespaceId() {
  const std::optional<Luna::SymbolId> Identity =
      SymbolIdentityRegistry::ComputeIdentity(Luna::Detail::MakeScopeSymbol(
          Luna::SymbolKind::Namespace, "Studio", Luna::SymbolId()));
  return Identity ? *Identity : Luna::SymbolId();
}

[[nodiscard]] std::vector<SymbolVector> SymbolVectors() {
  const Luna::SymbolId Root;
  const Luna::SymbolId Studio = StudioNamespaceId();

  CallableSignatureDescriptor Method = FreeSignature(Canonical<int>(), {});
  Method.ReceiverType = WidgetType();

  return {
      {"namespace Studio",
       Luna::Detail::MakeScopeSymbol(Luna::SymbolKind::Namespace, "Studio",
                                     Root),
       "d0e4a53bf6585f72d31a1e982b82549ed9ab1a62038c2802577b8c25ca24ec41"},
      {"overload set Studio.Add",
       Luna::Detail::MakeOverloadSetSymbol("Studio.Add", Studio),
       "ccc2248439915c4440c4925ed2518b9f9e25e8c3719959fbd6efa2de8f76b7ca"},
      {"candidate Studio.Add(int)->int",
       Luna::Detail::MakeCallableCandidateSymbol(
           Luna::SymbolKind::FunctionCandidate, "Studio.Add", Studio,
           FreeSignature(Canonical<int>(), {Canonical<int>()})),
       "6bf21bab7de1601b48fa0028daccef2d84c536dd3fed2f1d369d79079a6e90b4"},
      {"module studio.physics 1.2.0",
       Luna::Detail::MakeModuleSymbol(
           "studio.physics", Root, ModuleProvenance{"studio.physics", "1.2.0"}),
       "9e1ddf923fb0ab19c202b4cf235b0cdc36547bdc38d67dfea5dabb994a5528ef"},
      {"method studio.ui.Widget.Size",
       Luna::Detail::MakeClassMemberSymbol(Luna::SymbolKind::Method,
                                           "studio.ui.Widget.Size", Studio,
                                           WidgetType(), Method),
       "a99dc7f7237af4370bffa94bbdc5acbe7d9b323ab1d8081e103421474b0718ae"},
      {"alias studio.ui.Color.Crimson",
       Luna::Detail::MakeEnumeratorAliasSymbol("studio.ui.Color.Crimson",
                                               Studio, ColorType(), "Red"),
       "a605c37153f8bfc1f74bf242b6adf43a3a39e47ea018a2d82539429be4014c9b"},
  };
}

[[nodiscard]] bool DumpEveryVector() {
  for (const TypeVector &Vector : FixedTypeVectors()) {
    const auto Identity =
        TypeIdentityRegistry::ComputeIdentity(Vector.Descriptor);
    std::cerr << "TYPE " << Vector.Label << " = \""
              << (Identity ? Identity->ToString() : std::string("INVALID"))
              << "\"\n";
  }
  for (const TypeVector &Vector : StructuralTypeVectors()) {
    const auto Identity =
        TypeIdentityRegistry::ComputeIdentity(Vector.Descriptor);
    std::cerr << "TYPE " << Vector.Label << " = \""
              << (Identity ? Identity->ToString() : std::string("INVALID"))
              << "\"\n";
  }
  for (const SymbolVector &Vector : SymbolVectors()) {
    const auto Identity =
        SymbolIdentityRegistry::ComputeIdentity(Vector.Descriptor);
    std::cerr << "SYMBOL " << Vector.Label << " = \""
              << (Identity ? Identity->ToString() : std::string("INVALID"))
              << "\"\n";
  }
  return true;
}

[[nodiscard]] std::vector<TypeVector> EveryTypeVector() {
  std::vector<TypeVector> Vectors = FixedTypeVectors();
  for (TypeVector &Structural : StructuralTypeVectors())
    Vectors.push_back(std::move(Structural));
  return Vectors;
}

template <class Identity>
[[nodiscard]] bool MatchesPinnedText(const Identity &Value,
                                     std::string_view Pinned) {
  if (Pinned.size() != Identity::TextLength)
    return false;
  const std::optional<Identity> Parsed = Identity::Parse(Pinned);
  if (!Parsed || *Parsed != Value)
    return false;
  return Value.IsValid() && Value.ToString() == Pinned;
}

[[nodiscard]] bool VerifyPinnedTypeVectors() {
  const std::vector<TypeVector> Vectors = EveryTypeVector();
  std::vector<Luna::TypeId> Identities;
  for (const TypeVector &Vector : Vectors) {
    const std::optional<Luna::TypeId> Identity =
        TypeIdentityRegistry::ComputeIdentity(Vector.Descriptor);
    if (!Identity || !MatchesPinnedText(*Identity, Vector.Identity))
      return false;
    Identities.push_back(*Identity);
  }

  for (std::size_t Left = 0; Left < Identities.size(); ++Left) {
    for (std::size_t Right = Left + 1; Right < Identities.size(); ++Right) {
      if (Identities[Left] == Identities[Right])
        return false;
    }
  }

  TypeIdentityRegistry Forward;
  TypeIdentityRegistry Reverse;
  for (std::size_t Index = 0; Index < Vectors.size(); ++Index) {
    const auto Resolution = Forward.Resolve(Vectors[Index].Descriptor);
    if (!Resolution.IsSuccess() || *Resolution.Value != Identities[Index])
      return false;
  }
  for (std::size_t Index = Vectors.size(); Index > 0; --Index) {
    const auto Resolution = Reverse.Resolve(Vectors[Index - 1].Descriptor);
    if (!Resolution.IsSuccess() || *Resolution.Value != Identities[Index - 1])
      return false;
  }
  return Forward.Size() == Vectors.size() && Reverse.Size() == Vectors.size();
}

[[nodiscard]] bool VerifyPinnedSymbolVectors() {
  const std::vector<SymbolVector> Vectors = SymbolVectors();
  std::vector<Luna::SymbolId> Identities;
  SymbolIdentityRegistry Registry;
  for (const SymbolVector &Vector : Vectors) {
    const std::optional<Luna::SymbolId> Identity =
        SymbolIdentityRegistry::ComputeIdentity(Vector.Descriptor);
    if (!Identity || !MatchesPinnedText(*Identity, Vector.Identity))
      return false;
    const auto Resolution = Registry.Resolve(Vector.Descriptor);
    if (!Resolution.IsSuccess() || *Resolution.Value != *Identity)
      return false;
    Identities.push_back(*Identity);
  }
  if (Registry.Size() != Vectors.size())
    return false;

  for (std::size_t Left = 0; Left < Identities.size(); ++Left) {
    for (std::size_t Right = Left + 1; Right < Identities.size(); ++Right) {
      if (Identities[Left] == Identities[Right])
        return false;
    }
  }

  if (StudioNamespaceId() != Identities.front())
    return false;

  for (const TypeVector &Type : EveryTypeVector()) {
    for (const SymbolVector &Symbol : Vectors) {
      if (Type.Identity == Symbol.Identity)
        return false;
    }
  }
  return true;
}

} // namespace

int RunCanonicalIdentityVectorTests() {
  if (DumpVectors)
    return DumpEveryVector() ? 0 : 1;
  if (!VerifyPinnedTypeVectors())
    return 1;
  if (!VerifyPinnedSymbolVectors())
    return 2;
  return 0;
}
