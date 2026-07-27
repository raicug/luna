#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

inline constexpr char QualifiedNameSeparator = '.';

inline constexpr std::size_t MaximumQualifiedNameLength = 1024;

enum class QualifiedNameStatus {
  Valid,
  Empty,
  TooLong,
  EmptySegment,
  InvalidLeadingCharacter,
  InvalidCharacter
};

[[nodiscard]] QualifiedNameStatus
ClassifyQualifiedName(std::string_view Text) noexcept;

[[nodiscard]] bool IsCanonicalQualifiedName(std::string_view Text) noexcept;

struct ModuleProvenance final {
  std::string Identity;
  std::string Version;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::size_t Hash() const noexcept;

  [[nodiscard]] friend bool operator==(const ModuleProvenance &Left,
                                       const ModuleProvenance &Right) = default;
};

[[nodiscard]] std::strong_ordering
CompareProvenance(const ModuleProvenance &Left,
                  const ModuleProvenance &Right) noexcept;

struct CallableSignatureDescriptor final {
  TypeDescriptor ReturnType;
  std::vector<TypeDescriptor> ParameterTypes;
  std::optional<TypeDescriptor> ReceiverType;
  bool ReceiverIsConst = false;
  bool IsVariadic = false;
  std::size_t RequiredParameterCount = 0;

  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::size_t Hash() const;
};

[[nodiscard]] std::strong_ordering
CompareSignature(const CallableSignatureDescriptor &Left,
                 const CallableSignatureDescriptor &Right);

[[nodiscard]] bool operator==(const CallableSignatureDescriptor &Left,
                              const CallableSignatureDescriptor &Right);

struct SymbolDescriptor final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string QualifiedName;
  SymbolId Parent;
  std::optional<ModuleProvenance> Module;
  std::optional<CallableSignatureDescriptor> Signature;
  std::optional<TypeDescriptor> AssociatedType;
  std::string TargetName;

  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::size_t Hash() const;
};

[[nodiscard]] std::strong_ordering
CompareSymbolDescriptor(const SymbolDescriptor &Left,
                        const SymbolDescriptor &Right);

[[nodiscard]] bool operator==(const SymbolDescriptor &Left,
                              const SymbolDescriptor &Right);

[[nodiscard]] SymbolDescriptor
MakeScopeSymbol(SymbolKind Kind, std::string QualifiedName, SymbolId Parent);

[[nodiscard]] SymbolDescriptor MakeModuleSymbol(std::string QualifiedName,
                                                SymbolId Parent,
                                                ModuleProvenance Provenance);

[[nodiscard]] SymbolDescriptor MakeOverloadSetSymbol(std::string QualifiedName,
                                                     SymbolId Parent);

[[nodiscard]] SymbolDescriptor
MakeCallableCandidateSymbol(SymbolKind Kind, std::string QualifiedName,
                            SymbolId Parent,
                            CallableSignatureDescriptor Signature);

[[nodiscard]] SymbolDescriptor
MakeEnumeratorAliasSymbol(std::string QualifiedName, SymbolId Parent,
                          TypeDescriptor Enumeration,
                          std::string CanonicalEnumeratorName);

[[nodiscard]] SymbolDescriptor MakeClassMemberSymbol(
    SymbolKind Kind, std::string QualifiedName, SymbolId Parent,
    TypeDescriptor OwnerType,
    std::optional<CallableSignatureDescriptor> Signature = std::nullopt);

[[nodiscard]] SymbolDescriptor
WithModuleProvenance(SymbolDescriptor Descriptor, ModuleProvenance Provenance);

} // namespace Luna::Detail
