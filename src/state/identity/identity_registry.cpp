// clang-format off
#include "state/identity/identity_registry.hpp"

#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/canonical_encoding.hpp"
#include "state/identity/digest.hpp"
#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string DescribeType(const TypeDescriptor &Descriptor) {
  std::string Text;
  if (const auto FixedKey = Descriptor.FixedKey()) {
    Text.append(FixedTypeKeyText(*FixedKey));
  } else if (Descriptor.Kind() == TypeKind::Enumeration ||
             Descriptor.Kind() == TypeKind::Class) {
    Text.append(TypeKindText(Descriptor.Kind()));
    Text.push_back(':');
    Text.append(Descriptor.Key().Text());
  } else {
    Text.append(TypeKindText(Descriptor.Kind()));
    if (Descriptor.Kind() == TypeKind::Array) {
      Text.push_back('[');
      Text.append(std::to_string(Descriptor.ArrayExtent()));
      Text.push_back(']');
    }
    Text.push_back('<');
    bool First = true;
    for (const TypeDescriptor &Child : Descriptor.Children()) {
      if (!First)
        Text.push_back(',');
      First = false;
      Text.append(DescribeType(Child));
    }
    Text.push_back('>');
  }
  if (Descriptor.Qualification() != CvQualification::None) {
    Text.push_back(' ');
    Text.append(CvQualificationText(Descriptor.Qualification()));
  }
  return Text;
}

[[nodiscard]] std::string DescribeSymbol(const SymbolDescriptor &Descriptor) {
  std::string Text(SymbolKindText(Descriptor.Kind));
  Text.append(" '");
  Text.append(Descriptor.QualifiedName);
  Text.push_back('\'');
  if (Descriptor.Module) {
    Text.append(" from module '");
    Text.append(Descriptor.Module->Identity);
    Text.push_back('@');
    Text.append(Descriptor.Module->Version);
    Text.push_back('\'');
  }
  if (Descriptor.Signature) {
    Text.append(" signature ");
    Text.append(DescribeType(Descriptor.Signature->ReturnType));
    Text.push_back('(');
    bool First = true;
    for (const TypeDescriptor &Parameter :
         Descriptor.Signature->ParameterTypes) {
      if (!First)
        Text.push_back(',');
      First = false;
      Text.append(DescribeType(Parameter));
    }
    if (Descriptor.Signature->IsVariadic)
      Text.append(First ? "..." : ",...");
    Text.push_back(')');
  }
  if (Descriptor.AssociatedType) {
    Text.append(" of ");
    Text.append(DescribeType(*Descriptor.AssociatedType));
  }
  if (!Descriptor.TargetName.empty()) {
    Text.append(" aliasing '");
    Text.append(Descriptor.TargetName);
    Text.push_back('\'');
  }
  return Text;
}

template <class Identity, class Descriptor, class Encode, class Describe>
[[nodiscard]] IdentityResolution<Identity> ResolveCanonicalIdentity(
    std::map<Identity, Descriptor> &Index, IdentityCollisionInjector &Injector,
    const Descriptor &Requested, Encode EncodeDescriptor,
    Describe DescribeDescriptor, std::string_view DomainName) {
  IdentityResolution<Identity> Resolution;

  const std::vector<std::uint8_t> Bytes = EncodeDescriptor(Requested);
  if (Bytes.empty()) {
    Resolution.Diagnostic = ErrorDiagnostic::Create(
        ErrorCategory::Internal,
        "Internal identity failure: incomplete canonical " +
            std::string(DomainName) + " descriptor for " +
            DescribeDescriptor(Requested) + ".");
    return Resolution;
  }

  CanonicalDigest::Storage DigestBytes = CanonicalDigest::Compute(Bytes);
  if (const auto Forced = Injector.Consume())
    DigestBytes = *Forced;
  const Identity Candidate = Identity::FromBytes(DigestBytes);

  const auto Existing = Index.find(Candidate);
  if (Existing != Index.end()) {
    if (!(Existing->second == Requested)) {
      Resolution.Diagnostic = ErrorDiagnostic::Create(
          ErrorCategory::Internal,
          "Internal identity collision: canonical " + std::string(DomainName) +
              " id " + Candidate.ToString() +
              " is shared by unequal descriptors " +
              DescribeDescriptor(Existing->second) + " and " +
              DescribeDescriptor(Requested) + ".");
      return Resolution;
    }
    Resolution.Value = Candidate;
    return Resolution;
  }

  Index.emplace(Candidate, Requested);
  Resolution.Value = Candidate;
  return Resolution;
}

} // namespace

CanonicalDigest::Storage
IdentityCollisionInjector::SharedIdentityBytes() noexcept {
  CanonicalDigest::Storage Bytes{};
  for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
    Bytes[Index] = 0xcc;
  return Bytes;
}

void IdentityCollisionInjector::Inject(std::size_t Count) noexcept {
  InjectIdentity(SharedIdentityBytes(), Count);
}

void IdentityCollisionInjector::InjectIdentity(CanonicalDigest::Storage Bytes,
                                               std::size_t Count) noexcept {
  ForcedBytes = Bytes;
  PendingCount = Count;
}

void IdentityCollisionInjector::Clear() noexcept { PendingCount = 0; }

std::optional<CanonicalDigest::Storage>
IdentityCollisionInjector::Consume() noexcept {
  if (PendingCount == 0)
    return std::nullopt;
  --PendingCount;
  return ForcedBytes;
}

std::optional<TypeId>
TypeIdentityRegistry::ComputeIdentity(const TypeDescriptor &Descriptor) {
  const std::vector<std::uint8_t> Bytes = EncodeCanonicalType(Descriptor);
  if (Bytes.empty())
    return std::nullopt;
  return TypeId::FromBytes(CanonicalDigest::Compute(Bytes));
}

TypeIdentityResolution
TypeIdentityRegistry::Resolve(const TypeDescriptor &Descriptor) {
  return ResolveCanonicalIdentity<TypeId, TypeDescriptor>(
      DescriptorsByIdentity, Injector, Descriptor, EncodeCanonicalType,
      DescribeType, "type");
}

const TypeDescriptor *
TypeIdentityRegistry::Find(TypeId Identity) const noexcept {
  const auto Found = DescriptorsByIdentity.find(Identity);
  return Found == DescriptorsByIdentity.end() ? nullptr : &Found->second;
}

bool TypeIdentityRegistry::Matches(TypeId Identity,
                                   const TypeDescriptor &Descriptor) const {
  const TypeDescriptor *Stored = Find(Identity);
  return Stored != nullptr && *Stored == Descriptor;
}

std::optional<SymbolId>
SymbolIdentityRegistry::ComputeIdentity(const SymbolDescriptor &Descriptor) {
  const std::vector<std::uint8_t> Bytes = EncodeCanonicalSymbol(Descriptor);
  if (Bytes.empty())
    return std::nullopt;
  return SymbolId::FromBytes(CanonicalDigest::Compute(Bytes));
}

SymbolIdentityResolution
SymbolIdentityRegistry::Resolve(const SymbolDescriptor &Descriptor) {
  return ResolveCanonicalIdentity<SymbolId, SymbolDescriptor>(
      DescriptorsByIdentity, Injector, Descriptor, EncodeCanonicalSymbol,
      DescribeSymbol, "symbol");
}

const SymbolDescriptor *
SymbolIdentityRegistry::Find(SymbolId Identity) const noexcept {
  const auto Found = DescriptorsByIdentity.find(Identity);
  return Found == DescriptorsByIdentity.end() ? nullptr : &Found->second;
}

bool SymbolIdentityRegistry::Matches(SymbolId Identity,
                                     const SymbolDescriptor &Descriptor) const {
  const SymbolDescriptor *Stored = Find(Identity);
  return Stored != nullptr && *Stored == Descriptor;
}

} // namespace Luna::Detail
