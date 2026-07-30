// clang-format off
#include <luna/generation/declaration.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/generation/writer.hpp"
#include "state/userdata/class_operators.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {
namespace {

using Detail::ClassOperatorDescriptors;
using Detail::GenerationWriter;
using Detail::ModuleKeyText;

constexpr std::string_view ArtifactName = "Declaration";
constexpr std::string_view StrictModeLine = "--!strict";
constexpr std::string_view CommentPrefix = "-- ";
constexpr std::string_view IndentStep = "  ";

constexpr std::string_view ReservedWords[] = {
    "and", "break",    "do",     "else", "elseif", "end",   "false",
    "for", "function", "if",     "in",   "local",  "nil",   "not",
    "or",  "repeat",   "return", "then", "true",   "until", "while"};

constexpr std::string_view ReservedTypeNames[] = {
    "any",    "boolean", "never",   "nil",     "number",
    "string", "thread",  "unknown", "userdata"};

enum class CallableCategory { Static, Method, Operator, Unknown };

[[nodiscard]] CallableCategory CategoryOf(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::FunctionCandidate:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::StaticMethod:
    return CallableCategory::Static;
  case SymbolKind::Method:
    return CallableCategory::Method;
  case SymbolKind::Operator:
    return CallableCategory::Operator;
  default:
    return CallableCategory::Unknown;
  }
}

[[nodiscard]] bool IsCallableKind(SymbolKind Kind) noexcept {
  return CategoryOf(Kind) != CallableCategory::Unknown;
}

[[nodiscard]] bool Contains(std::span<const std::string_view> Words,
                            std::string_view Text) {
  for (const std::string_view Word : Words) {
    if (Word == Text)
      return true;
  }
  return false;
}

[[nodiscard]] bool IsIdentifier(std::string_view Text) {
  if (Text.empty())
    return false;
  const auto Start = static_cast<unsigned char>(Text.front());
  const bool Leading = Start == '_' || (Start >= 'A' && Start <= 'Z') ||
                       (Start >= 'a' && Start <= 'z');
  if (!Leading)
    return false;
  for (const char Character : Text) {
    const auto Byte = static_cast<unsigned char>(Character);
    const bool Allowed = Byte == '_' || (Byte >= '0' && Byte <= '9') ||
                         (Byte >= 'A' && Byte <= 'Z') ||
                         (Byte >= 'a' && Byte <= 'z');
    if (!Allowed)
      return false;
  }
  return true;
}

[[nodiscard]] std::string Context(std::string_view Subject,
                                  std::string_view Field) {
  std::string Text("the ");
  Text.append(Field);
  Text.append(" of ");
  Text.append(Subject.empty() ? std::string_view("one unnamed declaration")
                              : Subject);
  return Text;
}

class Generator final {
public:
  Generator(const ReflectionSnapshot &Snapshot,
            const DeclarationOptions &Options)
      : SnapshotValue(Snapshot), OptionsValue(Options) {}

  [[nodiscard]] GeneratedArtifact Run() {
    if (Prepare()) {
      WriteHeader();
      WriteClassSection();
      WriteGlobalSection();
    }
    return Writer.Release(ArtifactName);
  }

private:
  using IndexList = std::vector<std::size_t>;

  [[nodiscard]] bool Prepare() {
    const ReflectionRecordRange Symbols = SnapshotValue.Symbols();
    Records.reserve(Symbols.Size());
    for (std::size_t Index = 0; Index < Symbols.Size(); ++Index)
      Records.push_back(Symbols.At(Index));

    for (std::size_t Index = 0; Index < Records.size(); ++Index) {
      const ReflectionRecord &Record = Records[Index];
      if (!Record.Id().IsValid())
        return Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Record.QualifiedName(), "stable identity"));
      if (!IndexById.emplace(Record.Id(), Index).second)
        return Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Record.QualifiedName(), "duplicated stable identity"));
    }

    for (std::size_t Index = 0; Index < Records.size(); ++Index) {
      const ReflectionRecord &Record = Records[Index];
      if (Record.Scope().IsRoot()) {
        RootChildren.push_back(Index);
        continue;
      }
      ChildrenByScope[Record.Scope().Owner()].push_back(Index);
    }

    return CheckScopes() && RegisterClassNames();
  }

  [[nodiscard]] bool CheckScopes() {
    for (const ReflectionRecord &Record : Records) {
      if (Record.Scope().IsRoot()) {
        if (!AcceptsChild(SymbolKind::Namespace, Record.Kind()))
          return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                               Context(Record.QualifiedName(), "root scope"));
        continue;
      }
      const auto Owner = IndexById.find(Record.Scope().Owner());
      if (Owner == IndexById.end())
        return Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Record.QualifiedName(), "unknown enclosing scope"));
      if (!AcceptsChild(Records[Owner->second].Kind(), Record.Kind()))
        return Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Record.QualifiedName(), "enclosing scope"));
    }
    return true;
  }

  [[nodiscard]] static bool AcceptsChild(SymbolKind Owner,
                                         SymbolKind Child) noexcept {
    switch (Child) {
    case SymbolKind::Namespace:
    case SymbolKind::Class:
    case SymbolKind::Enumeration:
    case SymbolKind::Module:
    case SymbolKind::Type:
      return Owner == SymbolKind::Namespace;
    case SymbolKind::Constant:
      return Owner == SymbolKind::Namespace || Owner == SymbolKind::Class;
    case SymbolKind::OverloadSet:
      return Owner == SymbolKind::Namespace || Owner == SymbolKind::Class;
    case SymbolKind::FunctionCandidate:
    case SymbolKind::Constructor:
    case SymbolKind::Factory:
    case SymbolKind::Method:
    case SymbolKind::StaticMethod:
    case SymbolKind::Operator:
      return Owner == SymbolKind::OverloadSet ||
             Owner == SymbolKind::Namespace || Owner == SymbolKind::Class;
    case SymbolKind::Property:
    case SymbolKind::Field:
      return Owner == SymbolKind::Class;
    case SymbolKind::Enumerator:
    case SymbolKind::EnumeratorAlias:
      return Owner == SymbolKind::Enumeration;
    }
    return false;
  }

  [[nodiscard]] bool RegisterClassNames() {
    for (const ReflectionRecord &Record : Records) {
      if (Record.Kind() != SymbolKind::Class)
        continue;
      const std::string Qualified(Record.QualifiedName());
      std::string Identifier;
      std::size_t Position = 0;
      while (Position <= Qualified.size()) {
        const std::size_t Separator = Qualified.find('.', Position);
        const std::size_t End =
            Separator == std::string::npos ? Qualified.size() : Separator;
        const std::string_view Segment(Qualified.data() + Position,
                                       End - Position);
        if (!IsIdentifier(Segment) || IsReserved(Segment))
          return Writer.Refuse(GenerationStatus::UnsupportedDeclaration,
                               Context(Qualified, "class name"));
        if (!Identifier.empty())
          Identifier.push_back('_');
        Identifier.append(Segment);
        if (Separator == std::string::npos)
          break;
        Position = Separator + 1;
      }
      if (IsReserved(Identifier))
        return Writer.Refuse(GenerationStatus::UnsupportedDeclaration,
                             Context(Qualified, "class type name"));
      const std::string Key(Record.Descriptor().Key().Text());
      if (Key.empty())
        return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                             Context(Qualified, "canonical class identity"));
      if (!ClassNames.emplace(Key, Identifier).second ||
          !DeclaredNames.emplace(Identifier, Key).second)
        return Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Qualified, "Luau type name shared with another class"));
    }
    return true;
  }

  [[nodiscard]] static bool IsReserved(std::string_view Text) {
    return Contains(std::span<const std::string_view>(ReservedWords), Text) ||
           Contains(std::span<const std::string_view>(ReservedTypeNames), Text);
  }

  [[nodiscard]] const IndexList &ChildrenOf(const SymbolId &Owner) const {
    static const IndexList Empty;
    const auto Found = ChildrenByScope.find(Owner);
    return Found == ChildrenByScope.end() ? Empty : Found->second;
  }

  [[nodiscard]] const IndexList &
  ChildrenOf(const ReflectionRecord &Record) const {
    return ChildrenOf(Record.Id());
  }

  [[nodiscard]] bool MapType(const TypeDescriptor &Type,
                             const std::string &Where, std::string &Out) {
    switch (Type.Kind()) {
    case TypeKind::Fixed:
      return MapFixed(Type, Where, Out);
    case TypeKind::Enumeration:
      Out = "number";
      return true;
    case TypeKind::Class:
      return MapClass(Type, Where, Out);
    case TypeKind::Converted:

      Out = "any";
      return true;
    case TypeKind::Pointer:
    case TypeKind::SharedOwnership:
    case TypeKind::BorrowedReference: {
      if (Type.ChildCount() != 1)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      const TypeDescriptor &Pointee = Type.Children().front();
      if (Pointee.Kind() != TypeKind::Class)
        return Writer.Refuse(GenerationStatus::UnrepresentableType, Where);
      return MapClass(Pointee, Where, Out);
    }
    case TypeKind::Optional: {
      if (Type.ChildCount() != 1)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      std::string Element;
      if (!MapType(Type.Children().front(), Where, Element))
        return false;
      if (Element.ends_with('?'))
        return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
      Out = Element + "?";
      return true;
    }
    case TypeKind::Sequence:
    case TypeKind::Array: {
      if (Type.ChildCount() != 1)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      std::string Element;
      if (!MapType(Type.Children().front(), Where, Element))
        return false;
      Out = "{" + Element + "}";
      return true;
    }
    case TypeKind::Map: {
      if (Type.ChildCount() != 2)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      std::string Key;
      std::string Element;
      if (!MapType(Type.Children()[0], Where, Key) ||
          !MapType(Type.Children()[1], Where, Element))
        return false;
      Out = "{[" + Key + "]: " + Element + "}";
      return true;
    }
    case TypeKind::Callable: {

      if (Type.ChildCount() < 1)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      const std::span<const TypeDescriptor> Children = Type.Children();
      std::string Parameters;
      for (std::size_t Index = 1; Index < Children.size(); ++Index) {
        std::string Parameter;
        if (!MapType(Children[Index], Where, Parameter))
          return false;
        if (Index != 1)
          Parameters.append(", ");
        Parameters.append(Parameter);
      }

      std::string Result("()");
      if (Children[0].FixedKey() != FixedTypeKey::Void &&
          !MapType(Children[0], Where, Result))
        return false;
      Out = "((" + Parameters + ") -> " + Result + ")";
      return true;
    }
    case TypeKind::Pair:
    case TypeKind::Tuple:
    case TypeKind::ArgumentPack:
    case TypeKind::ReturnPack:
      return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
    case TypeKind::Unsupported:
      break;
    }
    return Writer.Refuse(GenerationStatus::UnrepresentableType, Where);
  }

  [[nodiscard]] bool MapFixed(const TypeDescriptor &Type,
                              const std::string &Where, std::string &Out) {
    const std::optional<FixedTypeKey> Key = Type.FixedKey();
    if (!Key)
      return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
    switch (*Key) {
    case FixedTypeKey::Boolean:
      Out = "boolean";
      return true;
    case FixedTypeKey::Int32:
    case FixedTypeKey::Float:
    case FixedTypeKey::Double:
      Out = "number";
      return true;
    case FixedTypeKey::String:
    case FixedTypeKey::StringView:
    case FixedTypeKey::CString:
      Out = "string";
      return true;
    case FixedTypeKey::Null:
      Out = "nil";
      return true;
    case FixedTypeKey::Value:
      Out = "any";
      return true;
    case FixedTypeKey::Void:
    case FixedTypeKey::ValuePack:
      return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
    }
    return Writer.Refuse(GenerationStatus::UnrepresentableType, Where);
  }

  [[nodiscard]] bool MapClass(const TypeDescriptor &Type,
                              const std::string &Where, std::string &Out) {
    const auto Found = ClassNames.find(std::string(Type.Key().Text()));
    if (Found == ClassNames.end())
      return Writer.Refuse(GenerationStatus::UnrepresentableType, Where);
    Out = Found->second;
    return true;
  }

  [[nodiscard]] bool ParameterList(const ReflectionRecord &Record, bool Named,
                                   std::string_view SelfType,
                                   std::string &Out) {
    const std::string Subject(Record.QualifiedName());
    std::string Text;
    if (!SelfType.empty()) {
      if (Named)
        Text.append("self: ");
      Text.append(SelfType);
    }

    bool SawOmittable = false;
    bool SawVariadic = false;
    for (std::size_t Index = 0; Index < Record.ParameterCount(); ++Index) {
      const ParameterRecord Parameter = Record.Parameter(Index);
      const std::string Where =
          Context(Subject, "parameter " + std::to_string(Index + 1));
      const ParameterDisposition Disposition = Parameter.Disposition();

      if (SawVariadic)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      const bool Omittable = Disposition == ParameterDisposition::Optional ||
                             Disposition == ParameterDisposition::Defaulted;
      if (Disposition == ParameterDisposition::Required && SawOmittable)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);
      SawOmittable = SawOmittable || Omittable;
      SawVariadic = Disposition == ParameterDisposition::Variadic;

      std::string Element;
      if (!MapType(Parameter.Descriptor(), Where, Element))
        return false;
      if (Omittable && !Element.ends_with('?'))
        Element.push_back('?');

      if (!Text.empty())
        Text.append(", ");
      if (SawVariadic) {
        Text.append(Named ? "...: " : "...");
        Text.append(Element);
        continue;
      }
      if (Named) {
        if (!IsIdentifier(Parameter.Name()) || IsReserved(Parameter.Name()))
          return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
        Text.append(Parameter.Name());
        Text.append(": ");
      }
      Text.append(Element);
    }
    Out = "(" + Text + ")";
    return true;
  }

  [[nodiscard]] bool ReturnType(const ReflectionRecord &Record,
                                std::string &Out) {
    const std::string Subject(Record.QualifiedName());
    const std::size_t Count = Record.ReturnCount();
    const ReturnShape Shape = Record.Returns();
    const std::string Where = Context(Subject, "return shape");

    const bool Agrees = (Shape == ReturnShape::Zero && Count == 0) ||
                        (Shape == ReturnShape::Scalar && Count == 1) ||
                        (Shape == ReturnShape::Multiple && Count != 1);
    if (!Agrees)
      return Writer.Refuse(GenerationStatus::InconsistentMetadata, Where);

    if (Count == 0) {
      Out = Shape == ReturnShape::Multiple ? "...any" : "()";
      return true;
    }

    std::string Text;
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const ReturnRecord Return = Record.Return(Index);
      std::string Element;
      if (!MapType(
              Return.Descriptor(),
              Context(Subject, "returned value " + std::to_string(Index + 1)),
              Element))
        return false;
      if (!Text.empty())
        Text.append(", ");
      Text.append(Element);
    }
    Out = Count == 1 ? Text : "(" + Text + ")";
    return true;
  }

  [[nodiscard]] bool FunctionType(const ReflectionRecord &Record,
                                  std::string_view SelfType, std::string &Out) {
    std::string Parameters;
    std::string Returns;
    if (!ParameterList(Record, false, SelfType, Parameters) ||
        !ReturnType(Record, Returns))
      return false;
    Out = Parameters + " -> " + Returns;
    return true;
  }

  [[nodiscard]] bool Candidates(std::size_t Index, IndexList &Out) {
    const ReflectionRecord &Record = Records[Index];
    if (IsCallableKind(Record.Kind())) {
      Out.push_back(Index);
      return true;
    }
    for (const std::size_t Child : ChildrenOf(Record)) {
      if (IsCallableKind(Records[Child].Kind()))
        Out.push_back(Child);
    }
    if (Out.empty())
      return Writer.Refuse(
          GenerationStatus::InconsistentMetadata,
          Context(Record.QualifiedName(), "overload set without a candidate"));
    return true;
  }

  [[nodiscard]] bool CategoryOfSet(std::size_t Index, const IndexList &Group,
                                   CallableCategory &Out) {
    Out = CategoryOf(Records[Group.front()].Kind());
    for (const std::size_t Candidate : Group) {
      if (CategoryOf(Records[Candidate].Kind()) != Out)
        return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                             Context(Records[Index].QualifiedName(),
                                     "overload candidates of one name"));
    }
    return true;
  }

  [[nodiscard]] bool OverloadType(const IndexList &Group,
                                  std::string_view SelfType, std::string &Out) {
    if (Group.size() == 1)
      return FunctionType(Records[Group.front()], SelfType, Out);
    std::string Text;
    for (const std::size_t Candidate : Group) {
      std::string Element;
      if (!FunctionType(Records[Candidate], SelfType, Element))
        return false;
      if (!Text.empty())
        Text.append(" & ");
      Text.append("(" + Element + ")");
    }
    Out = std::move(Text);
    return true;
  }

  const ReflectionSnapshot &SnapshotValue;
  const DeclarationOptions &OptionsValue;
  GenerationWriter Writer;
  std::vector<ReflectionRecord> Records;
  std::map<SymbolId, std::size_t> IndexById;
  std::map<SymbolId, IndexList> ChildrenByScope;
  IndexList RootChildren;
  std::map<std::string, std::string> ClassNames;
  std::map<std::string, std::string> DeclaredNames;

  [[nodiscard]] static std::string Indentation(std::size_t Depth) {
    std::string Text;
    for (std::size_t Level = 0; Level < Depth; ++Level)
      Text.append(IndentStep);
    return Text;
  }

  [[nodiscard]] bool WriteName(std::string_view Name,
                               const std::string &Where) {
    if (!IsIdentifier(Name) || IsReserved(Name))
      return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
    return Writer.Inline(Name, Where);
  }

  void WriteCallableDocumentation(std::size_t Index, const IndexList &Group,
                                  std::size_t Depth) {
    const ReflectionRecord &Named = Records[Index];
    if (!Named.Documentation().empty() || Group.size() != 1) {
      WriteDocumentation(Named, Depth);
      return;
    }
    WriteDocumentation(Records[Group.front()], Depth);
  }

  void WriteDocumentation(const ReflectionRecord &Record, std::size_t Depth) {
    if (!OptionsValue.IncludesDocumentation() || Record.Documentation().empty())
      return;
    const std::string Prefix = Indentation(Depth) + std::string(CommentPrefix);
    static_cast<void>(
        Writer.Block(Record.Documentation(), Prefix,
                     Context(Record.QualifiedName(), "documentation text")));
  }

  void WriteProvenance(const ReflectionRecord &Record) {
    if (!OptionsValue.IncludesProvenance() || !Record.HasModule())
      return;
    Writer.Literal(" ");
    Writer.Literal(CommentPrefix);
    static_cast<void>(
        Writer.Inline(ModuleKeyText(Record.Module()),
                      Context(Record.QualifiedName(), "module provenance")));
  }

  void WriteHeader() {
    if (OptionsValue.IncludesStrictMode()) {
      Writer.Literal(StrictModeLine);
      Writer.Break();
    }
    const std::string Banner(OptionsValue.Banner());
    Writer.Literal(CommentPrefix);
    static_cast<void>(Writer.Inline(Banner, Context(Banner, "banner comment")));
    Writer.Break();

    if (OptionsValue.IncludesProvenance()) {
      const ModuleRecordRange Modules = SnapshotValue.Modules();
      if (Modules.IsEmpty()) {
        Writer.Literal(CommentPrefix);
        Writer.Literal("No module contributed a declaration.");
        Writer.Break();
      }
      for (std::size_t Index = 0; Index < Modules.Size(); ++Index) {
        const std::string Key = ModuleKeyText(Modules.At(Index));
        Writer.Literal(CommentPrefix);
        Writer.Literal("Module: ");
        static_cast<void>(Writer.Inline(Key, Context(Key, "module identity")));
        Writer.Break();
      }
    }
    Writer.Break();
  }

  void WriteClassSection() {
    std::vector<std::pair<std::size_t, std::string>> Pending;
    for (std::size_t Index = 0; Index < Records.size(); ++Index) {
      if (Records[Index].Kind() != SymbolKind::Class)
        continue;
      std::string Base;
      if (!BaseOf(Records[Index], Base))
        return;
      Pending.emplace_back(Index, std::move(Base));
    }

    std::map<std::string, std::size_t> Declared;
    while (!Pending.empty()) {
      std::vector<std::pair<std::size_t, std::string>> Deferred;
      for (auto &Entry : Pending) {
        if (!Entry.second.empty() &&
            Declared.find(Entry.second) == Declared.end()) {
          Deferred.push_back(std::move(Entry));
          continue;
        }
        if (!WriteClassBlock(Entry.first, Entry.second))
          return;
        Writer.Break();
        Declared.emplace(NameOfClass(Entry.first), Entry.first);
      }
      if (Deferred.size() == Pending.size()) {
        static_cast<void>(Writer.Refuse(
            GenerationStatus::InconsistentMetadata,
            Context(Records[Deferred.front().first].QualifiedName(),
                    "base class cycle")));
        return;
      }
      Pending = std::move(Deferred);
    }
  }

  [[nodiscard]] std::string NameOfClass(std::size_t Index) const {
    const auto Named =
        ClassNames.find(std::string(Records[Index].Descriptor().Key().Text()));
    return Named == ClassNames.end() ? std::string() : Named->second;
  }

  [[nodiscard]] bool WriteClassBlock(std::size_t Index,
                                     const std::string &Base) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());
    const auto Named =
        ClassNames.find(std::string(Record.Descriptor().Key().Text()));
    if (Named == ClassNames.end())
      return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                           Context(Subject, "canonical class identity"));

    WriteDocumentation(Record, 0);
    Writer.Literal("declare class ");
    Writer.Literal(Named->second);
    if (!Base.empty()) {
      Writer.Literal(" extends ");
      Writer.Literal(Base);
    }
    WriteProvenance(Record);
    Writer.Break();

    for (const std::size_t Child : ChildrenOf(Record)) {
      if (!WriteClassMember(Child, Named->second))
        return false;
    }
    Writer.Literal("end");
    Writer.Break();
    return true;
  }

  [[nodiscard]] bool BaseOf(const ReflectionRecord &Record, std::string &Out) {
    const std::string Subject(Record.QualifiedName());
    for (std::size_t Index = 0; Index < Record.RelationCount(); ++Index) {
      const TypeRelation Relation = Record.Relation(Index);
      if (Relation.Kind() != TypeRelationKind::Base ||
          Relation.Note() == "inaccessible")
        continue;
      const std::string Where = Context(Subject, "base class");
      const TypeRecord Base = SnapshotValue.FindType(Relation.Type());
      if (!Base.IsValid())
        return Writer.Refuse(GenerationStatus::UnrepresentableType, Where);
      std::string Named;
      if (!MapClass(Base.Descriptor(), Where, Named))
        return false;
      if (!Out.empty())
        return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);
      Out = std::move(Named);
    }
    return true;
  }

  [[nodiscard]] bool WriteClassMember(std::size_t Index,
                                      const std::string &SelfType) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());

    if (Record.Kind() == SymbolKind::Constant)
      return true;

    if (Record.Kind() == SymbolKind::Property ||
        Record.Kind() == SymbolKind::Field) {
      std::string Element;
      if (!MapType(Record.Descriptor(), Context(Subject, "declared type"),
                   Element))
        return false;
      if (!Record.IsReadable() && !Record.IsWritable())
        return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                             Context(Subject, "permitted directions"));
      WriteDocumentation(Record, 1);
      Writer.Literal(Indentation(1));
      if (Record.IsReadable() != Record.IsWritable())
        Writer.Literal(Record.IsReadable() ? "read " : "write ");
      if (!WriteName(Record.Name(), Context(Subject, "member name")))
        return false;
      Writer.Literal(": ");
      Writer.Literal(Element);
      WriteProvenance(Record);
      Writer.Break();
      return true;
    }

    IndexList Group;
    if (!Candidates(Index, Group))
      return false;
    CallableCategory Category = CallableCategory::Unknown;
    if (!CategoryOfSet(Index, Group, Category))
      return false;

    if (Category == CallableCategory::Static)
      return true;
    if (Category == CallableCategory::Operator)
      return WriteOperator(Index, Group, SelfType);
    return WriteMethod(Index, Group, SelfType);
  }

  [[nodiscard]] bool WriteMethod(std::size_t Index, const IndexList &Group,
                                 const std::string &SelfType) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());
    WriteCallableDocumentation(Index, Group, 1);
    Writer.Literal(Indentation(1));

    if (Group.size() == 1) {
      const ReflectionRecord &Candidate = Records[Group.front()];
      std::string Parameters;
      std::string Returns;
      if (!ParameterList(Candidate, true, SelfType, Parameters) ||
          !ReturnType(Candidate, Returns))
        return false;
      Writer.Literal("function ");
      if (!WriteName(Record.Name(), Context(Subject, "member name")))
        return false;
      Writer.Literal(Parameters);
      Writer.Literal(": ");
      Writer.Literal(Returns);
      WriteProvenance(Record);
      Writer.Break();
      return true;
    }

    std::string Overloads;
    if (!OverloadType(Group, SelfType, Overloads))
      return false;
    if (!WriteName(Record.Name(), Context(Subject, "member name")))
      return false;
    Writer.Literal(": ");
    Writer.Literal(Overloads);
    WriteProvenance(Record);
    Writer.Break();
    return true;
  }

  [[nodiscard]] bool WriteOperator(std::size_t Index, const IndexList &Group,
                                   const std::string &SelfType) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());
    const std::string Where = Context(Subject, "declared operator");

    const Detail::ClassOperatorDescriptor *Described = nullptr;
    for (const Detail::ClassOperatorDescriptor &Candidate :
         ClassOperatorDescriptors()) {
      if (Candidate.Segment == Record.Name()) {
        Described = &Candidate;
        break;
      }
    }
    if (Described == nullptr)
      return Writer.Refuse(GenerationStatus::UnsupportedDeclaration, Where);

    if (Described->Metamethod.empty()) {
      Writer.Literal(Indentation(1));
      Writer.Literal(CommentPrefix);
      Writer.Literal(ClassOperatorText(Described->Selected));
      Writer.Literal(" is answered by Luna's reserved dispatch.");
      Writer.Break();
      return true;
    }

    WriteCallableDocumentation(Index, Group, 1);
    Writer.Literal(Indentation(1));
    if (Group.size() == 1) {
      const ReflectionRecord &Candidate = Records[Group.front()];
      std::string Parameters;
      std::string Returns;
      if (!ParameterList(Candidate, true, SelfType, Parameters) ||
          !ReturnType(Candidate, Returns))
        return false;
      Writer.Literal("function ");
      Writer.Literal(Described->Metamethod);
      Writer.Literal(Parameters);
      Writer.Literal(": ");
      Writer.Literal(Returns);
      WriteProvenance(Record);
      Writer.Break();
      return true;
    }

    std::string Overloads;
    if (!OverloadType(Group, SelfType, Overloads))
      return false;
    Writer.Literal(Described->Metamethod);
    Writer.Literal(": ");
    Writer.Literal(Overloads);
    WriteProvenance(Record);
    Writer.Break();
    return true;
  }

  void WriteGlobalSection() {
    for (const std::size_t Index : RootChildren) {
      if (Writer.IsRejected())
        return;
      if (!WriteGlobal(Index))
        return;
    }
  }

  [[nodiscard]] bool WriteGlobal(std::size_t Index) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());

    if (Record.Kind() == SymbolKind::Module ||
        Record.Kind() == SymbolKind::Type)
      return true;

    switch (Record.Kind()) {
    case SymbolKind::Namespace:
    case SymbolKind::Class:
    case SymbolKind::Enumeration: {
      WriteDocumentation(Record, 0);
      Writer.Literal("declare ");
      if (!WriteName(Record.Name(), Context(Subject, "declared name")))
        return false;
      Writer.Literal(": ");
      if (!WriteTable(Index, 0))
        return false;
      WriteProvenance(Record);
      Writer.Break();
      Writer.Break();
      return true;
    }
    case SymbolKind::Constant: {
      std::string Element;
      if (!MapType(Record.Descriptor(), Context(Subject, "declared type"),
                   Element))
        return false;
      WriteDocumentation(Record, 0);
      Writer.Literal("declare ");
      if (!WriteName(Record.Name(), Context(Subject, "declared name")))
        return false;
      Writer.Literal(": ");
      Writer.Literal(Element);
      WriteProvenance(Record);
      Writer.Break();
      Writer.Break();
      return true;
    }
    default:
      break;
    }

    IndexList Group;
    if (!Candidates(Index, Group))
      return false;
    CallableCategory Category = CallableCategory::Unknown;
    if (!CategoryOfSet(Index, Group, Category))
      return false;
    if (Category != CallableCategory::Static)
      return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                           Context(Subject, "root-scope member declaration"));

    WriteCallableDocumentation(Index, Group, 0);
    if (Group.size() == 1) {
      const ReflectionRecord &Candidate = Records[Group.front()];
      std::string Parameters;
      std::string Returns;
      if (!ParameterList(Candidate, true, std::string_view(), Parameters) ||
          !ReturnType(Candidate, Returns))
        return false;
      Writer.Literal("declare function ");
      if (!WriteName(Record.Name(), Context(Subject, "declared name")))
        return false;
      Writer.Literal(Parameters);
      Writer.Literal(": ");
      Writer.Literal(Returns);
      WriteProvenance(Record);
      Writer.Break();
      Writer.Break();
      return true;
    }

    std::string Overloads;
    if (!OverloadType(Group, std::string_view(), Overloads))
      return false;
    Writer.Literal("declare ");
    if (!WriteName(Record.Name(), Context(Subject, "declared name")))
      return false;
    Writer.Literal(": ");
    Writer.Literal(Overloads);
    WriteProvenance(Record);
    Writer.Break();
    Writer.Break();
    return true;
  }

  [[nodiscard]] bool TableEntries(std::size_t Index, IndexList &Out) {
    const ReflectionRecord &Record = Records[Index];
    for (const std::size_t Child : ChildrenOf(Record)) {
      const SymbolKind Kind = Records[Child].Kind();
      if (Kind == SymbolKind::Module || Kind == SymbolKind::Type)
        continue;
      if (Record.Kind() == SymbolKind::Enumeration) {
        Out.push_back(Child);
        continue;
      }
      if (Kind == SymbolKind::OverloadSet || IsCallableKind(Kind)) {
        IndexList Group;
        if (!Candidates(Child, Group))
          return false;
        CallableCategory Category = CallableCategory::Unknown;
        if (!CategoryOfSet(Child, Group, Category))
          return false;
        if (Category == CallableCategory::Static)
          Out.push_back(Child);
        continue;
      }
      if (Record.Kind() == SymbolKind::Class && Kind != SymbolKind::Constant)
        continue;
      Out.push_back(Child);
    }
    return true;
  }

  [[nodiscard]] bool WriteTable(std::size_t Index, std::size_t Depth) {
    IndexList Entries;
    if (!TableEntries(Index, Entries))
      return false;
    if (Entries.empty()) {
      Writer.Literal("{}");
      return true;
    }
    Writer.Literal("{");
    Writer.Break();
    for (const std::size_t Entry : Entries) {
      if (!WriteTableEntry(Entry, Depth + 1))
        return false;
    }
    Writer.Literal(Indentation(Depth));
    Writer.Literal("}");
    return true;
  }

  [[nodiscard]] bool WriteTableEntry(std::size_t Index, std::size_t Depth) {
    const ReflectionRecord &Record = Records[Index];
    const std::string Subject(Record.QualifiedName());

    switch (Record.Kind()) {
    case SymbolKind::Namespace:
    case SymbolKind::Class:
    case SymbolKind::Enumeration: {
      WriteDocumentation(Record, Depth);
      Writer.Literal(Indentation(Depth));
      if (!WriteName(Record.Name(), Context(Subject, "declared name")))
        return false;
      Writer.Literal(": ");
      if (!WriteTable(Index, Depth))
        return false;
      Writer.Literal(",");
      WriteProvenance(Record);
      Writer.Break();
      return true;
    }
    case SymbolKind::Constant:
    case SymbolKind::Enumerator:
    case SymbolKind::EnumeratorAlias: {
      std::string Element;
      if (!MapType(Record.Descriptor(), Context(Subject, "declared type"),
                   Element))
        return false;
      WriteDocumentation(Record, Depth);
      Writer.Literal(Indentation(Depth));
      if (!WriteName(Record.Name(), Context(Subject, "declared name")))
        return false;
      Writer.Literal(": ");
      Writer.Literal(Element);
      Writer.Literal(",");
      WriteProvenance(Record);
      Writer.Break();
      return true;
    }
    default:
      break;
    }

    IndexList Group;
    if (!Candidates(Index, Group))
      return false;
    CallableCategory Category = CallableCategory::Unknown;
    if (!CategoryOfSet(Index, Group, Category))
      return false;
    if (Category != CallableCategory::Static)
      return Writer.Refuse(GenerationStatus::InconsistentMetadata,
                           Context(Subject, "declared member category"));

    std::string Overloads;
    if (!OverloadType(Group, std::string_view(), Overloads))
      return false;
    WriteCallableDocumentation(Index, Group, Depth);
    Writer.Literal(Indentation(Depth));
    if (!WriteName(Record.Name(), Context(Subject, "declared name")))
      return false;
    Writer.Literal(": ");
    Writer.Literal(Overloads);
    Writer.Literal(",");
    WriteProvenance(Record);
    Writer.Break();
    return true;
  }
};

} // namespace

GeneratedArtifact GenerateDeclarations(const ReflectionSnapshot &Snapshot,
                                       const DeclarationOptions &Options) {
  Generator Declarations(Snapshot, Options);
  return Declarations.Run();
}

} // namespace Luna
