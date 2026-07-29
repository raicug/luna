// clang-format off
#include "state/identity/canonical_encoding.hpp"

#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

enum class ComponentTag : std::uint8_t {
  TypeNode = 0x10,
  FixedLeaf = 0x11,
  UserLeaf = 0x12,
  Children = 0x13,
  SymbolNode = 0x20,
  Provenance = 0x21,
  Signature = 0x22,
  AssociatedType = 0x23,
  AliasTarget = 0x24,
  Absent = 0x2f
};

void WriteComponentTag(CanonicalEncoder &Encoder, ComponentTag Tag) {
  Encoder.WriteTag(static_cast<std::uint8_t>(Tag));
}

[[nodiscard]] std::vector<std::uint8_t>
EncodeTypeNode(const TypeDescriptor &Descriptor) {
  CanonicalEncoder Encoder;
  WriteComponentTag(Encoder, ComponentTag::TypeNode);
  Encoder.WriteText(TypeKindText(Descriptor.Kind()));
  Encoder.WriteText(CvQualificationText(Descriptor.Qualification()));
  Encoder.WriteUnsigned(Descriptor.ArrayExtent());

  if (const auto FixedKey = Descriptor.FixedKey()) {
    WriteComponentTag(Encoder, ComponentTag::FixedLeaf);
    Encoder.WriteText(FixedTypeKeyText(*FixedKey));
  } else {
    WriteComponentTag(Encoder, ComponentTag::Absent);
  }

  if (Descriptor.Kind() == TypeKind::Enumeration ||
      Descriptor.Kind() == TypeKind::Class ||
      Descriptor.Kind() == TypeKind::Converted) {
    WriteComponentTag(Encoder, ComponentTag::UserLeaf);
    Encoder.WriteText(Descriptor.Key().Text());
  } else {
    WriteComponentTag(Encoder, ComponentTag::Absent);
  }

  WriteComponentTag(Encoder, ComponentTag::Children);
  Encoder.WriteUnsigned(Descriptor.ChildCount());
  for (const TypeDescriptor &Child : Descriptor.Children())
    Encoder.WriteBlock(EncodeTypeNode(Child));
  return Encoder.Release();
}

[[nodiscard]] std::vector<std::uint8_t>
EncodeSignature(const CallableSignatureDescriptor &Signature) {
  CanonicalEncoder Encoder;
  WriteComponentTag(Encoder, ComponentTag::Signature);
  Encoder.WriteBlock(EncodeTypeNode(Signature.ReturnType));
  Encoder.WriteUnsigned(Signature.ParameterTypes.size());
  for (const TypeDescriptor &Parameter : Signature.ParameterTypes)
    Encoder.WriteBlock(EncodeTypeNode(Parameter));
  if (Signature.ReceiverType) {
    Encoder.WriteFlag(true);
    Encoder.WriteBlock(EncodeTypeNode(*Signature.ReceiverType));
  } else {
    Encoder.WriteFlag(false);
  }
  Encoder.WriteFlag(Signature.ReceiverIsConst);
  Encoder.WriteFlag(Signature.IsVariadic);
  Encoder.WriteUnsigned(Signature.RequiredParameterCount);
  return Encoder.Release();
}

} // namespace

void CanonicalEncoder::WriteRootTag(CanonicalDomain Domain) {
  WriteUnsigned(CanonicalSchemaVersion);
  WriteTag(static_cast<std::uint8_t>(Domain));
}

void CanonicalEncoder::WriteTag(std::uint8_t Tag) {
  BytesValue.push_back(Tag);
}

void CanonicalEncoder::WriteUnsigned(std::uint64_t Value) {
  for (std::size_t Index = 0; Index < 8; ++Index)
    BytesValue.push_back(
        static_cast<std::uint8_t>((Value >> ((7 - Index) * 8)) & 0xffULL));
}

void CanonicalEncoder::WriteFlag(bool Value) {
  WriteTag(Value ? std::uint8_t{1} : std::uint8_t{0});
}

void CanonicalEncoder::WriteText(std::string_view Text) {
  WriteUnsigned(Text.size());
  for (const char Character : Text)
    BytesValue.push_back(static_cast<std::uint8_t>(Character));
}

void CanonicalEncoder::WriteBlock(std::span<const std::uint8_t> Block) {
  WriteUnsigned(Block.size());
  BytesValue.insert(BytesValue.end(), Block.begin(), Block.end());
}

std::vector<std::uint8_t>
EncodeCanonicalType(const TypeDescriptor &Descriptor) {
  if (!Descriptor.IsValid())
    return {};
  CanonicalEncoder Encoder;
  Encoder.WriteRootTag(CanonicalDomain::Type);
  Encoder.WriteBlock(EncodeTypeNode(Descriptor));
  return Encoder.Release();
}

std::vector<std::uint8_t>
EncodeCanonicalSymbol(const SymbolDescriptor &Descriptor) {
  if (!Descriptor.IsValid())
    return {};

  CanonicalEncoder Encoder;
  Encoder.WriteRootTag(CanonicalDomain::Symbol);
  WriteComponentTag(Encoder, ComponentTag::SymbolNode);
  Encoder.WriteText(SymbolKindText(Descriptor.Kind));
  Encoder.WriteText(Descriptor.QualifiedName);
  Encoder.WriteBlock(Descriptor.Parent.Bytes());

  if (Descriptor.Module) {
    WriteComponentTag(Encoder, ComponentTag::Provenance);
    Encoder.WriteText(Descriptor.Module->Identity);
    Encoder.WriteText(Descriptor.Module->Version);
  } else {
    WriteComponentTag(Encoder, ComponentTag::Absent);
  }

  if (Descriptor.Signature) {
    Encoder.WriteBlock(EncodeSignature(*Descriptor.Signature));
  } else {
    WriteComponentTag(Encoder, ComponentTag::Absent);
  }

  if (Descriptor.AssociatedType) {
    WriteComponentTag(Encoder, ComponentTag::AssociatedType);
    Encoder.WriteBlock(EncodeTypeNode(*Descriptor.AssociatedType));
  } else {
    WriteComponentTag(Encoder, ComponentTag::Absent);
  }

  WriteComponentTag(Encoder, ComponentTag::AliasTarget);
  Encoder.WriteText(Descriptor.TargetName);
  return Encoder.Release();
}

} // namespace Luna::Detail
