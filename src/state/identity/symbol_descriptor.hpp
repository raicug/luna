#pragma once

// Complete canonical identity of one reflected symbol. A symbol identity is
// derived only from this descriptor: its schema version, symbol kind, canonical
// qualified name, parent symbol identity, module provenance when declaration
// ownership is module-specific, and the kind-disambiguating identity (a
// canonical signature, an owning type, or a canonical alias target). It never
// contains a registration order, a generation number, documentation text, or a
// mutable availability flag.

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

// Canonical separator of every qualified name. Segments are validated
// identifier segments, so no escaping is ever required.
inline constexpr char QualifiedNameSeparator = '.';

// Explicit Luna-owned policy bound on one canonical qualified name.
inline constexpr std::size_t MaximumQualifiedNameLength = 1024;

// Deterministic reason a qualified name is accepted or rejected.
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

// Identity of the module that owns one declaration.
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

// Canonical signature of one callable candidate. Every type is already a
// normalized canonical descriptor.
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

  // A descriptor is complete only when every field its kind requires is
  // present and every field its kind forbids is absent.
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::size_t Hash() const;
};

[[nodiscard]] std::strong_ordering
CompareSymbolDescriptor(const SymbolDescriptor &Left,
                        const SymbolDescriptor &Right);

[[nodiscard]] bool operator==(const SymbolDescriptor &Left,
                              const SymbolDescriptor &Right);

// Scope symbols: namespaces and enum/class scopes reached by qualified name.
[[nodiscard]] SymbolDescriptor
MakeScopeSymbol(SymbolKind Kind, std::string QualifiedName, SymbolId Parent);

// A module-owned scope or symbol keeps its manifest identity and version, so a
// different loaded version is a different declaration identity.
[[nodiscard]] SymbolDescriptor MakeModuleSymbol(std::string QualifiedName,
                                                SymbolId Parent,
                                                ModuleProvenance Provenance);

// One overload set groups every candidate sharing a qualified name; its
// identity depends only on the name and parent, never on candidate order.
[[nodiscard]] SymbolDescriptor MakeOverloadSetSymbol(std::string QualifiedName,
                                                     SymbolId Parent);

// One callable candidate adds its canonical signature to the overload-set
// identity.
[[nodiscard]] SymbolDescriptor
MakeCallableCandidateSymbol(SymbolKind Kind, std::string QualifiedName,
                            SymbolId Parent,
                            CallableSignatureDescriptor Signature);

// One enum alias identifies the canonical enumerator it names.
[[nodiscard]] SymbolDescriptor
MakeEnumeratorAliasSymbol(std::string QualifiedName, SymbolId Parent,
                          TypeDescriptor Enumeration,
                          std::string CanonicalEnumeratorName);

// One class member identity includes its declaring receiver type, so an
// inherited member view keeps the declaration owner's identity.
[[nodiscard]] SymbolDescriptor MakeClassMemberSymbol(
    SymbolKind Kind, std::string QualifiedName, SymbolId Parent,
    TypeDescriptor OwnerType,
    std::optional<CallableSignatureDescriptor> Signature = std::nullopt);

[[nodiscard]] SymbolDescriptor
WithModuleProvenance(SymbolDescriptor Descriptor, ModuleProvenance Provenance);

} // namespace Luna::Detail
