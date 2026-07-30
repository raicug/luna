// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::ReflectionDatabase;
using Luna::Detail::ReflectionGenerationBuilder;
using Luna::Detail::ReflectionGenerationStatus;
using Luna::Detail::ReflectionModuleFields;
using Luna::Detail::ReflectionParameterFields;
using Luna::Detail::ReflectionRecordFields;
using Luna::Detail::ReflectionReturnFields;
using Luna::Detail::ReflectionTypeFields;

constexpr std::size_t SymbolKindCount =
    static_cast<std::size_t>(Luna::SymbolKind::Type) + 1;

constexpr std::array<std::string_view, 4> ModuleIdentityTexts{
    "studio.physics", "engine.core", "app.tools", "luna.math"};

constexpr std::array<std::string_view, 3> ModuleVersionTexts{"1.0.0", "1.2.0",
                                                             "2.0.0-beta.1"};

constexpr std::array<std::string_view, 4> ModuleRecordNames{
    "Physics", "Core", "ToolBox", "MathKit"};

constexpr std::array<std::string_view, 4> NamespaceNames{"Studio", "Engine",
                                                         "App", "Widgets"};

constexpr std::array<std::string_view, 3> MemberNames{"Find", "Apply", "Reset"};

constexpr std::array<std::string_view, 2> SignatureTexts{"number(string)",
                                                         "void(string,number)"};

constexpr std::array<std::string_view, 3> ConstantNames{"Gravity", "Limit",
                                                        "Epsilon"};

constexpr std::array<std::string_view, 3> TypeNames{"number", "string",
                                                    "boolean"};

class ByteCursor final {
public:
  explicit ByteCursor(std::span<const std::uint8_t> Bytes) noexcept
      : BytesValue(Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue.empty())
      return static_cast<std::uint8_t>(Index * 29U + 7U);
    return BytesValue[Index % BytesValue.size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  std::span<const std::uint8_t> BytesValue;
  std::size_t IndexValue = 0;
};

[[nodiscard]] Luna::SymbolId Symbol(std::size_t Value) {
  Luna::SymbolId::Storage Bytes{};
  Bytes[Luna::SymbolId::ByteCount - 1] =
      static_cast<std::uint8_t>(Value & 0xffU);
  Bytes[Luna::SymbolId::ByteCount - 2] =
      static_cast<std::uint8_t>((Value >> 8) & 0xffU);
  return Luna::SymbolId::FromBytes(Bytes);
}

[[nodiscard]] Luna::TypeId TypeOf(std::size_t Value) {
  Luna::TypeId::Storage Bytes{};
  Bytes[Luna::TypeId::ByteCount - 1] = static_cast<std::uint8_t>(Value & 0xffU);
  Bytes[Luna::TypeId::ByteCount - 2] =
      static_cast<std::uint8_t>((Value >> 8) & 0xffU);
  return Luna::TypeId::FromBytes(Bytes);
}

[[nodiscard]] Luna::TypeDescriptor NumberDescriptor() {
  return Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
}

[[nodiscard]] Luna::TypeDescriptor TextDescriptor() {
  return Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::String);
}

struct LogicalGeneration final {
  std::vector<ReflectionRecordFields> Records;
  std::vector<ReflectionTypeFields> Types;
  std::vector<ReflectionModuleFields> Modules;
};

[[nodiscard]] ReflectionParameterFields
MakeParameterFields(std::string Name, Luna::TypeId Type,
                    Luna::TypeDescriptor Descriptor,
                    Luna::ParameterDisposition Disposition) {
  ReflectionParameterFields Parameter;
  Parameter.Name = std::move(Name);
  Parameter.Type = Type;
  Parameter.Descriptor = std::move(Descriptor);
  Parameter.Disposition = Disposition;
  if (Disposition == Luna::ParameterDisposition::Defaulted) {
    Parameter.HasDefault = true;
    Parameter.DefaultText = "8";
  }
  return Parameter;
}

[[nodiscard]] LogicalGeneration MakeGeneration(ByteCursor &Cursor) {
  LogicalGeneration Generation;
  std::size_t NextSymbol = 1;

  const std::size_t ModuleCount = 1U + Cursor.Pick(ModuleIdentityTexts.size());
  for (std::size_t Slot = 0; Slot < ModuleCount; ++Slot) {
    const Luna::SymbolId Owner = Symbol(NextSymbol++);
    ReflectionRecordFields Record;
    Record.Kind = Luna::SymbolKind::Module;
    Record.Id = Owner;
    Record.Name = std::string(ModuleRecordNames[Slot]);
    Record.QualifiedName = Record.Name;
    Record.Documentation = "Module symbol.";
    Generation.Records.push_back(std::move(Record));

    ReflectionModuleFields Module;
    Module.Identity = std::string(ModuleIdentityTexts[Slot]);
    Module.Version =
        std::string(ModuleVersionTexts[Cursor.Pick(ModuleVersionTexts.size())]);
    Module.Symbol = Owner;
    Module.Documentation = "Module provenance.";
    Generation.Modules.push_back(std::move(Module));
  }

  const std::size_t NamespaceCount = 1U + Cursor.Pick(NamespaceNames.size());
  for (std::size_t Slot = 0; Slot < NamespaceCount; ++Slot) {
    const Luna::SymbolId NamespaceIdentity = Symbol(NextSymbol++);
    const std::string NamespaceName(NamespaceNames[Slot]);
    ReflectionRecordFields Namespace;
    Namespace.Kind = Luna::SymbolKind::Namespace;
    Namespace.Id = NamespaceIdentity;
    Namespace.Name = NamespaceName;
    Namespace.QualifiedName = NamespaceName;
    Generation.Records.push_back(std::move(Namespace));

    const std::size_t OverloadSetCount = Cursor.Pick(MemberNames.size());
    for (std::size_t SetIndex = 0; SetIndex < OverloadSetCount; ++SetIndex) {
      const Luna::SymbolId SetIdentity = Symbol(NextSymbol++);
      const std::string MemberName(MemberNames[SetIndex]);
      const std::string SetQualifiedName = NamespaceName + "." + MemberName;

      ReflectionRecordFields Set;
      Set.Kind = Luna::SymbolKind::OverloadSet;
      Set.Id = SetIdentity;
      Set.Name = MemberName;
      Set.QualifiedName = SetQualifiedName;
      Set.Scope = Luna::ScopeId(NamespaceIdentity);
      Generation.Records.push_back(std::move(Set));

      const std::size_t CandidateCount =
          1U + Cursor.Pick(SignatureTexts.size());
      for (std::size_t Index = 0; Index < CandidateCount; ++Index) {
        ReflectionRecordFields Candidate;
        Candidate.Kind = Luna::SymbolKind::FunctionCandidate;
        Candidate.Id = Symbol(NextSymbol++);
        Candidate.Name = MemberName;
        Candidate.QualifiedName = SetQualifiedName;
        Candidate.Signature = std::string(SignatureTexts[Index]);
        Candidate.Scope = Luna::ScopeId(SetIdentity);
        Candidate.OverloadSet = SetIdentity;
        Candidate.Parameters.push_back(
            MakeParameterFields("Query", TypeOf(2), TextDescriptor(),
                                Luna::ParameterDisposition::Required));
        if (Index == 0) {
          Candidate.Returns = Luna::ReturnShape::Scalar;
          ReflectionReturnFields Result;
          Result.Name = "Result";
          Result.Type = TypeOf(1);
          Result.Descriptor = NumberDescriptor();
          Candidate.ReturnValues.push_back(std::move(Result));
        } else {
          Candidate.Returns = Luna::ReturnShape::Zero;
          Candidate.Parameters.push_back(
              MakeParameterFields("Limit", TypeOf(1), NumberDescriptor(),
                                  Luna::ParameterDisposition::Defaulted));
        }
        Generation.Records.push_back(std::move(Candidate));
      }
    }

    const std::size_t ConstantCount = Cursor.Pick(ConstantNames.size());
    for (std::size_t Index = 0; Index < ConstantCount; ++Index) {
      ReflectionRecordFields Constant;
      Constant.Kind = Luna::SymbolKind::Constant;
      Constant.Id = Symbol(NextSymbol++);
      Constant.Name = std::string(ConstantNames[Index]);
      Constant.QualifiedName = NamespaceName + "." + Constant.Name;
      Constant.Scope = Luna::ScopeId(NamespaceIdentity);
      Constant.Type = TypeOf(1);
      Constant.Descriptor = NumberDescriptor();
      Constant.Documentation = "Constant metadata.";
      Constant.Examples.push_back("print(" + Constant.QualifiedName + ")");
      Constant.Attributes.push_back({"Unit", "m/s^2"});
      if (Cursor.Pick(2) == 0)
        Constant.Module = Cursor.Pick(ModuleCount);
      Generation.Records.push_back(std::move(Constant));
    }
  }

  const std::size_t TypeCount = 1U + Cursor.Pick(4);
  for (std::size_t Index = 0; Index < TypeCount; ++Index) {
    ReflectionTypeFields Fields;
    Fields.Id = TypeOf(Index + 1);
    Fields.Name = std::string(TypeNames[Cursor.Pick(TypeNames.size())]);
    Fields.Descriptor = Index % 2 == 0 ? NumberDescriptor() : TextDescriptor();
    Generation.Types.push_back(std::move(Fields));
  }

  return Generation;
}

} // namespace

namespace {

[[nodiscard]] bool IdentityPrecedes(const Luna::SymbolId &Left,
                                    const Luna::SymbolId &Right) {
  for (std::size_t Index = 0; Index < Luna::SymbolId::ByteCount; ++Index) {
    if (Left.Bytes()[Index] != Right.Bytes()[Index])
      return Left.Bytes()[Index] < Right.Bytes()[Index];
  }
  return false;
}

[[nodiscard]] bool IdentityPrecedes(const Luna::TypeId &Left,
                                    const Luna::TypeId &Right) {
  for (std::size_t Index = 0; Index < Luna::TypeId::ByteCount; ++Index) {
    if (Left.Bytes()[Index] != Right.Bytes()[Index])
      return Left.Bytes()[Index] < Right.Bytes()[Index];
  }
  return false;
}

[[nodiscard]] bool ModelRecordPrecedes(const ReflectionRecordFields &Left,
                                       const ReflectionRecordFields &Right) {
  if (Left.QualifiedName != Right.QualifiedName)
    return Left.QualifiedName < Right.QualifiedName;
  if (Left.Kind != Right.Kind)
    return static_cast<int>(Left.Kind) < static_cast<int>(Right.Kind);
  if (Left.Signature != Right.Signature)
    return Left.Signature < Right.Signature;
  return IdentityPrecedes(Left.Id, Right.Id);
}

[[nodiscard]] bool ModelTypePrecedes(const ReflectionTypeFields &Left,
                                     const ReflectionTypeFields &Right) {
  if (Left.Name != Right.Name)
    return Left.Name < Right.Name;
  return IdentityPrecedes(Left.Id, Right.Id);
}

[[nodiscard]] bool ModelModulePrecedes(const ReflectionModuleFields &Left,
                                       const ReflectionModuleFields &Right) {
  if (Left.Identity != Right.Identity)
    return Left.Identity < Right.Identity;
  if (Left.Version != Right.Version)
    return Left.Version < Right.Version;
  return IdentityPrecedes(Left.Symbol, Right.Symbol);
}

void AppendField(std::string &Text, std::string_view Value) {
  Text.append(std::to_string(Value.size()));
  Text.push_back(':');
  Text.append(Value);
  Text.push_back('|');
}

[[nodiscard]] std::string
ModelRecordText(const ReflectionRecordFields &Fields,
                const std::vector<ReflectionModuleFields> &Modules) {
  std::string Text;
  AppendField(Text, Fields.Id.ToString());
  AppendField(Text, Fields.QualifiedName);
  AppendField(Text, Fields.Name);
  AppendField(Text, Luna::SymbolKindText(Fields.Kind));
  AppendField(Text, Fields.Signature);
  AppendField(Text, Fields.Scope.Owner().ToString());
  AppendField(Text, Fields.Declaration.IsValid() ? Fields.Declaration.ToString()
                                                 : Fields.Id.ToString());
  AppendField(Text, Fields.OverloadSet.ToString());
  AppendField(Text, Fields.Type.ToString());
  AppendField(Text, Luna::ReturnShapeText(Fields.Returns));
  for (const ReflectionParameterFields &Parameter : Fields.Parameters) {
    Text.append("parameter|");
    AppendField(Text, Parameter.Name);
    AppendField(Text, Parameter.Type.ToString());
    AppendField(Text, Luna::ParameterDispositionText(Parameter.Disposition));
    AppendField(Text, Parameter.DefaultText);
  }
  for (const ReflectionReturnFields &Return : Fields.ReturnValues) {
    Text.append("return|");
    AppendField(Text, Return.Name);
    AppendField(Text, Return.Type.ToString());
  }
  if (Fields.Module) {
    Text.append("module|");
    AppendField(Text, Modules[*Fields.Module].Identity);
    AppendField(Text, Modules[*Fields.Module].Version);
  } else {
    Text.append("no-module|");
  }
  return Text;
}

[[nodiscard]] std::string ModelTypeText(const ReflectionTypeFields &Fields) {
  std::string Text;
  AppendField(Text, Fields.Id.ToString());
  AppendField(Text, Fields.Name);
  return Text;
}

[[nodiscard]] std::string
ModelModuleText(const ReflectionModuleFields &Fields) {
  std::string Text;
  AppendField(Text, Fields.Identity);
  AppendField(Text, Fields.Version);
  AppendField(Text, Fields.Symbol.ToString());
  return Text;
}

struct CanonicalModel final {
  std::vector<std::string> Records;
  std::vector<std::string> Types;
  std::vector<std::string> Modules;
  std::array<std::vector<std::string>, SymbolKindCount> ByKind;
  std::vector<std::pair<Luna::ScopeId, std::vector<std::string>>> ByScope;
  std::vector<std::pair<Luna::SymbolId, std::string>> ById;
  std::vector<std::pair<std::string, std::string>> FirstByQualifiedName;
  std::vector<std::pair<Luna::TypeId, std::string>> TypeById;
};

[[nodiscard]] CanonicalModel MakeModel(const LogicalGeneration &Generation) {
  CanonicalModel Model;

  std::vector<ReflectionRecordFields> Records = Generation.Records;
  std::sort(Records.begin(), Records.end(), ModelRecordPrecedes);
  std::vector<ReflectionTypeFields> Types = Generation.Types;
  std::sort(Types.begin(), Types.end(), ModelTypePrecedes);
  std::vector<ReflectionModuleFields> Modules = Generation.Modules;
  std::sort(Modules.begin(), Modules.end(), ModelModulePrecedes);

  for (const ReflectionRecordFields &Fields : Records) {
    const std::string Text = ModelRecordText(Fields, Generation.Modules);
    Model.Records.push_back(Text);
    Model.ByKind[static_cast<std::size_t>(Fields.Kind)].push_back(Text);
    Model.ById.emplace_back(Fields.Id, Text);

    const auto Scope = std::find_if(
        Model.ByScope.begin(), Model.ByScope.end(),
        [&Fields](const auto &Entry) { return Entry.first == Fields.Scope; });
    if (Scope == Model.ByScope.end())
      Model.ByScope.emplace_back(Fields.Scope, std::vector<std::string>{Text});
    else
      Scope->second.push_back(Text);

    const auto Named = std::find_if(
        Model.FirstByQualifiedName.begin(), Model.FirstByQualifiedName.end(),
        [&Fields](const auto &Entry) {
          return Entry.first == Fields.QualifiedName;
        });
    if (Named == Model.FirstByQualifiedName.end())
      Model.FirstByQualifiedName.emplace_back(Fields.QualifiedName, Text);
  }
  for (const ReflectionTypeFields &Fields : Types) {
    Model.Types.push_back(ModelTypeText(Fields));
    Model.TypeById.emplace_back(Fields.Id, Fields.Name);
  }
  for (const ReflectionModuleFields &Fields : Modules)
    Model.Modules.push_back(ModelModuleText(Fields));
  return Model;
}

} // namespace

namespace {

[[nodiscard]] std::string RenderRecord(const Luna::ReflectionRecord &Record) {
  std::string Text;
  AppendField(Text, Record.Id().ToString());
  AppendField(Text, Record.QualifiedName());
  AppendField(Text, Record.Name());
  AppendField(Text, Luna::SymbolKindText(Record.Kind()));
  AppendField(Text, Record.Signature());
  AppendField(Text, Record.Scope().Owner().ToString());
  AppendField(Text, Record.Declaration().ToString());
  AppendField(Text, Record.OverloadSet().ToString());
  AppendField(Text, Record.Type().ToString());
  AppendField(Text, Luna::ReturnShapeText(Record.Returns()));
  for (std::size_t Index = 0; Index < Record.ParameterCount(); ++Index) {
    const Luna::ParameterRecord Parameter = Record.Parameter(Index);
    Text.append("parameter|");
    AppendField(Text, Parameter.Name());
    AppendField(Text, Parameter.Type().ToString());
    AppendField(Text, Luna::ParameterDispositionText(Parameter.Disposition()));
    AppendField(Text, Parameter.DefaultText());
  }
  for (std::size_t Index = 0; Index < Record.ReturnCount(); ++Index) {
    const Luna::ReturnRecord Return = Record.Return(Index);
    Text.append("return|");
    AppendField(Text, Return.Name());
    AppendField(Text, Return.Type().ToString());
  }
  if (Record.HasModule()) {
    Text.append("module|");
    AppendField(Text, Record.Module().Identity());
    AppendField(Text, Record.Module().Version());
  } else {
    Text.append("no-module|");
  }
  return Text;
}

[[nodiscard]] std::vector<std::string>
RenderRecords(const Luna::ReflectionRecordRange &Range) {
  std::vector<std::string> Texts;
  Texts.reserve(Range.Size());
  for (std::size_t Index = 0; Index < Range.Size(); ++Index)
    Texts.push_back(RenderRecord(Range.At(Index)));
  return Texts;
}

[[nodiscard]] std::vector<std::string>
RenderTypes(const Luna::TypeRecordRange &Range) {
  std::vector<std::string> Texts;
  Texts.reserve(Range.Size());
  for (std::size_t Index = 0; Index < Range.Size(); ++Index) {
    const Luna::TypeRecord Record = Range.At(Index);
    std::string Text;
    AppendField(Text, Record.Id().ToString());
    AppendField(Text, Record.Name());
    Texts.push_back(std::move(Text));
  }
  return Texts;
}

[[nodiscard]] std::vector<std::string>
RenderModules(const Luna::ModuleRecordRange &Range) {
  std::vector<std::string> Texts;
  Texts.reserve(Range.Size());
  for (std::size_t Index = 0; Index < Range.Size(); ++Index) {
    const Luna::ModuleRecord Record = Range.At(Index);
    std::string Text;
    AppendField(Text, Record.Identity());
    AppendField(Text, Record.Version());
    AppendField(Text, Record.Symbol().ToString());
    Texts.push_back(std::move(Text));
  }
  return Texts;
}

[[nodiscard]] std::vector<std::size_t> ForwardOrder(std::size_t Count) {
  std::vector<std::size_t> Order(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Order[Index] = Index;
  return Order;
}

[[nodiscard]] std::vector<std::size_t> ReversedOrder(std::size_t Count) {
  std::vector<std::size_t> Order = ForwardOrder(Count);
  std::reverse(Order.begin(), Order.end());
  return Order;
}

[[nodiscard]] std::vector<std::size_t>
ShuffledOrder(std::size_t Count, std::span<const std::uint8_t> Bytes) {
  std::vector<std::size_t> Order = ForwardOrder(Count);
  ByteCursor Cursor(Bytes);
  for (std::size_t Index = Count; Index > 1; --Index)
    std::swap(Order[Index - 1], Order[Cursor.Pick(Index)]);
  return Order;
}

template <class Identity>
[[nodiscard]] std::vector<std::size_t>
HashOrder(const std::vector<Identity> &Keys) {
  std::unordered_map<Identity, std::size_t, Luna::CanonicalHash> Container;
  for (std::size_t Index = 0; Index < Keys.size(); ++Index)
    Container.emplace(Keys[Index], Index);
  std::vector<std::size_t> Order;
  Order.reserve(Container.size());
  for (const auto &Entry : Container)
    Order.push_back(Entry.second);
  return Order;
}

[[nodiscard]] std::vector<std::size_t>
HashOrderByText(const std::vector<std::string> &Keys) {
  std::unordered_map<std::string, std::size_t> Container;
  for (std::size_t Index = 0; Index < Keys.size(); ++Index)
    Container.emplace(Keys[Index], Index);
  std::vector<std::size_t> Order;
  Order.reserve(Container.size());
  for (const auto &Entry : Container)
    Order.push_back(Entry.second);
  return Order;
}

[[nodiscard]] ReflectionGenerationBuilder
Assemble(const LogicalGeneration &Generation,
         const std::vector<std::size_t> &RecordOrder,
         const std::vector<std::size_t> &TypeOrder,
         const std::vector<std::size_t> &ModuleOrder) {
  ReflectionGenerationBuilder Candidate;
  std::vector<std::size_t> ModuleSlot(Generation.Modules.size(), 0);
  for (const std::size_t Index : ModuleOrder)
    ModuleSlot[Index] = Candidate.AddModule(Generation.Modules[Index]);
  for (const std::size_t Index : TypeOrder)
    Candidate.AddType(Generation.Types[Index]);
  for (const std::size_t Index : RecordOrder) {
    ReflectionRecordFields Fields = Generation.Records[Index];
    if (Fields.Module)
      Fields.Module = ModuleSlot[*Fields.Module];
    Candidate.AddRecord(std::move(Fields));
  }
  return Candidate;
}

void VerifySubmission(const LogicalGeneration &Generation,
                      const CanonicalModel &Model,
                      const ReflectionGenerationBuilder &Candidate) {
  ReflectionDatabase Database;
  RC_ASSERT(Database.PublishGeneration(Candidate) ==
            ReflectionGenerationStatus::Valid);
  RC_ASSERT(Database.Generation() == 1);
  RC_ASSERT(Database.Count() == Generation.Records.size());

  const Luna::ReflectionSnapshot Snapshot = Database.Snapshot();
  RC_ASSERT(Snapshot.Generation() == 1);
  RC_ASSERT(Snapshot.Size() == Generation.Records.size());

  RC_ASSERT(RenderRecords(Snapshot.Symbols()) == Model.Records);
  RC_ASSERT(RenderTypes(Snapshot.Types()) == Model.Types);
  RC_ASSERT(RenderModules(Snapshot.Modules()) == Model.Modules);

  for (std::size_t Kind = 0; Kind < SymbolKindCount; ++Kind) {
    const auto Value = static_cast<Luna::SymbolKind>(Kind);
    RC_ASSERT(RenderRecords(Snapshot.Symbols(Value)) == Model.ByKind[Kind]);
  }
  for (const auto &Scope : Model.ByScope)
    RC_ASSERT(RenderRecords(Snapshot.Symbols(Scope.first)) == Scope.second);
  RC_ASSERT(Snapshot.Symbols(Luna::ScopeId(Symbol(60000))).IsEmpty());

  for (const auto &Entry : Model.ById)
    RC_ASSERT(RenderRecord(Snapshot.Find(Entry.first)) == Entry.second);
  for (const auto &Entry : Model.FirstByQualifiedName)
    RC_ASSERT(RenderRecord(Snapshot.Find(Entry.first)) == Entry.second);
  RC_ASSERT(!Snapshot.Find(Symbol(60000)).IsValid());
  RC_ASSERT(!Snapshot.Find("Missing.Symbol").IsValid());
  for (const auto &Entry : Model.TypeById)
    RC_ASSERT(Snapshot.FindType(Entry.first).Name() == Entry.second);
}

} // namespace

int RunReflectionEnumerationOrderProperties() {

  const bool Passed = rc::check(

      "Reflection enumeration is permutation-invariant",
      [](const std::vector<std::uint8_t> &Shape,
         const std::vector<std::uint8_t> &PermutationShape) {
        ByteCursor Cursor(Shape);
        const LogicalGeneration Generation = MakeGeneration(Cursor);
        RC_ASSERT(!Generation.Records.empty());
        RC_ASSERT(!Generation.Modules.empty());
        RC_ASSERT(!Generation.Types.empty());

        const CanonicalModel Model = MakeModel(Generation);
        RC_ASSERT(Model.Records.size() == Generation.Records.size());

        const std::size_t RecordCount = Generation.Records.size();
        const std::size_t TypeCount = Generation.Types.size();
        const std::size_t ModuleCount = Generation.Modules.size();

        std::vector<Luna::SymbolId> RecordKeys;
        RecordKeys.reserve(RecordCount);
        for (const ReflectionRecordFields &Fields : Generation.Records)
          RecordKeys.push_back(Fields.Id);
        std::vector<Luna::TypeId> TypeKeys;
        TypeKeys.reserve(TypeCount);
        for (const ReflectionTypeFields &Fields : Generation.Types)
          TypeKeys.push_back(Fields.Id);
        std::vector<std::string> ModuleKeys;
        ModuleKeys.reserve(ModuleCount);
        for (const ReflectionModuleFields &Fields : Generation.Modules)
          ModuleKeys.push_back(Fields.Identity);

        VerifySubmission(Generation, Model,
                         Assemble(Generation, ForwardOrder(RecordCount),
                                  ForwardOrder(TypeCount),
                                  ForwardOrder(ModuleCount)));
        VerifySubmission(Generation, Model,
                         Assemble(Generation, ReversedOrder(RecordCount),
                                  ReversedOrder(TypeCount),
                                  ReversedOrder(ModuleCount)));
        VerifySubmission(
            Generation, Model,
            Assemble(Generation, ShuffledOrder(RecordCount, PermutationShape),
                     ShuffledOrder(TypeCount, PermutationShape),
                     ShuffledOrder(ModuleCount, PermutationShape)));
        VerifySubmission(Generation, Model,
                         Assemble(Generation, HashOrder(RecordKeys),
                                  HashOrder(TypeKeys),
                                  HashOrderByText(ModuleKeys)));
      });

  return Passed ? 0 : 1;
}
