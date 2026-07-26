// clang-format off
#include "state/reflection/storage.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool IsScopeCapable(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Namespace:
  case SymbolKind::Module:
  case SymbolKind::Class:
  case SymbolKind::Enumeration:
  case SymbolKind::OverloadSet:
    return true;
  default:
    return false;
  }
}

// Only these kinds describe one reflected value of their own.
[[nodiscard]] bool PermitsValue(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Constant:
  case SymbolKind::Enumerator:
  case SymbolKind::EnumeratorAlias:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool RequiresSignature(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::FunctionCandidate:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

// Only a member of a class describes a receiver, access directions, and an
// evaluation policy of its own.
[[nodiscard]] bool PermitsMemberPolicy(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Property:
  case SymbolKind::Field:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

// A property and a field are the two kinds whose whole meaning is their
// declared directions, so one that permits neither describes nothing at all.
[[nodiscard]] bool RequiresMemberDirection(SymbolKind Kind) noexcept {
  return Kind == SymbolKind::Property || Kind == SymbolKind::Field;
}

[[nodiscard]] bool DescribesMemberPolicy(const ReflectionRecordFields &Fields) {
  return Fields.ReceiverType.IsValid() || Fields.MemberIsReadable ||
         Fields.MemberIsWritable || !Fields.MemberAccessText.empty() ||
         !Fields.MemberEvaluationText.empty() ||
         !Fields.MemberOwnershipText.empty();
}

// The local name must be the final canonical segment of the qualified name, so
// a record can never claim a name its qualified path does not contain.
[[nodiscard]] bool NameMatchesQualifiedName(std::string_view Name,
                                            std::string_view QualifiedName) {
  if (Name.size() > QualifiedName.size())
    return false;
  if (Name.size() == QualifiedName.size())
    return Name == QualifiedName;
  return QualifiedName.substr(QualifiedName.size() - Name.size()) == Name &&
         QualifiedName[QualifiedName.size() - Name.size() - 1] == '.';
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

[[nodiscard]] bool ModulePrecedes(const ReflectionModuleFields &Left,
                                  const ReflectionModuleFields &Right) {
  if (const auto Order = CompareText(Left.Identity, Right.Identity);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  if (const auto Order = CompareText(Left.Version, Right.Version);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return Left.Symbol < Right.Symbol;
}

// Canonical dependency order of one module: required identity, then the
// resolved version, then the declared constraint text.
[[nodiscard]] bool
ModuleDependencyPrecedes(const ReflectionModuleDependencyFields &Left,
                         const ReflectionModuleDependencyFields &Right) {
  if (const auto Order = CompareText(Left.Identity, Right.Identity);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  if (const auto Order = CompareText(Left.Version, Right.Version);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return CompareText(Left.Constraints, Right.Constraints) ==
         std::strong_ordering::less;
}

// Canonical export order of one module: qualified name, then symbol kind.
[[nodiscard]] bool
ModuleExportPrecedes(const ReflectionModuleExportFields &Left,
                     const ReflectionModuleExportFields &Right) {
  if (const auto Order = CompareText(Left.Name, Right.Name);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return static_cast<int>(Left.Kind) < static_cast<int>(Right.Kind);
}

[[nodiscard]] bool TextPrecedes(const std::string &Left,
                                const std::string &Right) {
  return CompareText(Left, Right) == std::strong_ordering::less;
}

// Canonical enumeration of one module. Dependencies, exports, namespaces, and
// types are sorted and deduplicated here, so enumeration never depends on
// manifest declaration order, resolution order, or load order.
[[nodiscard]] ReflectionGenerationStatus
NormalizeModule(ReflectionModuleFields &Fields) {
  std::stable_sort(Fields.Dependencies.begin(), Fields.Dependencies.end(),
                   ModuleDependencyPrecedes);
  std::stable_sort(Fields.Exports.begin(), Fields.Exports.end(),
                   ModuleExportPrecedes);
  std::stable_sort(Fields.Namespaces.begin(), Fields.Namespaces.end(),
                   TextPrecedes);
  std::stable_sort(Fields.Types.begin(), Fields.Types.end(), TextPrecedes);
  Fields.Namespaces.erase(
      std::unique(Fields.Namespaces.begin(), Fields.Namespaces.end()),
      Fields.Namespaces.end());
  Fields.Types.erase(std::unique(Fields.Types.begin(), Fields.Types.end()),
                     Fields.Types.end());

  for (const ReflectionModuleDependencyFields &Dependency :
       Fields.Dependencies) {
    if (Dependency.Identity.empty())
      return ReflectionGenerationStatus::IncompleteMetadata;
  }
  for (const ReflectionModuleExportFields &Export : Fields.Exports) {
    if (Export.Name.empty())
      return ReflectionGenerationStatus::IncompleteMetadata;
  }
  for (const std::string &Name : Fields.Namespaces) {
    if (Name.empty())
      return ReflectionGenerationStatus::IncompleteMetadata;
  }
  for (const std::string &Name : Fields.Types) {
    if (Name.empty())
      return ReflectionGenerationStatus::IncompleteMetadata;
  }
  return ReflectionGenerationStatus::Valid;
}

[[nodiscard]] bool TypePrecedes(const ReflectionTypeFields &Left,
                                const ReflectionTypeFields &Right) {
  if (const auto Order = CompareText(Left.Name, Right.Name);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return Left.Id < Right.Id;
}

[[nodiscard]] ReflectionGenerationStatus
ValidateParameters(const ReflectionRecordFields &Fields) {
  bool SawRelaxed = false;
  for (std::size_t Index = 0; Index < Fields.Parameters.size(); ++Index) {
    const ReflectionParameterFields &Parameter = Fields.Parameters[Index];
    if (Parameter.Name.empty() || !Parameter.Type.IsValid())
      return ReflectionGenerationStatus::IncompleteMetadata;
    const bool HasDefaultText = !Parameter.DefaultText.empty();
    if (Parameter.HasDefault !=
        (Parameter.Disposition == ParameterDisposition::Defaulted))
      return ReflectionGenerationStatus::InconsistentParameters;
    if (HasDefaultText && !Parameter.HasDefault)
      return ReflectionGenerationStatus::InconsistentParameters;
    if (Parameter.Disposition == ParameterDisposition::Variadic &&
        Index + 1 != Fields.Parameters.size())
      return ReflectionGenerationStatus::InconsistentParameters;
    if (Parameter.Disposition == ParameterDisposition::Required && SawRelaxed)
      return ReflectionGenerationStatus::InconsistentParameters;
    if (Parameter.Disposition != ParameterDisposition::Required)
      SawRelaxed = true;
  }
  return ReflectionGenerationStatus::Valid;
}

[[nodiscard]] ReflectionGenerationStatus
ValidateReturns(const ReflectionRecordFields &Fields) {
  for (const ReflectionReturnFields &Return : Fields.ReturnValues) {
    if (!Return.Type.IsValid())
      return ReflectionGenerationStatus::IncompleteMetadata;
  }
  switch (Fields.Returns) {
  case ReturnShape::Zero:
    if (!Fields.ReturnValues.empty())
      return ReflectionGenerationStatus::InconsistentReturns;
    break;
  case ReturnShape::Scalar:
    if (Fields.ReturnValues.size() != 1)
      return ReflectionGenerationStatus::InconsistentReturns;
    break;
  case ReturnShape::Multiple:
    // A statically declared pack names every value it publishes, so it carries
    // at least two. A dynamic return pack decides its element count and element
    // types per call and therefore declares no per-value record at all; what it
    // never does is claim exactly one value while reporting the multiple shape.
    if (Fields.ReturnValues.size() == 1)
      return ReflectionGenerationStatus::InconsistentReturns;
    break;
  }
  return ReflectionGenerationStatus::Valid;
}

} // namespace

std::string_view
ReflectionGenerationStatusText(ReflectionGenerationStatus Status) noexcept {
  switch (Status) {
  case ReflectionGenerationStatus::Valid:
    return "valid";
  case ReflectionGenerationStatus::InvalidIdentity:
    return "invalid-identity";
  case ReflectionGenerationStatus::IncompleteMetadata:
    return "incomplete-metadata";
  case ReflectionGenerationStatus::DuplicateIdentity:
    return "duplicate-identity";
  case ReflectionGenerationStatus::DuplicateQualifiedName:
    return "duplicate-qualified-name";
  case ReflectionGenerationStatus::InconsistentScope:
    return "inconsistent-scope";
  case ReflectionGenerationStatus::InconsistentDeclaration:
    return "inconsistent-declaration";
  case ReflectionGenerationStatus::InconsistentOverloadSet:
    return "inconsistent-overload-set";
  case ReflectionGenerationStatus::InconsistentParameters:
    return "inconsistent-parameters";
  case ReflectionGenerationStatus::InconsistentReturns:
    return "inconsistent-returns";
  case ReflectionGenerationStatus::InconsistentModule:
    return "inconsistent-module";
  case ReflectionGenerationStatus::InconsistentType:
    return "inconsistent-type";
  }
  return "invalid";
}

bool ReflectionStorage::RecordPrecedes(const ReflectionRecordFields &Left,
                                       const ReflectionRecordFields &Right) {
  if (const auto Order = CompareText(Left.QualifiedName, Right.QualifiedName);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  if (Left.Kind != Right.Kind)
    return static_cast<int>(Left.Kind) < static_cast<int>(Right.Kind);
  if (const auto Order = CompareText(Left.Signature, Right.Signature);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return Left.Id < Right.Id;
}

std::shared_ptr<const ReflectionStorage> ReflectionStorage::Empty() {
  static const std::shared_ptr<const ReflectionStorage> Shared = [] {
    ReflectionGenerationStatus Status = ReflectionGenerationStatus::Valid;
    return Build(0, {}, {}, {}, Status);
  }();
  return Shared;
}

std::shared_ptr<const ReflectionStorage>
ReflectionStorage::Build(std::uint64_t Generation,
                         std::vector<ReflectionRecordFields> Records,
                         std::vector<ReflectionTypeFields> Types,
                         std::vector<ReflectionModuleFields> Modules,
                         ReflectionGenerationStatus &Status) {
  Status = ReflectionGenerationStatus::Valid;

  for (const ReflectionRecordFields &Fields : Records) {
    if (Fields.Module && *Fields.Module >= Modules.size()) {
      Status = ReflectionGenerationStatus::InconsistentModule;
      return nullptr;
    }
  }

  // Canonical module order first, then remap every record's provenance index so
  // ordering never depends on submission order.
  std::vector<std::size_t> ModulePermutation(Modules.size());
  {
    std::vector<std::size_t> Order(Modules.size());
    for (std::size_t Index = 0; Index < Order.size(); ++Index)
      Order[Index] = Index;
    std::stable_sort(Order.begin(), Order.end(),
                     [&Modules](std::size_t Left, std::size_t Right) {
                       return ModulePrecedes(Modules[Left], Modules[Right]);
                     });
    std::vector<ReflectionModuleFields> Sorted;
    Sorted.reserve(Modules.size());
    for (std::size_t Position = 0; Position < Order.size(); ++Position) {
      ModulePermutation[Order[Position]] = Position;
      Sorted.push_back(std::move(Modules[Order[Position]]));
    }
    Modules = std::move(Sorted);
  }
  for (ReflectionRecordFields &Fields : Records) {
    if (Fields.Module)
      Fields.Module = ModulePermutation[*Fields.Module];
  }

  std::stable_sort(Types.begin(), Types.end(), TypePrecedes);
  std::stable_sort(Records.begin(), Records.end(), RecordPrecedes);

  auto Storage = std::shared_ptr<ReflectionStorage>(new ReflectionStorage());
  Storage->GenerationValue = Generation;

  // Records are validated in canonical order, so the first rejection reason is
  // independent of how the candidate generation was assembled.
  for (std::size_t Index = 0; Index < Records.size(); ++Index) {
    const ReflectionRecordFields &Fields = Records[Index];
    if (!Fields.Id.IsValid()) {
      Status = ReflectionGenerationStatus::InvalidIdentity;
      return nullptr;
    }
    if (Fields.Name.empty() || Fields.QualifiedName.empty() ||
        !NameMatchesQualifiedName(Fields.Name, Fields.QualifiedName)) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    if (RequiresSignature(Fields.Kind) && Fields.Signature.empty()) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    // Only a constant, an enumerator, or an enumerator alias carries a value,
    // and a carried value always names the canonical type it converts through.
    if (Fields.ValueIsAvailable &&
        (!PermitsValue(Fields.Kind) || !Fields.Type.IsValid())) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    if (!Fields.ValueIsAvailable && !Fields.ValueText.empty()) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    // A receiver, an access direction, and an evaluation policy belong to a
    // class member; a property or a field additionally has to permit at least
    // one direction, because that is the whole content of its declaration.
    if (DescribesMemberPolicy(Fields) && !PermitsMemberPolicy(Fields.Kind)) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    if (RequiresMemberDirection(Fields.Kind) &&
        !(Fields.MemberIsReadable || Fields.MemberIsWritable)) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    for (const ReflectionAttributeFields &Attribute : Fields.Attributes) {
      if (Attribute.Name.empty()) {
        Status = ReflectionGenerationStatus::IncompleteMetadata;
        return nullptr;
      }
    }
    for (const ReflectionRelationFields &Relation : Fields.Relations) {
      if (!Relation.Type.IsValid()) {
        Status = ReflectionGenerationStatus::InconsistentType;
        return nullptr;
      }
    }
    if (const auto Reason = ValidateParameters(Fields);
        Reason != ReflectionGenerationStatus::Valid) {
      Status = Reason;
      return nullptr;
    }
    if (const auto Reason = ValidateReturns(Fields);
        Reason != ReflectionGenerationStatus::Valid) {
      Status = Reason;
      return nullptr;
    }
    if (!Storage->IdLookup.emplace(Fields.Id, Index).second) {
      Status = ReflectionGenerationStatus::DuplicateIdentity;
      return nullptr;
    }
    if (Index > 0) {
      const ReflectionRecordFields &Previous = Records[Index - 1];
      if (Previous.QualifiedName == Fields.QualifiedName &&
          Previous.Kind == Fields.Kind &&
          Previous.Signature == Fields.Signature) {
        Status = ReflectionGenerationStatus::DuplicateQualifiedName;
        return nullptr;
      }
    }
    // Canonical order places an overload set before its candidates, so the
    // first record for a qualified name is the primary lookup result.
    Storage->NameLookup.emplace(Fields.QualifiedName, Index);
  }

  Storage->Records = std::move(Records);

  // Relationship checks need the completed identity index.
  for (const ReflectionRecordFields &Fields : Storage->Records) {
    if (!Fields.Scope.IsRoot()) {
      const auto Owner = Storage->IdLookup.find(Fields.Scope.Owner());
      if (Owner == Storage->IdLookup.end() ||
          !IsScopeCapable(Storage->Records[Owner->second].Kind)) {
        Status = ReflectionGenerationStatus::InconsistentScope;
        return nullptr;
      }
    }
    if (Fields.Declaration.IsValid() && Fields.Declaration != Fields.Id &&
        Storage->IdLookup.find(Fields.Declaration) == Storage->IdLookup.end()) {
      Status = ReflectionGenerationStatus::InconsistentDeclaration;
      return nullptr;
    }
    if (Fields.OverloadSet.IsValid()) {
      const auto Owner = Storage->IdLookup.find(Fields.OverloadSet);
      if (Owner == Storage->IdLookup.end() ||
          Storage->Records[Owner->second].Kind != SymbolKind::OverloadSet) {
        Status = ReflectionGenerationStatus::InconsistentOverloadSet;
        return nullptr;
      }
    }
  }

  for (std::size_t Index = 0; Index < Types.size(); ++Index) {
    const ReflectionTypeFields &Fields = Types[Index];
    if (!Fields.Id.IsValid()) {
      Status = ReflectionGenerationStatus::InvalidIdentity;
      return nullptr;
    }
    if (Fields.Name.empty()) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    if (!Fields.Descriptor.IsValid()) {
      Status = ReflectionGenerationStatus::InconsistentType;
      return nullptr;
    }
    if (Fields.Declaration.IsValid() &&
        Storage->IdLookup.find(Fields.Declaration) == Storage->IdLookup.end()) {
      Status = ReflectionGenerationStatus::InconsistentDeclaration;
      return nullptr;
    }
    if (!Storage->TypeLookup.emplace(Fields.Id, Index).second) {
      Status = ReflectionGenerationStatus::DuplicateIdentity;
      return nullptr;
    }
  }
  Storage->Types = std::move(Types);

  for (std::size_t Index = 0; Index < Modules.size(); ++Index) {
    ReflectionModuleFields &Fields = Modules[Index];
    if (Fields.Identity.empty() || Fields.Version.empty()) {
      Status = ReflectionGenerationStatus::IncompleteMetadata;
      return nullptr;
    }
    // Canonical dependency, export, namespace, and type enumeration of this
    // module, independent of the order the manifest or the load produced.
    if (const auto Reason = NormalizeModule(Fields);
        Reason != ReflectionGenerationStatus::Valid) {
      Status = Reason;
      return nullptr;
    }
    if (Index > 0 && Modules[Index - 1].Identity == Fields.Identity) {
      Status = ReflectionGenerationStatus::InconsistentModule;
      return nullptr;
    }
    if (Fields.Symbol.IsValid()) {
      const auto Owner = Storage->IdLookup.find(Fields.Symbol);
      if (Owner == Storage->IdLookup.end() ||
          Storage->Records[Owner->second].Kind != SymbolKind::Module) {
        Status = ReflectionGenerationStatus::InconsistentModule;
        return nullptr;
      }
    }
  }
  Storage->Modules = std::move(Modules);

  Storage->SymbolOrder.resize(Storage->Records.size());
  for (std::size_t Index = 0; Index < Storage->Records.size(); ++Index) {
    Storage->SymbolOrder[Index] = Index;
    const ReflectionRecordFields &Fields = Storage->Records[Index];
    Storage->KindIndices[static_cast<std::size_t>(Fields.Kind)].push_back(
        Index);
    const auto Existing = Storage->ScopeLookup.find(Fields.Scope);
    if (Existing == Storage->ScopeLookup.end()) {
      Storage->ScopeLookup.emplace(Fields.Scope, Storage->ScopeIndices.size());
      Storage->ScopeIndices.push_back({Index});
      continue;
    }
    Storage->ScopeIndices[Existing->second].push_back(Index);
  }

  Storage->TypeIndices.resize(Storage->Types.size());
  for (std::size_t Index = 0; Index < Storage->Types.size(); ++Index)
    Storage->TypeIndices[Index] = Index;
  Storage->ModuleIndices.resize(Storage->Modules.size());
  for (std::size_t Index = 0; Index < Storage->Modules.size(); ++Index)
    Storage->ModuleIndices[Index] = Index;

  return Storage;
}

const ReflectionRecordFields *
ReflectionStorage::RecordAt(std::size_t Index) const noexcept {
  return Index < Records.size() ? &Records[Index] : nullptr;
}

const ReflectionTypeFields *
ReflectionStorage::TypeAt(std::size_t Index) const noexcept {
  return Index < Types.size() ? &Types[Index] : nullptr;
}

const ReflectionModuleFields *
ReflectionStorage::ModuleAt(std::size_t Index) const noexcept {
  return Index < Modules.size() ? &Modules[Index] : nullptr;
}

const ReflectionModuleDependencyFields *ReflectionStorage::ModuleDependencyAt(
    std::size_t ModuleIndex, std::size_t DependencyIndex) const noexcept {
  const ReflectionModuleFields *Fields = ModuleAt(ModuleIndex);
  if (!Fields || DependencyIndex >= Fields->Dependencies.size())
    return nullptr;
  return &Fields->Dependencies[DependencyIndex];
}

const ReflectionModuleExportFields *
ReflectionStorage::ModuleExportAt(std::size_t ModuleIndex,
                                  std::size_t ExportIndex) const noexcept {
  const ReflectionModuleFields *Fields = ModuleAt(ModuleIndex);
  if (!Fields || ExportIndex >= Fields->Exports.size())
    return nullptr;
  return &Fields->Exports[ExportIndex];
}

const ReflectionParameterFields *
ReflectionStorage::ParameterAt(std::size_t RecordIndex,
                               std::size_t ParameterIndex) const noexcept {
  const ReflectionRecordFields *Fields = RecordAt(RecordIndex);
  if (!Fields || ParameterIndex >= Fields->Parameters.size())
    return nullptr;
  return &Fields->Parameters[ParameterIndex];
}

const ReflectionReturnFields *
ReflectionStorage::ReturnAt(std::size_t RecordIndex,
                            std::size_t ReturnIndex) const noexcept {
  const ReflectionRecordFields *Fields = RecordAt(RecordIndex);
  if (!Fields || ReturnIndex >= Fields->ReturnValues.size())
    return nullptr;
  return &Fields->ReturnValues[ReturnIndex];
}

const ReflectionAttributeFields *
ReflectionStorage::AttributeAt(std::size_t RecordIndex,
                               std::size_t AttributeIndex) const noexcept {
  const ReflectionRecordFields *Fields = RecordAt(RecordIndex);
  if (!Fields || AttributeIndex >= Fields->Attributes.size())
    return nullptr;
  return &Fields->Attributes[AttributeIndex];
}

const ReflectionRelationFields *
ReflectionStorage::RelationAt(std::size_t RecordIndex,
                              std::size_t RelationIndex) const noexcept {
  const ReflectionRecordFields *Fields = RecordAt(RecordIndex);
  if (!Fields || RelationIndex >= Fields->Relations.size())
    return nullptr;
  return &Fields->Relations[RelationIndex];
}

std::optional<std::size_t>
ReflectionStorage::IndexOf(const SymbolId &Id) const noexcept {
  const auto Found = IdLookup.find(Id);
  if (Found == IdLookup.end())
    return std::nullopt;
  return Found->second;
}

std::optional<std::size_t>
ReflectionStorage::IndexOf(std::string_view QualifiedName) const noexcept {
  const auto Found = NameLookup.find(QualifiedName);
  if (Found == NameLookup.end())
    return std::nullopt;
  return Found->second;
}

std::optional<std::size_t>
ReflectionStorage::TypeIndexOf(const TypeId &Id) const noexcept {
  const auto Found = TypeLookup.find(Id);
  if (Found == TypeLookup.end())
    return std::nullopt;
  return Found->second;
}

const std::vector<std::size_t> &
ReflectionStorage::OrderOfKind(SymbolKind Kind) const noexcept {
  const std::size_t Position = static_cast<std::size_t>(Kind);
  if (Position >= KindIndices.size())
    return EmptyIndices;
  return KindIndices[Position];
}

const std::vector<std::size_t> &
ReflectionStorage::OrderOfScope(const ScopeId &Scope) const noexcept {
  const auto Found = ScopeLookup.find(Scope);
  if (Found == ScopeLookup.end())
    return EmptyIndices;
  return ScopeIndices[Found->second];
}

ReflectionSnapshot ReflectionStorage::MakeSnapshot(
    std::shared_ptr<const ReflectionStorage> Storage) {
  return ReflectionSnapshot(std::move(Storage));
}

ReflectionRecord
ReflectionStorage::MakeRecord(std::shared_ptr<const ReflectionStorage> Storage,
                              std::size_t RecordIndex) {
  return ReflectionRecord(std::move(Storage), RecordIndex);
}

TypeRecord
ReflectionStorage::MakeType(std::shared_ptr<const ReflectionStorage> Storage,
                            std::size_t TypeIndex) {
  return TypeRecord(std::move(Storage), TypeIndex);
}

ModuleDependencyRecord ReflectionStorage::MakeModuleDependency(
    std::shared_ptr<const ReflectionStorage> Storage, std::size_t ModuleIndex,
    std::size_t DependencyIndex) {
  return ModuleDependencyRecord(std::move(Storage), ModuleIndex,
                                DependencyIndex);
}

ModuleExportRecord ReflectionStorage::MakeModuleExport(
    std::shared_ptr<const ReflectionStorage> Storage, std::size_t ModuleIndex,
    std::size_t ExportIndex) {
  return ModuleExportRecord(std::move(Storage), ModuleIndex, ExportIndex);
}

ModuleRecord
ReflectionStorage::MakeModule(std::shared_ptr<const ReflectionStorage> Storage,
                              std::size_t ModuleIndex) {
  return ModuleRecord(std::move(Storage), ModuleIndex);
}

ParameterRecord ReflectionStorage::MakeParameter(
    std::shared_ptr<const ReflectionStorage> Storage, std::size_t RecordIndex,
    std::size_t ParameterIndex) {
  return ParameterRecord(std::move(Storage), RecordIndex, ParameterIndex);
}

ReturnRecord
ReflectionStorage::MakeReturn(std::shared_ptr<const ReflectionStorage> Storage,
                              std::size_t RecordIndex,
                              std::size_t ReturnIndex) {
  return ReturnRecord(std::move(Storage), RecordIndex, ReturnIndex);
}

AttributeRecord ReflectionStorage::MakeAttribute(
    std::shared_ptr<const ReflectionStorage> Storage, std::size_t RecordIndex,
    std::size_t AttributeIndex) {
  return AttributeRecord(std::move(Storage), RecordIndex, AttributeIndex);
}

TypeRelation ReflectionStorage::MakeRelation(
    std::shared_ptr<const ReflectionStorage> Storage, std::size_t RecordIndex,
    std::size_t RelationIndex) {
  return TypeRelation(std::move(Storage), RecordIndex, RelationIndex);
}

ReflectionRecordRange ReflectionStorage::MakeRecordRange(
    std::shared_ptr<const ReflectionStorage> Storage,
    const std::vector<std::size_t> &Indices) {
  if (Indices.empty())
    return ReflectionRecordRange();
  return ReflectionRecordRange(std::move(Storage), Indices.data(),
                               Indices.size());
}

TypeRecordRange ReflectionStorage::MakeTypeRange(
    std::shared_ptr<const ReflectionStorage> Storage,
    const std::vector<std::size_t> &Indices) {
  if (Indices.empty())
    return TypeRecordRange();
  return TypeRecordRange(std::move(Storage), Indices.data(), Indices.size());
}

ModuleRecordRange ReflectionStorage::MakeModuleRange(
    std::shared_ptr<const ReflectionStorage> Storage,
    const std::vector<std::size_t> &Indices) {
  if (Indices.empty())
    return ModuleRecordRange();
  return ModuleRecordRange(std::move(Storage), Indices.data(), Indices.size());
}

} // namespace Luna::Detail
