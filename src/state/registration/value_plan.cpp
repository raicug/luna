// clang-format off
#include "state/registration/value_plan.hpp"

#include <luna/binding/constant_value.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_record.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr std::int64_t CanonicalIntegerLowest =
    static_cast<std::int64_t>(std::numeric_limits<int>::min());
constexpr std::int64_t CanonicalIntegerHighest =
    static_cast<std::int64_t>(std::numeric_limits<int>::max());

[[nodiscard]] std::string
EnumerationSubject(const StagedEnumeration &Declaration) {
  return SubjectText(SymbolKindText(SymbolKind::Enumeration),
                     Declaration.QualifiedName);
}

[[nodiscard]] std::string
EnumeratorSubject(const StagedEnumeration &Declaration,
                  const StagedEnumerator &Enumerator) {
  return SubjectText(
      SymbolKindText(Enumerator.IsAlias ? SymbolKind::EnumeratorAlias
                                        : SymbolKind::Enumerator),
      JoinQualifiedName(Declaration.QualifiedName, Enumerator.Segment));
}

[[nodiscard]] TypeDescriptor
EnumerationTypeOf(const StagedEnumeration &Declaration) {
  return TypeDescriptor::ForEnumeration(Declaration.Key);
}

[[nodiscard]] SymbolDescriptor
MakeEnumerationSymbol(const StagedEnumeration &Declaration, SymbolId Parent) {
  return MakeClassMemberSymbol(SymbolKind::Enumeration,
                               Declaration.QualifiedName, Parent,
                               EnumerationTypeOf(Declaration));
}

[[nodiscard]] SymbolId IdentityOf(const SymbolDescriptor &Symbol) {
  if (const auto Identity = SymbolIdentityRegistry::ComputeIdentity(Symbol))
    return *Identity;
  return SymbolId();
}

} // namespace

std::string CanonicalValueText(const Value &Staged) {
  if (const bool *Boolean = std::get_if<bool>(&Staged))
    return *Boolean ? "true" : "false";
  if (const int *Integer = std::get_if<int>(&Staged))
    return std::to_string(*Integer);
  if (const double *Number = std::get_if<double>(&Staged)) {
    std::ostringstream Stream;
    Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << *Number;
    return Stream.str();
  }
  if (const std::string *Text = std::get_if<std::string>(&Staged))
    return *Text;
  return std::string();
}

DescriptorPlanEntry MakeConstantPlanEntry(const StagedConstant &Declaration,
                                          SymbolId Parent, const TypeId &Type) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Value;

  Entry.VmPath = Declaration.QualifiedName;

  SymbolDescriptor Symbol =
      MakeScopeSymbol(SymbolKind::Constant, Declaration.QualifiedName, Parent);
  Symbol.AssociatedType = Declaration.Request.Type;
  Entry.Symbol = std::move(Symbol);
  Entry.Identity = IdentityOf(Entry.Symbol);

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Constant;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(Declaration.QualifiedName));
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Scope = Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root();
  Record.Declaration = Entry.Identity;
  Record.Type = Type;
  Record.Descriptor = Declaration.Request.Type;
  Record.Returns = ReturnShape::Zero;
  Record.ValueIsAvailable = true;
  Record.ValueText = CanonicalValueText(Declaration.Request.Constant);
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  PlannedValue Installed;
  Installed.Type = Declaration.Request.Type;
  Installed.Staged = StructuredValue::Scalar(Declaration.Request.Constant);
  Entry.InstalledValue = std::move(Installed);
  return Entry;
}

EnumerationDomain MakeEnumerationDomain(const StagedEnumeration &Declaration) {
  EnumerationDomain Domain;
  Domain.IsBitflags = Declaration.IsBitflags;
  Domain.PublishesObjects = Declaration.PublishesObjects;

  std::vector<std::pair<std::int64_t, std::string>> Named;
  for (const StagedEnumerator &Enumerator : Declaration.Enumerators) {
    if (Enumerator.IsAlias)
      continue;
    Named.emplace_back(Enumerator.Numeric, Enumerator.Segment);
    Domain.SupportedBits |= Enumerator.Numeric;
  }
  std::sort(Named.begin(), Named.end(),
            [](const std::pair<std::int64_t, std::string> &Left,
               const std::pair<std::int64_t, std::string> &Right) {
              return Left.first < Right.first;
            });
  for (const auto &[Numeric, Name] : Named) {
    if (!Domain.Values.empty() && Domain.Values.back() == Numeric)
      continue;
    Domain.Values.push_back(Numeric);
    Domain.Names.push_back(Name);
  }
  if (!Declaration.PublishesObjects)
    Domain.Names.clear();

  if (Declaration.HasDeclaredMask)
    Domain.SupportedBits = Declaration.SupportedBits;
  if (!Declaration.IsBitflags)
    Domain.SupportedBits = 0;
  return Domain;
}

DescriptorPlanEntry
MakeEnumerationPlanEntry(const StagedEnumeration &Declaration,
                         SymbolId Parent) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::Scope;
  Entry.VmPath = Declaration.QualifiedName;
  Entry.Symbol = MakeEnumerationSymbol(Declaration, Parent);
  Entry.Identity = IdentityOf(Entry.Symbol);

  const TypeDescriptor Type = EnumerationTypeOf(Declaration);
  TypeRecord Declared =
      DeclareEnumerationTypeRecord(Declaration.Key, Declaration.QualifiedName,
                                   MakeEnumerationDomain(Declaration));

  ReflectionTypeFields TypeFields;
  TypeFields.Id = Declared.Identity;
  TypeFields.Name = Declaration.QualifiedName;
  TypeFields.Descriptor = Type;
  TypeFields.Declaration = Entry.Identity;
  Entry.TypeFields = std::move(TypeFields);
  Entry.TypeConversion = std::move(Declared);

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Enumeration;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(Declaration.QualifiedName));
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Scope = Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root();
  Record.Declaration = Entry.Identity;
  Record.Type = Entry.TypeFields->Id;
  Record.Descriptor = Type;
  Record.Returns = ReturnShape::Zero;
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;

  if (Declaration.IsBitflags) {
    ReflectionAttributeFields Flags;
    Flags.Name = "Bitflags";
    Flags.Value = std::to_string(static_cast<long long>(
        MakeEnumerationDomain(Declaration).SupportedBits));
    Record.Attributes.push_back(std::move(Flags));
  }
  Entry.Record = std::move(Record);

  PlannedValueTable Table;
  Table.Type = Type;
  for (const StagedEnumerator &Enumerator : Declaration.Enumerators) {
    const StagedEnumerator *Canonical =
        Enumerator.IsAlias
            ? FindCanonicalEnumerator(Declaration, Enumerator.CanonicalSegment)
            : &Enumerator;
    if (!Canonical)
      continue;
    PlannedValueField Field;
    Field.Name = Enumerator.Segment;
    Field.Staged =
        StructuredValue::Scalar(Value(static_cast<int>(Canonical->Numeric)));
    Table.Fields.push_back(std::move(Field));
  }
  Entry.InstalledTable = std::move(Table);
  return Entry;
}

DescriptorPlanEntry
MakeEnumeratorPlanEntry(const StagedEnumeration &Declaration,
                        const StagedEnumerator &Enumerator,
                        const SymbolId &Enumeration, const TypeId &Type,
                        const SymbolId &CanonicalEnumerator) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::ReflectionRecord;

  const std::string QualifiedName =
      JoinQualifiedName(Declaration.QualifiedName, Enumerator.Segment);
  Entry.VmPath = QualifiedName;

  const TypeDescriptor Enumerated = EnumerationTypeOf(Declaration);
  Entry.Symbol =
      Enumerator.IsAlias
          ? MakeEnumeratorAliasSymbol(
                QualifiedName, Enumeration, Enumerated,
                JoinQualifiedName(Declaration.QualifiedName,
                                  Enumerator.CanonicalSegment))
          : MakeClassMemberSymbol(SymbolKind::Enumerator, QualifiedName,
                                  Enumeration, Enumerated);
  Entry.Identity = IdentityOf(Entry.Symbol);

  const StagedEnumerator *Canonical =
      Enumerator.IsAlias
          ? FindCanonicalEnumerator(Declaration, Enumerator.CanonicalSegment)
          : &Enumerator;

  ReflectionRecordFields Record;
  Record.Kind =
      Enumerator.IsAlias ? SymbolKind::EnumeratorAlias : SymbolKind::Enumerator;
  Record.Id = Entry.Identity;
  Record.Name = Enumerator.Segment;
  Record.QualifiedName = QualifiedName;
  Record.Scope = ScopeId(Enumeration);

  Record.Declaration = Enumerator.IsAlias && CanonicalEnumerator.IsValid()
                           ? CanonicalEnumerator
                           : Entry.Identity;
  Record.Type = Type;
  Record.Descriptor = Enumerated;
  Record.Returns = ReturnShape::Zero;
  Record.ValueIsAvailable = true;
  Record.ValueText =
      std::to_string(Canonical ? Canonical->Numeric : Enumerator.Numeric);
  Record.Documentation = Enumerator.Documentation;
  Record.Attributes = Enumerator.Attributes;
  Record.Examples = Enumerator.Examples;
  Entry.Record = std::move(Record);
  return Entry;
}

const StagedEnumerator *
FindCanonicalEnumerator(const StagedEnumeration &Declaration,
                        std::string_view Segment) {
  for (const StagedEnumerator &Enumerator : Declaration.Enumerators) {
    if (!Enumerator.IsAlias && Enumerator.Segment == Segment)
      return &Enumerator;
  }
  return nullptr;
}

std::optional<ErrorDiagnostic>
ValidateStagedEnumeration(const StagedEnumeration &Declaration) {
  const std::string Subject = EnumerationSubject(Declaration);

  if (!Declaration.Policy.IsScoped && !Declaration.UnscopedIsAllowed)
    return MissingOptInDiagnostic(
        Subject, "the enumeration is unscoped; exposing an unscoped "
                 "enumeration requires the explicit unscoped opt-in.");

  bool HasCanonicalValue = false;
  for (const StagedEnumerator &Enumerator : Declaration.Enumerators) {
    if (!Enumerator.IsAlias) {
      HasCanonicalValue = true;
      break;
    }
  }
  if (!HasCanonicalValue)
    return MalformedMetadataDiagnostic(
        Subject, "the enumeration declares no canonical enumerator.");

  for (std::size_t Index = 0; Index < Declaration.Enumerators.size(); ++Index) {
    const StagedEnumerator &Enumerator = Declaration.Enumerators[Index];
    const std::string Member = EnumeratorSubject(Declaration, Enumerator);

    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      if (Declaration.Enumerators[Earlier].Segment == Enumerator.Segment)
        return DuplicateNameDiagnostic(Member);
    }

    if (Enumerator.IsAlias) {
      if (!FindCanonicalEnumerator(Declaration, Enumerator.CanonicalSegment))
        return UnknownAliasTargetDiagnostic(
            Member, JoinQualifiedName(Declaration.QualifiedName,
                                      Enumerator.CanonicalSegment));
      continue;
    }

    if (Enumerator.Numeric < Declaration.Policy.Minimum ||
        Enumerator.Numeric > Declaration.Policy.Maximum)
      return ValueOutOfRangeDiagnostic(
          Member, "the declared C++ underlying type",
          static_cast<long long>(Enumerator.Numeric),
          static_cast<long long>(Declaration.Policy.Minimum),
          static_cast<long long>(Declaration.Policy.Maximum));

    if (Enumerator.Numeric < CanonicalIntegerLowest ||
        Enumerator.Numeric > CanonicalIntegerHighest)
      return ValueOutOfRangeDiagnostic(
          Member,
          "the exact-integer domain Luna converts an enumeration "
          "through",
          static_cast<long long>(Enumerator.Numeric),
          static_cast<long long>(CanonicalIntegerLowest),
          static_cast<long long>(CanonicalIntegerHighest));

    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      const StagedEnumerator &Other = Declaration.Enumerators[Earlier];
      if (Other.IsAlias || Other.Numeric != Enumerator.Numeric)
        continue;
      return DuplicateEnumeratorValueDiagnostic(
          Member, Other.Segment, static_cast<long long>(Enumerator.Numeric));
    }

    if (Declaration.IsBitflags && Declaration.HasDeclaredMask &&
        (Enumerator.Numeric & ~Declaration.SupportedBits) != 0)
      return UnsupportedFlagBitsDiagnostic(
          Member, static_cast<long long>(Enumerator.Numeric),
          static_cast<long long>(Declaration.SupportedBits));
  }

  if (Declaration.IsBitflags && Declaration.HasDeclaredMask &&
      (Declaration.SupportedBits < CanonicalIntegerLowest ||
       Declaration.SupportedBits > CanonicalIntegerHighest))
    return ValueOutOfRangeDiagnostic(
        Subject,
        "the exact-integer domain Luna converts an enumeration through",
        static_cast<long long>(Declaration.SupportedBits),
        static_cast<long long>(CanonicalIntegerLowest),
        static_cast<long long>(CanonicalIntegerHighest));

  return std::nullopt;
}

} // namespace Luna::Detail
