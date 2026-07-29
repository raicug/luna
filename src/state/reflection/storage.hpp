#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/type/type_descriptor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
// clang-format on

namespace Luna::Detail {

enum class ReflectionGenerationStatus {
  Valid,
  InvalidIdentity,
  IncompleteMetadata,
  DuplicateIdentity,
  DuplicateQualifiedName,
  InconsistentScope,
  InconsistentDeclaration,
  InconsistentOverloadSet,
  InconsistentParameters,
  InconsistentReturns,
  InconsistentModule,
  InconsistentType
};

[[nodiscard]] std::string_view
ReflectionGenerationStatusText(ReflectionGenerationStatus Status) noexcept;

struct ReflectionParameterFields final {
  std::string Name;
  TypeId Type;
  TypeDescriptor Descriptor;
  ParameterDisposition Disposition = ParameterDisposition::Required;
  bool HasDefault = false;
  std::string DefaultText;
  std::string Documentation;
};

struct ReflectionReturnFields final {
  std::string Name;
  TypeId Type;
  TypeDescriptor Descriptor;
};

struct ReflectionAttributeFields final {
  std::string Name;
  std::string Value;
};

struct ReflectionRelationFields final {
  TypeRelationKind Kind = TypeRelationKind::Declared;
  TypeId Type;
  SymbolId Declaration;
  std::string Note;
};

struct ReflectionModuleDependencyFields final {
  std::string Identity;
  std::string Version;
  std::string Constraints;
};

struct ReflectionModuleExportFields final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string Name;
  std::string Documentation;
};

struct ReflectionModuleFields final {
  std::string Identity;
  std::string Version;
  SymbolId Symbol;
  std::string Documentation;

  std::vector<ReflectionModuleDependencyFields> Dependencies;
  std::vector<ReflectionModuleExportFields> Exports;
  std::vector<std::string> Namespaces;
  std::vector<std::string> Types;
};

struct ReflectionTypeFields final {
  TypeId Id;
  std::string Name;
  TypeDescriptor Descriptor;
  SymbolId Declaration;
};

struct ReflectionRecordFields final {
  SymbolKind Kind = SymbolKind::Namespace;
  SymbolId Id;
  std::string Name;
  std::string QualifiedName;
  std::string Signature;
  ScopeId Scope;
  SymbolId Declaration;
  SymbolId OverloadSet;
  TypeId Type;
  TypeDescriptor Descriptor;
  ReturnShape Returns = ReturnShape::Zero;
  bool ReturnsAsynchronously = false;

  bool ValueIsAvailable = false;
  std::string ValueText;

  std::string OwnershipResult;
  std::string AllocatorPolicy;

  TypeId ReceiverType;
  bool ReceiverPermitsConst = false;
  bool MemberIsReadable = false;
  bool MemberIsWritable = false;
  std::string MemberAccessText;
  std::string MemberEvaluationText;
  std::string MemberOwnershipText;

  std::string Documentation;
  std::vector<std::string> Examples;
  std::vector<ReflectionParameterFields> Parameters;
  std::vector<ReflectionReturnFields> ReturnValues;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<ReflectionRelationFields> Relations;
  std::optional<std::size_t> Module;
};

class ReflectionStorage final {
public:
  static constexpr std::size_t SymbolKindCount =
      static_cast<std::size_t>(SymbolKind::Type) + 1;

  [[nodiscard]] static std::shared_ptr<const ReflectionStorage> Empty();

  [[nodiscard]] static std::shared_ptr<const ReflectionStorage>
  Build(std::uint64_t Generation, std::vector<ReflectionRecordFields> Records,
        std::vector<ReflectionTypeFields> Types,
        std::vector<ReflectionModuleFields> Modules,
        ReflectionGenerationStatus &Status);

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  [[nodiscard]] std::size_t RecordCount() const noexcept {
    return Records.size();
  }

  [[nodiscard]] std::size_t TypeCount() const noexcept { return Types.size(); }

  [[nodiscard]] std::size_t ModuleCount() const noexcept {
    return Modules.size();
  }

  [[nodiscard]] const ReflectionRecordFields *
  RecordAt(std::size_t Index) const noexcept;
  [[nodiscard]] const ReflectionTypeFields *
  TypeAt(std::size_t Index) const noexcept;
  [[nodiscard]] const ReflectionModuleFields *
  ModuleAt(std::size_t Index) const noexcept;
  [[nodiscard]] const ReflectionModuleDependencyFields *
  ModuleDependencyAt(std::size_t ModuleIndex,
                     std::size_t DependencyIndex) const noexcept;
  [[nodiscard]] const ReflectionModuleExportFields *
  ModuleExportAt(std::size_t ModuleIndex,
                 std::size_t ExportIndex) const noexcept;
  [[nodiscard]] const ReflectionParameterFields *
  ParameterAt(std::size_t RecordIndex,
              std::size_t ParameterIndex) const noexcept;
  [[nodiscard]] const ReflectionReturnFields *
  ReturnAt(std::size_t RecordIndex, std::size_t ReturnIndex) const noexcept;
  [[nodiscard]] const ReflectionAttributeFields *
  AttributeAt(std::size_t RecordIndex,
              std::size_t AttributeIndex) const noexcept;
  [[nodiscard]] const ReflectionRelationFields *
  RelationAt(std::size_t RecordIndex, std::size_t RelationIndex) const noexcept;

  [[nodiscard]] std::optional<std::size_t>
  IndexOf(const SymbolId &Id) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  IndexOf(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  TypeIndexOf(const TypeId &Id) const noexcept;

  [[nodiscard]] static ReflectionSnapshot
  MakeSnapshot(std::shared_ptr<const ReflectionStorage> Storage);
  [[nodiscard]] static ReflectionRecord
  MakeRecord(std::shared_ptr<const ReflectionStorage> Storage,
             std::size_t RecordIndex);
  [[nodiscard]] static TypeRecord
  MakeType(std::shared_ptr<const ReflectionStorage> Storage,
           std::size_t TypeIndex);
  [[nodiscard]] static ModuleRecord
  MakeModule(std::shared_ptr<const ReflectionStorage> Storage,
             std::size_t ModuleIndex);
  [[nodiscard]] static ModuleDependencyRecord
  MakeModuleDependency(std::shared_ptr<const ReflectionStorage> Storage,
                       std::size_t ModuleIndex, std::size_t DependencyIndex);
  [[nodiscard]] static ModuleExportRecord
  MakeModuleExport(std::shared_ptr<const ReflectionStorage> Storage,
                   std::size_t ModuleIndex, std::size_t ExportIndex);
  [[nodiscard]] static ParameterRecord
  MakeParameter(std::shared_ptr<const ReflectionStorage> Storage,
                std::size_t RecordIndex, std::size_t ParameterIndex);
  [[nodiscard]] static ReturnRecord
  MakeReturn(std::shared_ptr<const ReflectionStorage> Storage,
             std::size_t RecordIndex, std::size_t ReturnIndex);
  [[nodiscard]] static AttributeRecord
  MakeAttribute(std::shared_ptr<const ReflectionStorage> Storage,
                std::size_t RecordIndex, std::size_t AttributeIndex);
  [[nodiscard]] static TypeRelation
  MakeRelation(std::shared_ptr<const ReflectionStorage> Storage,
               std::size_t RecordIndex, std::size_t RelationIndex);
  [[nodiscard]] static ReflectionRecordRange
  MakeRecordRange(std::shared_ptr<const ReflectionStorage> Storage,
                  const std::vector<std::size_t> &Indices);
  [[nodiscard]] static TypeRecordRange
  MakeTypeRange(std::shared_ptr<const ReflectionStorage> Storage,
                const std::vector<std::size_t> &Indices);
  [[nodiscard]] static ModuleRecordRange
  MakeModuleRange(std::shared_ptr<const ReflectionStorage> Storage,
                  const std::vector<std::size_t> &Indices);

  [[nodiscard]] const std::vector<std::size_t> &AllOrder() const noexcept {
    return SymbolOrder;
  }
  [[nodiscard]] const std::vector<std::size_t> &TypeOrder() const noexcept {
    return TypeIndices;
  }
  [[nodiscard]] const std::vector<std::size_t> &ModuleOrder() const noexcept {
    return ModuleIndices;
  }
  [[nodiscard]] const std::vector<std::size_t> &
  OrderOfKind(SymbolKind Kind) const noexcept;
  [[nodiscard]] const std::vector<std::size_t> &
  OrderOfScope(const ScopeId &Scope) const noexcept;

  [[nodiscard]] static bool RecordPrecedes(const ReflectionRecordFields &Left,
                                           const ReflectionRecordFields &Right);

private:
  ReflectionStorage() = default;

  std::uint64_t GenerationValue = 0;
  std::vector<ReflectionRecordFields> Records;
  std::vector<ReflectionTypeFields> Types;
  std::vector<ReflectionModuleFields> Modules;
  std::vector<std::size_t> SymbolOrder;
  std::vector<std::size_t> TypeIndices;
  std::vector<std::size_t> ModuleIndices;
  std::array<std::vector<std::size_t>, SymbolKindCount> KindIndices;
  std::vector<std::vector<std::size_t>> ScopeIndices;
  std::map<ScopeId, std::size_t> ScopeLookup;
  std::unordered_map<SymbolId, std::size_t, CanonicalHash> IdLookup;
  std::map<std::string, std::size_t, std::less<>> NameLookup;
  std::unordered_map<TypeId, std::size_t, CanonicalHash> TypeLookup;
  std::vector<std::size_t> EmptyIndices;
};

} // namespace Luna::Detail
