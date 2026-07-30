// clang-format off
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include "state/reflection/storage.hpp"

#include <cstddef>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {
namespace {

[[nodiscard]] const Detail::ReflectionStorage *
Resolve(const std::shared_ptr<const Detail::ReflectionStorage> &Storage) {
  return Storage ? Storage.get() : Detail::ReflectionStorage::Empty().get();
}

} // namespace

ParameterRecord::ParameterRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t RecordIndex, std::size_t ParameterIndex) noexcept
    : StorageValue(std::move(Storage)), RecordIndexValue(RecordIndex),
      ParameterIndexValue(ParameterIndex) {}

bool ParameterRecord::IsValid() const noexcept {
  return StorageValue &&
         StorageValue->ParameterAt(RecordIndexValue, ParameterIndexValue);
}

std::string_view ParameterRecord::Name() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

TypeId ParameterRecord::Type() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? Fields->Type : TypeId();
}

TypeDescriptor ParameterRecord::Descriptor() const {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? Fields->Descriptor : TypeDescriptor::Unsupported();
}

ParameterDisposition ParameterRecord::Disposition() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? Fields->Disposition : ParameterDisposition::Required;
}

bool ParameterRecord::HasDefault() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields && Fields->HasDefault;
}

std::string_view ParameterRecord::DefaultText() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? std::string_view(Fields->DefaultText) : std::string_view();
}

std::string_view ParameterRecord::Documentation() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ParameterAt(RecordIndexValue, ParameterIndexValue);
  return Fields ? std::string_view(Fields->Documentation) : std::string_view();
}

ReturnRecord::ReturnRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t RecordIndex, std::size_t ReturnIndex) noexcept
    : StorageValue(std::move(Storage)), RecordIndexValue(RecordIndex),
      ReturnIndexValue(ReturnIndex) {}

bool ReturnRecord::IsValid() const noexcept {
  return StorageValue &&
         StorageValue->ReturnAt(RecordIndexValue, ReturnIndexValue);
}

std::string_view ReturnRecord::Name() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ReturnAt(RecordIndexValue, ReturnIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

TypeId ReturnRecord::Type() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ReturnAt(RecordIndexValue, ReturnIndexValue);
  return Fields ? Fields->Type : TypeId();
}

TypeDescriptor ReturnRecord::Descriptor() const {
  const auto *Fields =
      Resolve(StorageValue)->ReturnAt(RecordIndexValue, ReturnIndexValue);
  return Fields ? Fields->Descriptor : TypeDescriptor::Unsupported();
}

AttributeRecord::AttributeRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t RecordIndex, std::size_t AttributeIndex) noexcept
    : StorageValue(std::move(Storage)), RecordIndexValue(RecordIndex),
      AttributeIndexValue(AttributeIndex) {}

bool AttributeRecord::IsValid() const noexcept {
  return StorageValue &&
         StorageValue->AttributeAt(RecordIndexValue, AttributeIndexValue);
}

std::string_view AttributeRecord::Name() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->AttributeAt(RecordIndexValue, AttributeIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

std::string_view AttributeRecord::Value() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->AttributeAt(RecordIndexValue, AttributeIndexValue);
  return Fields ? std::string_view(Fields->Value) : std::string_view();
}

TypeRelation::TypeRelation(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t RecordIndex, std::size_t RelationIndex) noexcept
    : StorageValue(std::move(Storage)), RecordIndexValue(RecordIndex),
      RelationIndexValue(RelationIndex) {}

bool TypeRelation::IsValid() const noexcept {
  return StorageValue &&
         StorageValue->RelationAt(RecordIndexValue, RelationIndexValue);
}

TypeRelationKind TypeRelation::Kind() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->RelationAt(RecordIndexValue, RelationIndexValue);
  return Fields ? Fields->Kind : TypeRelationKind::Declared;
}

TypeId TypeRelation::Type() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->RelationAt(RecordIndexValue, RelationIndexValue);
  return Fields ? Fields->Type : TypeId();
}

SymbolId TypeRelation::Declaration() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->RelationAt(RecordIndexValue, RelationIndexValue);
  return Fields ? Fields->Declaration : SymbolId();
}

std::string_view TypeRelation::Note() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->RelationAt(RecordIndexValue, RelationIndexValue);
  return Fields ? std::string_view(Fields->Note) : std::string_view();
}

ModuleDependencyRecord::ModuleDependencyRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t ModuleIndex, std::size_t DependencyIndex) noexcept
    : StorageValue(std::move(Storage)), ModuleIndexValue(ModuleIndex),
      DependencyIndexValue(DependencyIndex) {}

bool ModuleDependencyRecord::IsValid() const noexcept {
  return StorageValue && StorageValue->ModuleDependencyAt(ModuleIndexValue,
                                                          DependencyIndexValue);
}

std::string_view ModuleDependencyRecord::Identity() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)
          ->ModuleDependencyAt(ModuleIndexValue, DependencyIndexValue);
  return Fields ? std::string_view(Fields->Identity) : std::string_view();
}

std::string_view ModuleDependencyRecord::Version() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)
          ->ModuleDependencyAt(ModuleIndexValue, DependencyIndexValue);
  return Fields ? std::string_view(Fields->Version) : std::string_view();
}

std::string_view ModuleDependencyRecord::Constraints() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)
          ->ModuleDependencyAt(ModuleIndexValue, DependencyIndexValue);
  return Fields ? std::string_view(Fields->Constraints) : std::string_view();
}

ModuleExportRecord::ModuleExportRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t ModuleIndex, std::size_t ExportIndex) noexcept
    : StorageValue(std::move(Storage)), ModuleIndexValue(ModuleIndex),
      ExportIndexValue(ExportIndex) {}

bool ModuleExportRecord::IsValid() const noexcept {
  return StorageValue &&
         StorageValue->ModuleExportAt(ModuleIndexValue, ExportIndexValue);
}

SymbolKind ModuleExportRecord::Kind() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ModuleExportAt(ModuleIndexValue, ExportIndexValue);
  return Fields ? Fields->Kind : SymbolKind::Namespace;
}

std::string_view ModuleExportRecord::Name() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ModuleExportAt(ModuleIndexValue, ExportIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

std::string_view ModuleExportRecord::Documentation() const noexcept {
  const auto *Fields =
      Resolve(StorageValue)->ModuleExportAt(ModuleIndexValue, ExportIndexValue);
  return Fields ? std::string_view(Fields->Documentation) : std::string_view();
}

ModuleRecord::ModuleRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t ModuleIndex) noexcept
    : StorageValue(std::move(Storage)), ModuleIndexValue(ModuleIndex) {}

bool ModuleRecord::IsValid() const noexcept {
  return StorageValue && StorageValue->ModuleAt(ModuleIndexValue);
}

std::string_view ModuleRecord::Identity() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? std::string_view(Fields->Identity) : std::string_view();
}

std::string_view ModuleRecord::Version() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? std::string_view(Fields->Version) : std::string_view();
}

SymbolId ModuleRecord::Symbol() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? Fields->Symbol : SymbolId();
}

std::string_view ModuleRecord::Documentation() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? std::string_view(Fields->Documentation) : std::string_view();
}

std::size_t ModuleRecord::DependencyCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? Fields->Dependencies.size() : 0;
}

ModuleDependencyRecord ModuleRecord::Dependency(std::size_t Index) const {
  if (!StorageValue ||
      !StorageValue->ModuleDependencyAt(ModuleIndexValue, Index))
    return ModuleDependencyRecord();
  return Detail::ReflectionStorage::MakeModuleDependency(
      StorageValue, ModuleIndexValue, Index);
}

std::size_t ModuleRecord::ExportCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? Fields->Exports.size() : 0;
}

ModuleExportRecord ModuleRecord::Export(std::size_t Index) const {
  if (!StorageValue || !StorageValue->ModuleExportAt(ModuleIndexValue, Index))
    return ModuleExportRecord();
  return Detail::ReflectionStorage::MakeModuleExport(StorageValue,
                                                     ModuleIndexValue, Index);
}

std::size_t ModuleRecord::NamespaceCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? Fields->Namespaces.size() : 0;
}

std::string_view ModuleRecord::Namespace(std::size_t Index) const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  if (!Fields || Index >= Fields->Namespaces.size())
    return std::string_view();
  return Fields->Namespaces[Index];
}

std::size_t ModuleRecord::TypeCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  return Fields ? Fields->Types.size() : 0;
}

std::string_view ModuleRecord::TypeName(std::size_t Index) const noexcept {
  const auto *Fields = Resolve(StorageValue)->ModuleAt(ModuleIndexValue);
  if (!Fields || Index >= Fields->Types.size())
    return std::string_view();
  return Fields->Types[Index];
}

TypeRecord::TypeRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                       std::size_t TypeIndex) noexcept
    : StorageValue(std::move(Storage)), TypeIndexValue(TypeIndex) {}

bool TypeRecord::IsValid() const noexcept {
  return StorageValue && StorageValue->TypeAt(TypeIndexValue);
}

TypeId TypeRecord::Id() const noexcept {
  const auto *Fields = Resolve(StorageValue)->TypeAt(TypeIndexValue);
  return Fields ? Fields->Id : TypeId();
}

std::string_view TypeRecord::Name() const noexcept {
  const auto *Fields = Resolve(StorageValue)->TypeAt(TypeIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

TypeKind TypeRecord::Kind() const noexcept {
  const auto *Fields = Resolve(StorageValue)->TypeAt(TypeIndexValue);
  return Fields ? Fields->Descriptor.Kind() : TypeKind::Unsupported;
}

TypeDescriptor TypeRecord::Descriptor() const {
  const auto *Fields = Resolve(StorageValue)->TypeAt(TypeIndexValue);
  return Fields ? Fields->Descriptor : TypeDescriptor::Unsupported();
}

SymbolId TypeRecord::Declaration() const noexcept {
  const auto *Fields = Resolve(StorageValue)->TypeAt(TypeIndexValue);
  return Fields ? Fields->Declaration : SymbolId();
}

ReflectionRecord::ReflectionRecord(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    std::size_t RecordIndex) noexcept
    : StorageValue(std::move(Storage)), RecordIndexValue(RecordIndex) {}

bool ReflectionRecord::IsValid() const noexcept {
  return StorageValue && StorageValue->RecordAt(RecordIndexValue);
}

SymbolKind ReflectionRecord::Kind() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Kind : SymbolKind::Namespace;
}

SymbolId ReflectionRecord::Id() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Id : SymbolId();
}

std::string_view ReflectionRecord::Name() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->Name) : std::string_view();
}

std::string_view ReflectionRecord::QualifiedName() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->QualifiedName) : std::string_view();
}

std::string_view ReflectionRecord::Signature() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->Signature) : std::string_view();
}

ScopeId ReflectionRecord::Scope() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Scope : ScopeId::Root();
}

SymbolId ReflectionRecord::Declaration() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  if (!Fields)
    return SymbolId();
  return Fields->Declaration.IsValid() ? Fields->Declaration : Fields->Id;
}

SymbolId ReflectionRecord::OverloadSet() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->OverloadSet : SymbolId();
}

TypeId ReflectionRecord::Type() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Type : TypeId();
}

TypeDescriptor ReflectionRecord::Descriptor() const {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Descriptor : TypeDescriptor::Unsupported();
}

ReturnShape ReflectionRecord::Returns() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Returns : ReturnShape::Zero;
}

bool ReflectionRecord::IsAsynchronous() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields != nullptr && Fields->ReturnsAsynchronously;
}

bool ReflectionRecord::HasValue() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields != nullptr && Fields->ValueIsAvailable;
}

std::string_view ReflectionRecord::ValueText() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->ValueText) : std::string_view();
}

std::string_view ReflectionRecord::OwnershipResult() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->OwnershipResult)
                : std::string_view();
}

std::string_view ReflectionRecord::AllocatorPolicy() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->AllocatorPolicy)
                : std::string_view();
}

TypeId ReflectionRecord::Receiver() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->ReceiverType : TypeId();
}

bool ReflectionRecord::ReceiverPermitsConst() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields != nullptr && Fields->ReceiverPermitsConst;
}

bool ReflectionRecord::IsReadable() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields != nullptr && Fields->MemberIsReadable;
}

bool ReflectionRecord::IsWritable() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields != nullptr && Fields->MemberIsWritable;
}

std::string_view ReflectionRecord::AccessPolicy() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->MemberAccessText)
                : std::string_view();
}

std::string_view ReflectionRecord::Evaluation() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->MemberEvaluationText)
                : std::string_view();
}

std::string_view ReflectionRecord::MemberOwnershipPolicy() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->MemberOwnershipText)
                : std::string_view();
}

std::string_view ReflectionRecord::Documentation() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? std::string_view(Fields->Documentation) : std::string_view();
}

std::size_t ReflectionRecord::ExampleCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Examples.size() : 0;
}

std::string_view ReflectionRecord::Example(std::size_t Index) const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  if (!Fields || Index >= Fields->Examples.size())
    return std::string_view();
  return Fields->Examples[Index];
}

std::size_t ReflectionRecord::ParameterCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Parameters.size() : 0;
}

ParameterRecord ReflectionRecord::Parameter(std::size_t Index) const {
  if (!StorageValue || !StorageValue->ParameterAt(RecordIndexValue, Index))
    return ParameterRecord();
  return Detail::ReflectionStorage::MakeParameter(StorageValue,
                                                  RecordIndexValue, Index);
}

std::size_t ReflectionRecord::ReturnCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->ReturnValues.size() : 0;
}

ReturnRecord ReflectionRecord::Return(std::size_t Index) const {
  if (!StorageValue || !StorageValue->ReturnAt(RecordIndexValue, Index))
    return ReturnRecord();
  return Detail::ReflectionStorage::MakeReturn(StorageValue, RecordIndexValue,
                                               Index);
}

std::size_t ReflectionRecord::AttributeCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Attributes.size() : 0;
}

AttributeRecord ReflectionRecord::Attribute(std::size_t Index) const {
  if (!StorageValue || !StorageValue->AttributeAt(RecordIndexValue, Index))
    return AttributeRecord();
  return Detail::ReflectionStorage::MakeAttribute(StorageValue,
                                                  RecordIndexValue, Index);
}

std::size_t ReflectionRecord::RelationCount() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields ? Fields->Relations.size() : 0;
}

TypeRelation ReflectionRecord::Relation(std::size_t Index) const {
  if (!StorageValue || !StorageValue->RelationAt(RecordIndexValue, Index))
    return TypeRelation();
  return Detail::ReflectionStorage::MakeRelation(StorageValue, RecordIndexValue,
                                                 Index);
}

bool ReflectionRecord::HasModule() const noexcept {
  const auto *Fields = Resolve(StorageValue)->RecordAt(RecordIndexValue);
  return Fields && Fields->Module.has_value();
}

ModuleRecord ReflectionRecord::Module() const {
  if (!StorageValue)
    return ModuleRecord();
  const auto *Fields = StorageValue->RecordAt(RecordIndexValue);
  if (!Fields || !Fields->Module)
    return ModuleRecord();
  return Detail::ReflectionStorage::MakeModule(StorageValue, *Fields->Module);
}

ReflectionRecordRange::ReflectionRecordRange(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    const std::size_t *Indices, std::size_t Count) noexcept
    : StorageValue(std::move(Storage)), IndicesValue(Indices),
      CountValue(Count) {}

ReflectionRecord ReflectionRecordRange::At(std::size_t Index) const {
  if (Index >= CountValue || !IndicesValue)
    return ReflectionRecord();
  return Detail::ReflectionStorage::MakeRecord(StorageValue,
                                               IndicesValue[Index]);
}

TypeRecordRange::TypeRecordRange(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    const std::size_t *Indices, std::size_t Count) noexcept
    : StorageValue(std::move(Storage)), IndicesValue(Indices),
      CountValue(Count) {}

TypeRecord TypeRecordRange::At(std::size_t Index) const {
  if (Index >= CountValue || !IndicesValue)
    return TypeRecord();
  return Detail::ReflectionStorage::MakeType(StorageValue, IndicesValue[Index]);
}

ModuleRecordRange::ModuleRecordRange(
    std::shared_ptr<const Detail::ReflectionStorage> Storage,
    const std::size_t *Indices, std::size_t Count) noexcept
    : StorageValue(std::move(Storage)), IndicesValue(Indices),
      CountValue(Count) {}

ModuleRecord ModuleRecordRange::At(std::size_t Index) const {
  if (Index >= CountValue || !IndicesValue)
    return ModuleRecord();
  return Detail::ReflectionStorage::MakeModule(StorageValue,
                                               IndicesValue[Index]);
}

ReflectionSnapshot::ReflectionSnapshot(
    std::shared_ptr<const Detail::ReflectionStorage> Storage) noexcept
    : StorageValue(std::move(Storage)) {}

std::uint64_t ReflectionSnapshot::Generation() const noexcept {
  return Resolve(StorageValue)->Generation();
}

std::size_t ReflectionSnapshot::Size() const noexcept {
  return Resolve(StorageValue)->RecordCount();
}

bool ReflectionSnapshot::IsEmpty() const noexcept { return Size() == 0; }

ReflectionRecord ReflectionSnapshot::Find(SymbolId Id) const {
  if (!StorageValue)
    return ReflectionRecord();
  const auto Index = StorageValue->IndexOf(Id);
  if (!Index)
    return ReflectionRecord();
  return Detail::ReflectionStorage::MakeRecord(StorageValue, *Index);
}

ReflectionRecord
ReflectionSnapshot::Find(std::string_view QualifiedName) const {
  if (!StorageValue)
    return ReflectionRecord();
  const auto Index = StorageValue->IndexOf(QualifiedName);
  if (!Index)
    return ReflectionRecord();
  return Detail::ReflectionStorage::MakeRecord(StorageValue, *Index);
}

ReflectionRecordRange ReflectionSnapshot::Symbols() const {
  if (!StorageValue)
    return ReflectionRecordRange();
  return Detail::ReflectionStorage::MakeRecordRange(StorageValue,
                                                    StorageValue->AllOrder());
}

ReflectionRecordRange ReflectionSnapshot::Symbols(ScopeId Scope) const {
  if (!StorageValue)
    return ReflectionRecordRange();
  return Detail::ReflectionStorage::MakeRecordRange(
      StorageValue, StorageValue->OrderOfScope(Scope));
}

ReflectionRecordRange ReflectionSnapshot::Symbols(SymbolKind Kind) const {
  if (!StorageValue)
    return ReflectionRecordRange();
  return Detail::ReflectionStorage::MakeRecordRange(
      StorageValue, StorageValue->OrderOfKind(Kind));
}

TypeRecordRange ReflectionSnapshot::Types() const {
  if (!StorageValue)
    return TypeRecordRange();
  return Detail::ReflectionStorage::MakeTypeRange(StorageValue,
                                                  StorageValue->TypeOrder());
}

ModuleRecordRange ReflectionSnapshot::Modules() const {
  if (!StorageValue)
    return ModuleRecordRange();
  return Detail::ReflectionStorage::MakeModuleRange(
      StorageValue, StorageValue->ModuleOrder());
}

TypeRecord ReflectionSnapshot::FindType(TypeId Id) const {
  if (!StorageValue)
    return TypeRecord();
  const auto Index = StorageValue->TypeIndexOf(Id);
  if (!Index)
    return TypeRecord();
  return Detail::ReflectionStorage::MakeType(StorageValue, *Index);
}

} // namespace Luna
