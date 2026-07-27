// clang-format off
#include "state/identity/symbol_descriptor.hpp"

#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr void MixHash(std::uint64_t &Accumulator,
                       std::uint64_t Component) noexcept {
  for (std::size_t Index = 0; Index < 8; ++Index) {
    Accumulator ^= (Component >> (Index * 8)) & 0xffULL;
    Accumulator *= 0x100000001b3ULL;
  }
}

void MixText(std::uint64_t &Accumulator, std::string_view Text) noexcept {
  MixHash(Accumulator, Text.size());
  for (const char Character : Text)
    MixHash(Accumulator,
            static_cast<std::uint64_t>(static_cast<unsigned char>(Character)));
}

[[nodiscard]] constexpr bool IsLeadingCharacter(char Character) noexcept {
  return (Character >= 'A' && Character <= 'Z') ||
         (Character >= 'a' && Character <= 'z') || Character == '_';
}

[[nodiscard]] constexpr bool IsTrailingCharacter(char Character) noexcept {
  return IsLeadingCharacter(Character) ||
         (Character >= '0' && Character <= '9');
}

[[nodiscard]] std::strong_ordering
CompareText(std::string_view Left, std::string_view Right) noexcept {
  const int Comparison = Left.compare(Right);
  if (Comparison < 0)
    return std::strong_ordering::less;
  if (Comparison > 0)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering
CompareOptionalType(const std::optional<TypeDescriptor> &Left,
                    const std::optional<TypeDescriptor> &Right) {
  if (Left.has_value() != Right.has_value())
    return Left.has_value() ? std::strong_ordering::greater
                            : std::strong_ordering::less;
  if (!Left.has_value())
    return std::strong_ordering::equal;
  return TypeDescriptor::Compare(*Left, *Right);
}

[[nodiscard]] constexpr bool RequiresSignature(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::FunctionCandidate:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool RequiresAssociatedType(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Operator:
  case SymbolKind::Property:
  case SymbolKind::Field:
  case SymbolKind::Enumerator:
  case SymbolKind::EnumeratorAlias:
  case SymbolKind::Class:
  case SymbolKind::Enumeration:
  case SymbolKind::Type:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr bool PermitsAssociatedType(SymbolKind Kind) noexcept {
  return RequiresAssociatedType(Kind) || Kind == SymbolKind::Constant;
}

} // namespace

QualifiedNameStatus ClassifyQualifiedName(std::string_view Text) noexcept {
  if (Text.empty())
    return QualifiedNameStatus::Empty;
  if (Text.size() > MaximumQualifiedNameLength)
    return QualifiedNameStatus::TooLong;

  bool AtSegmentStart = true;
  for (const char Character : Text) {
    if (Character == QualifiedNameSeparator) {
      if (AtSegmentStart)
        return QualifiedNameStatus::EmptySegment;
      AtSegmentStart = true;
      continue;
    }
    if (AtSegmentStart) {
      if (!IsLeadingCharacter(Character))
        return IsTrailingCharacter(Character)
                   ? QualifiedNameStatus::InvalidLeadingCharacter
                   : QualifiedNameStatus::InvalidCharacter;
      AtSegmentStart = false;
      continue;
    }
    if (!IsTrailingCharacter(Character))
      return QualifiedNameStatus::InvalidCharacter;
  }
  if (AtSegmentStart)
    return QualifiedNameStatus::EmptySegment;
  return QualifiedNameStatus::Valid;
}

bool IsCanonicalQualifiedName(std::string_view Text) noexcept {
  return ClassifyQualifiedName(Text) == QualifiedNameStatus::Valid;
}

bool ModuleProvenance::IsValid() const noexcept {
  return IsCanonicalQualifiedName(Identity) && !Version.empty() &&
         Version.size() <= MaximumQualifiedNameLength;
}

std::size_t ModuleProvenance::Hash() const noexcept {
  std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
  MixText(Accumulator, Identity);
  MixText(Accumulator, Version);
  return static_cast<std::size_t>(Accumulator);
}

std::strong_ordering CompareProvenance(const ModuleProvenance &Left,
                                       const ModuleProvenance &Right) noexcept {
  if (const auto Order = CompareText(Left.Identity, Right.Identity);
      Order != std::strong_ordering::equal)
    return Order;
  return CompareText(Left.Version, Right.Version);
}

bool CallableSignatureDescriptor::IsValid() const {
  if (!ReturnType.IsValid())
    return false;
  for (const TypeDescriptor &Parameter : ParameterTypes) {
    if (!Parameter.IsValid())
      return false;
  }
  if (ReceiverType && !ReceiverType->IsValid())
    return false;
  if (!ReceiverType && ReceiverIsConst)
    return false;
  return RequiredParameterCount <= ParameterTypes.size();
}

std::size_t CallableSignatureDescriptor::Hash() const {
  std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
  MixHash(Accumulator, static_cast<std::uint64_t>(ReturnType.Hash()));
  MixHash(Accumulator, ParameterTypes.size());
  for (const TypeDescriptor &Parameter : ParameterTypes)
    MixHash(Accumulator, static_cast<std::uint64_t>(Parameter.Hash()));
  MixHash(Accumulator,
          ReceiverType ? static_cast<std::uint64_t>(ReceiverType->Hash()) + 1
                       : 0);
  MixHash(Accumulator, ReceiverIsConst ? 1 : 0);
  MixHash(Accumulator, IsVariadic ? 1 : 0);
  MixHash(Accumulator, RequiredParameterCount);
  return static_cast<std::size_t>(Accumulator);
}

std::strong_ordering
CompareSignature(const CallableSignatureDescriptor &Left,
                 const CallableSignatureDescriptor &Right) {
  if (const auto Order =
          TypeDescriptor::Compare(Left.ReturnType, Right.ReturnType);
      Order != std::strong_ordering::equal)
    return Order;
  if (const auto Order =
          Left.ParameterTypes.size() <=> Right.ParameterTypes.size();
      Order != std::strong_ordering::equal)
    return Order;
  for (std::size_t Index = 0; Index < Left.ParameterTypes.size(); ++Index) {
    if (const auto Order = TypeDescriptor::Compare(Left.ParameterTypes[Index],
                                                   Right.ParameterTypes[Index]);
        Order != std::strong_ordering::equal)
      return Order;
  }
  if (const auto Order =
          CompareOptionalType(Left.ReceiverType, Right.ReceiverType);
      Order != std::strong_ordering::equal)
    return Order;
  if (const auto Order = Left.ReceiverIsConst <=> Right.ReceiverIsConst;
      Order != std::strong_ordering::equal)
    return Order;
  if (const auto Order = Left.IsVariadic <=> Right.IsVariadic;
      Order != std::strong_ordering::equal)
    return Order;
  return Left.RequiredParameterCount <=> Right.RequiredParameterCount;
}

bool operator==(const CallableSignatureDescriptor &Left,
                const CallableSignatureDescriptor &Right) {
  return CompareSignature(Left, Right) == std::strong_ordering::equal;
}

bool SymbolDescriptor::IsValid() const {
  if (!IsCanonicalQualifiedName(QualifiedName))
    return false;
  if (Module && !Module->IsValid())
    return false;
  if (Kind == SymbolKind::Module && !Module)
    return false;

  if (Signature) {
    if (!RequiresSignature(Kind) || !Signature->IsValid())
      return false;
  } else if (RequiresSignature(Kind)) {
    return false;
  }

  if (AssociatedType) {
    if (!PermitsAssociatedType(Kind) || !AssociatedType->IsValid())
      return false;
  } else if (RequiresAssociatedType(Kind)) {
    return false;
  }

  if (Kind == SymbolKind::EnumeratorAlias) {
    if (!IsCanonicalQualifiedName(TargetName))
      return false;
  } else if (!TargetName.empty()) {
    return false;
  }
  return true;
}

std::size_t SymbolDescriptor::Hash() const {
  std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
  MixHash(Accumulator, static_cast<std::uint64_t>(Kind));
  MixText(Accumulator, QualifiedName);
  for (const std::uint8_t Byte : Parent.Bytes())
    MixHash(Accumulator, static_cast<std::uint64_t>(Byte));
  MixHash(Accumulator,
          Module ? static_cast<std::uint64_t>(Module->Hash()) + 1 : 0);
  MixHash(Accumulator,
          Signature ? static_cast<std::uint64_t>(Signature->Hash()) + 1 : 0);
  MixHash(Accumulator,
          AssociatedType
              ? static_cast<std::uint64_t>(AssociatedType->Hash()) + 1
              : 0);
  MixText(Accumulator, TargetName);
  return static_cast<std::size_t>(Accumulator);
}

std::strong_ordering CompareSymbolDescriptor(const SymbolDescriptor &Left,
                                             const SymbolDescriptor &Right) {
  if (const auto Order = Left.Kind <=> Right.Kind;
      Order != std::strong_ordering::equal)
    return Order;
  if (const auto Order = CompareText(Left.QualifiedName, Right.QualifiedName);
      Order != std::strong_ordering::equal)
    return Order;
  if (const auto Order = Left.Parent <=> Right.Parent;
      Order != std::strong_ordering::equal)
    return Order;

  if (Left.Module.has_value() != Right.Module.has_value())
    return Left.Module.has_value() ? std::strong_ordering::greater
                                   : std::strong_ordering::less;
  if (Left.Module) {
    if (const auto Order = CompareProvenance(*Left.Module, *Right.Module);
        Order != std::strong_ordering::equal)
      return Order;
  }

  if (Left.Signature.has_value() != Right.Signature.has_value())
    return Left.Signature.has_value() ? std::strong_ordering::greater
                                      : std::strong_ordering::less;
  if (Left.Signature) {
    if (const auto Order = CompareSignature(*Left.Signature, *Right.Signature);
        Order != std::strong_ordering::equal)
      return Order;
  }

  if (const auto Order =
          CompareOptionalType(Left.AssociatedType, Right.AssociatedType);
      Order != std::strong_ordering::equal)
    return Order;
  return CompareText(Left.TargetName, Right.TargetName);
}

bool operator==(const SymbolDescriptor &Left, const SymbolDescriptor &Right) {
  return CompareSymbolDescriptor(Left, Right) == std::strong_ordering::equal;
}

SymbolDescriptor MakeScopeSymbol(SymbolKind Kind, std::string QualifiedName,
                                 SymbolId Parent) {
  SymbolDescriptor Descriptor;
  Descriptor.Kind = Kind;
  Descriptor.QualifiedName = std::move(QualifiedName);
  Descriptor.Parent = Parent;
  return Descriptor;
}

SymbolDescriptor MakeModuleSymbol(std::string QualifiedName, SymbolId Parent,
                                  ModuleProvenance Provenance) {
  SymbolDescriptor Descriptor =
      MakeScopeSymbol(SymbolKind::Module, std::move(QualifiedName), Parent);
  Descriptor.Module = std::move(Provenance);
  return Descriptor;
}

SymbolDescriptor MakeOverloadSetSymbol(std::string QualifiedName,
                                       SymbolId Parent) {
  return MakeScopeSymbol(SymbolKind::OverloadSet, std::move(QualifiedName),
                         Parent);
}

SymbolDescriptor
MakeCallableCandidateSymbol(SymbolKind Kind, std::string QualifiedName,
                            SymbolId Parent,
                            CallableSignatureDescriptor Signature) {
  SymbolDescriptor Descriptor =
      MakeScopeSymbol(Kind, std::move(QualifiedName), Parent);
  Descriptor.Signature = std::move(Signature);
  if (Descriptor.Signature->ReceiverType && RequiresAssociatedType(Kind))
    Descriptor.AssociatedType = Descriptor.Signature->ReceiverType;
  return Descriptor;
}

SymbolDescriptor
MakeEnumeratorAliasSymbol(std::string QualifiedName, SymbolId Parent,
                          TypeDescriptor Enumeration,
                          std::string CanonicalEnumeratorName) {
  SymbolDescriptor Descriptor = MakeScopeSymbol(
      SymbolKind::EnumeratorAlias, std::move(QualifiedName), Parent);
  Descriptor.AssociatedType = std::move(Enumeration);
  Descriptor.TargetName = std::move(CanonicalEnumeratorName);
  return Descriptor;
}

SymbolDescriptor
MakeClassMemberSymbol(SymbolKind Kind, std::string QualifiedName,
                      SymbolId Parent, TypeDescriptor OwnerType,
                      std::optional<CallableSignatureDescriptor> Signature) {
  SymbolDescriptor Descriptor =
      MakeScopeSymbol(Kind, std::move(QualifiedName), Parent);
  Descriptor.AssociatedType = std::move(OwnerType);
  Descriptor.Signature = std::move(Signature);
  return Descriptor;
}

SymbolDescriptor WithModuleProvenance(SymbolDescriptor Descriptor,
                                      ModuleProvenance Provenance) {
  Descriptor.Module = std::move(Provenance);
  return Descriptor;
}

} // namespace Luna::Detail
