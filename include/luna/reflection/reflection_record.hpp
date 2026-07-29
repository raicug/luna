#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include <compare>
#include <cstddef>
#include <memory>
#include <string_view>
// clang-format on

namespace Luna {

namespace Detail {
class ReflectionStorage;
}

enum class ParameterDisposition { Required, Optional, Defaulted, Variadic };

enum class ReturnShape { Zero, Scalar, Multiple };

enum class TypeRelationKind {
  Declared,
  Base,
  Cast,
  Operand,
  Element,
  Inherited
};

[[nodiscard]] constexpr std::string_view
ParameterDispositionText(ParameterDisposition Disposition) noexcept {
  switch (Disposition) {
  case ParameterDisposition::Required:
    return "required";
  case ParameterDisposition::Optional:
    return "optional";
  case ParameterDisposition::Defaulted:
    return "defaulted";
  case ParameterDisposition::Variadic:
    return "variadic";
  }
  return "required";
}

[[nodiscard]] constexpr std::string_view
ReturnShapeText(ReturnShape Shape) noexcept {
  switch (Shape) {
  case ReturnShape::Zero:
    return "zero";
  case ReturnShape::Scalar:
    return "scalar";
  case ReturnShape::Multiple:
    return "multiple";
  }
  return "zero";
}

[[nodiscard]] constexpr std::string_view
TypeRelationKindText(TypeRelationKind Kind) noexcept {
  switch (Kind) {
  case TypeRelationKind::Declared:
    return "declared";
  case TypeRelationKind::Base:
    return "base";
  case TypeRelationKind::Cast:
    return "cast";
  case TypeRelationKind::Operand:
    return "operand";
  case TypeRelationKind::Element:
    return "element";
  case TypeRelationKind::Inherited:
    return "inherited";
  }
  return "declared";
}

class ScopeId {
public:
  constexpr ScopeId() noexcept = default;

  explicit constexpr ScopeId(SymbolId Owner) noexcept : OwnerValue(Owner) {}

  [[nodiscard]] static constexpr ScopeId Root() noexcept { return ScopeId(); }

  [[nodiscard]] constexpr const SymbolId &Owner() const noexcept {
    return OwnerValue;
  }

  [[nodiscard]] constexpr bool IsRoot() const noexcept {
    return !OwnerValue.IsValid();
  }

  [[nodiscard]] std::size_t Hash() const noexcept { return OwnerValue.Hash(); }

  [[nodiscard]] friend constexpr bool
  operator==(const ScopeId &Left, const ScopeId &Right) noexcept {
    return Left.OwnerValue == Right.OwnerValue;
  }

  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(const ScopeId &Left, const ScopeId &Right) noexcept {
    return Left.OwnerValue <=> Right.OwnerValue;
  }

private:
  SymbolId OwnerValue;
};

class ParameterRecord final {
public:
  ParameterRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] TypeId Type() const noexcept;
  [[nodiscard]] TypeDescriptor Descriptor() const;
  [[nodiscard]] ParameterDisposition Disposition() const noexcept;
  [[nodiscard]] bool HasDefault() const noexcept;
  [[nodiscard]] std::string_view DefaultText() const noexcept;
  [[nodiscard]] std::string_view Documentation() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  ParameterRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                  std::size_t RecordIndex, std::size_t ParameterIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t RecordIndexValue = 0;
  std::size_t ParameterIndexValue = 0;
};

class ReturnRecord final {
public:
  ReturnRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] TypeId Type() const noexcept;
  [[nodiscard]] TypeDescriptor Descriptor() const;

private:
  friend class Detail::ReflectionStorage;

  ReturnRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
               std::size_t RecordIndex, std::size_t ReturnIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t RecordIndexValue = 0;
  std::size_t ReturnIndexValue = 0;
};

class AttributeRecord final {
public:
  AttributeRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] std::string_view Value() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  AttributeRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                  std::size_t RecordIndex, std::size_t AttributeIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t RecordIndexValue = 0;
  std::size_t AttributeIndexValue = 0;
};

class TypeRelation final {
public:
  TypeRelation() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] TypeRelationKind Kind() const noexcept;
  [[nodiscard]] TypeId Type() const noexcept;
  [[nodiscard]] SymbolId Declaration() const noexcept;
  [[nodiscard]] std::string_view Note() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  TypeRelation(std::shared_ptr<const Detail::ReflectionStorage> Storage,
               std::size_t RecordIndex, std::size_t RelationIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t RecordIndexValue = 0;
  std::size_t RelationIndexValue = 0;
};

class ModuleDependencyRecord final {
public:
  ModuleDependencyRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Identity() const noexcept;
  [[nodiscard]] std::string_view Version() const noexcept;
  [[nodiscard]] std::string_view Constraints() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  ModuleDependencyRecord(
      std::shared_ptr<const Detail::ReflectionStorage> Storage,
      std::size_t ModuleIndex, std::size_t DependencyIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t ModuleIndexValue = 0;
  std::size_t DependencyIndexValue = 0;
};

class ModuleExportRecord final {
public:
  ModuleExportRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] SymbolKind Kind() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] std::string_view Documentation() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  ModuleExportRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                     std::size_t ModuleIndex, std::size_t ExportIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t ModuleIndexValue = 0;
  std::size_t ExportIndexValue = 0;
};

class ModuleRecord final {
public:
  ModuleRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Identity() const noexcept;
  [[nodiscard]] std::string_view Version() const noexcept;
  [[nodiscard]] SymbolId Symbol() const noexcept;
  [[nodiscard]] std::string_view Documentation() const noexcept;

  [[nodiscard]] std::size_t DependencyCount() const noexcept;
  [[nodiscard]] ModuleDependencyRecord Dependency(std::size_t Index) const;
  [[nodiscard]] std::size_t ExportCount() const noexcept;
  [[nodiscard]] ModuleExportRecord Export(std::size_t Index) const;

  [[nodiscard]] std::size_t NamespaceCount() const noexcept;
  [[nodiscard]] std::string_view Namespace(std::size_t Index) const noexcept;
  [[nodiscard]] std::size_t TypeCount() const noexcept;
  [[nodiscard]] std::string_view TypeName(std::size_t Index) const noexcept;

private:
  friend class Detail::ReflectionStorage;

  ModuleRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
               std::size_t ModuleIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t ModuleIndexValue = 0;
};

class TypeRecord final {
public:
  TypeRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] TypeId Id() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] TypeKind Kind() const noexcept;
  [[nodiscard]] TypeDescriptor Descriptor() const;
  [[nodiscard]] SymbolId Declaration() const noexcept;

private:
  friend class Detail::ReflectionStorage;

  TypeRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
             std::size_t TypeIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t TypeIndexValue = 0;
};

class ReflectionRecord final {
public:
  ReflectionRecord() noexcept = default;

  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] SymbolKind Kind() const noexcept;
  [[nodiscard]] SymbolId Id() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  [[nodiscard]] std::string_view QualifiedName() const noexcept;
  [[nodiscard]] std::string_view Signature() const noexcept;
  [[nodiscard]] ScopeId Scope() const noexcept;
  [[nodiscard]] SymbolId Declaration() const noexcept;
  [[nodiscard]] SymbolId OverloadSet() const noexcept;
  [[nodiscard]] TypeId Type() const noexcept;
  [[nodiscard]] TypeDescriptor Descriptor() const;
  [[nodiscard]] ReturnShape Returns() const noexcept;
  [[nodiscard]] bool IsAsynchronous() const noexcept;

  [[nodiscard]] bool HasValue() const noexcept;
  [[nodiscard]] std::string_view ValueText() const noexcept;

  [[nodiscard]] std::string_view OwnershipResult() const noexcept;
  [[nodiscard]] std::string_view AllocatorPolicy() const noexcept;

  [[nodiscard]] TypeId Receiver() const noexcept;
  [[nodiscard]] bool ReceiverPermitsConst() const noexcept;
  [[nodiscard]] bool IsReadable() const noexcept;
  [[nodiscard]] bool IsWritable() const noexcept;
  [[nodiscard]] std::string_view AccessPolicy() const noexcept;
  [[nodiscard]] std::string_view Evaluation() const noexcept;
  [[nodiscard]] std::string_view MemberOwnershipPolicy() const noexcept;

  [[nodiscard]] std::string_view Documentation() const noexcept;

  [[nodiscard]] std::size_t ExampleCount() const noexcept;
  [[nodiscard]] std::string_view Example(std::size_t Index) const noexcept;
  [[nodiscard]] std::size_t ParameterCount() const noexcept;
  [[nodiscard]] ParameterRecord Parameter(std::size_t Index) const;
  [[nodiscard]] std::size_t ReturnCount() const noexcept;
  [[nodiscard]] ReturnRecord Return(std::size_t Index) const;
  [[nodiscard]] std::size_t AttributeCount() const noexcept;
  [[nodiscard]] AttributeRecord Attribute(std::size_t Index) const;
  [[nodiscard]] std::size_t RelationCount() const noexcept;
  [[nodiscard]] TypeRelation Relation(std::size_t Index) const;

  [[nodiscard]] bool HasModule() const noexcept;
  [[nodiscard]] ModuleRecord Module() const;

private:
  friend class Detail::ReflectionStorage;

  ReflectionRecord(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                   std::size_t RecordIndex) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  std::size_t RecordIndexValue = 0;
};

} // namespace Luna
