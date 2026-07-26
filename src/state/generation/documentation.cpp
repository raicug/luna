// Deterministic human-readable documentation generation. Everything written
// here comes from one captured immutable reflection snapshot: the traversal is
// module provenance, then the canonical symbol order the generation published
// (qualified name, symbol kind, declaration signature, stable identity), so the
// artifact never depends on registration order, addresses, locale, hash
// iteration, process-random values, or later changes to a live State.

// clang-format off
#include <luna/generation/documentation.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include "state/generation/writer.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna {
namespace {

using Detail::GenerationWriter;
using Detail::ModuleKeyText;
using Detail::SymbolText;
using Detail::TypeText;

// Continuation lines of one prose block and one list entry line up under their
// entry text.
constexpr std::string_view ProseIndent = "   ";
constexpr std::string_view ArtifactName = "Documentation";
constexpr std::string_view RootScopeText = "(root)";
constexpr std::string_view WithoutModuleText = "(no module)";

// Only these kinds declare one call shape of their own, so only they report a
// return shape even when they publish no named returned value.
[[nodiscard]] bool DeclaresCallShape(SymbolKind Kind) {
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

[[nodiscard]] std::string Context(std::string_view Subject,
                                  std::string_view Field) {
  std::string Text("the ");
  Text.append(Field);
  Text.append(" of ");
  Text.append(Subject.empty() ? std::string_view("one unnamed symbol")
                              : Subject);
  return Text;
}

[[nodiscard]] std::string Count(std::size_t Value) {
  return std::to_string(Value);
}

void WriteCounts(GenerationWriter &Writer, const ReflectionSnapshot &Snapshot) {
  Writer.Literal("Symbols: ");
  Writer.Literal(Count(Snapshot.Symbols().Size()));
  Writer.Break();
  Writer.Literal("Types: ");
  Writer.Literal(Count(Snapshot.Types().Size()));
  Writer.Break();
  Writer.Literal("Modules: ");
  Writer.Literal(Count(Snapshot.Modules().Size()));
  Writer.Break();
}

void WriteProse(GenerationWriter &Writer, std::string_view Label,
                std::string_view Value, std::string_view Subject,
                std::string_view Field) {
  if (Value.empty())
    return;
  Writer.Literal(Label);
  Writer.Literal(":");
  Writer.Break();
  static_cast<void>(Writer.Block(Value, ProseIndent, Context(Subject, Field)));
}

void WriteModule(GenerationWriter &Writer, const ModuleRecord &Module) {
  const std::string Key = ModuleKeyText(Module);
  Writer.Literal("### ");
  static_cast<void>(Writer.Inline(Key, Context(Key, "module identity")));
  Writer.Break();
  WriteProse(Writer, "Documentation", Module.Documentation(), Key,
             "module documentation");

  if (Module.DependencyCount() != 0) {
    Writer.Literal("Dependencies:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Module.DependencyCount(); ++Index) {
      const ModuleDependencyRecord Dependency = Module.Dependency(Index);
      Writer.Literal("- ");
      static_cast<void>(Writer.Inline(Dependency.Identity(),
                                      Context(Key, "dependency identity")));
      Writer.Literal("@");
      static_cast<void>(Writer.Inline(Dependency.Version(),
                                      Context(Key, "dependency version")));
      if (!Dependency.Constraints().empty()) {
        Writer.Literal(" [");
        static_cast<void>(Writer.Inline(
            Dependency.Constraints(), Context(Key, "dependency constraints")));
        Writer.Literal("]");
      }
      Writer.Break();
    }
  }

  if (Module.ExportCount() != 0) {
    Writer.Literal("Exports:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Module.ExportCount(); ++Index) {
      const ModuleExportRecord Export = Module.Export(Index);
      Writer.Literal("- ");
      Writer.Literal(SymbolKindText(Export.Kind()));
      Writer.Literal(" ");
      static_cast<void>(
          Writer.Inline(Export.Name(), Context(Key, "export name")));
      Writer.Break();
      WriteProse(Writer, "  Documentation", Export.Documentation(), Key,
                 "export documentation");
    }
  }

  if (Module.NamespaceCount() != 0) {
    Writer.Literal("Namespaces:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Module.NamespaceCount(); ++Index) {
      Writer.Literal("- ");
      static_cast<void>(Writer.Inline(Module.Namespace(Index),
                                      Context(Key, "declared namespace")));
      Writer.Break();
    }
  }

  if (Module.TypeCount() != 0) {
    Writer.Literal("Types:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Module.TypeCount(); ++Index) {
      Writer.Literal("- ");
      static_cast<void>(
          Writer.Inline(Module.TypeName(Index), Context(Key, "declared type")));
      Writer.Break();
    }
  }
  Writer.Break();
}

void WriteModules(GenerationWriter &Writer,
                  const ReflectionSnapshot &Snapshot) {
  Writer.Literal("## Modules");
  Writer.Break();
  Writer.Break();
  const ModuleRecordRange Modules = Snapshot.Modules();
  if (Modules.IsEmpty()) {
    Writer.Literal("None.");
    Writer.Break();
    Writer.Break();
    return;
  }
  for (std::size_t Index = 0; Index < Modules.Size(); ++Index)
    WriteModule(Writer, Modules.At(Index));
}

void WriteTypes(GenerationWriter &Writer, const ReflectionSnapshot &Snapshot,
                const DocumentationOptions &Options) {
  Writer.Literal("## Types");
  Writer.Break();
  Writer.Break();
  const TypeRecordRange Types = Snapshot.Types();
  if (Types.IsEmpty()) {
    Writer.Literal("None.");
    Writer.Break();
    Writer.Break();
    return;
  }
  for (std::size_t Index = 0; Index < Types.Size(); ++Index) {
    const TypeRecord Type = Types.At(Index);
    const std::string Name(Type.Name());
    Writer.Literal("- ");
    static_cast<void>(
        Writer.Inline(Name, Context(Name, "canonical type name")));
    Writer.Literal(" (");
    Writer.Literal(TypeKindText(Type.Kind()));
    Writer.Literal(")");
    if (Options.IncludesIdentities()) {
      Writer.Literal(" [");
      Writer.Literal(Type.Id().ToString());
      Writer.Literal("]");
    }
    if (const std::string Declaration =
            SymbolText(Snapshot, Type.Declaration());
        !Declaration.empty()) {
      Writer.Literal(" declared by ");
      static_cast<void>(
          Writer.Inline(Declaration, Context(Name, "declaring symbol")));
    }
    Writer.Break();
  }
  Writer.Break();
}

void WriteParameters(GenerationWriter &Writer,
                     const ReflectionSnapshot &Snapshot,
                     const ReflectionRecord &Record, std::string_view Subject) {
  if (Record.ParameterCount() == 0)
    return;
  Writer.Literal("Parameters:");
  Writer.Break();
  for (std::size_t Index = 0; Index < Record.ParameterCount(); ++Index) {
    const ParameterRecord Parameter = Record.Parameter(Index);
    Writer.Literal(Count(Index + 1));
    Writer.Literal(". ");
    static_cast<void>(
        Writer.Inline(Parameter.Name(), Context(Subject, "parameter name")));
    Writer.Literal(": ");
    Writer.Literal(
        TypeText(Snapshot, Parameter.Type(), Parameter.Descriptor()));
    Writer.Literal(" (");
    Writer.Literal(ParameterDispositionText(Parameter.Disposition()));
    Writer.Literal(")");
    if (Parameter.HasDefault()) {
      Writer.Literal(" = ");
      static_cast<void>(Writer.Inline(Parameter.DefaultText(),
                                      Context(Subject, "parameter default")));
    }
    Writer.Break();
    if (!Parameter.Documentation().empty())
      static_cast<void>(
          Writer.Block(Parameter.Documentation(), ProseIndent,
                       Context(Subject, "parameter documentation")));
  }
}

void WriteReturnValues(GenerationWriter &Writer,
                       const ReflectionSnapshot &Snapshot,
                       const ReflectionRecord &Record,
                       std::string_view Subject) {
  if (Record.ReturnCount() == 0)
    return;
  Writer.Literal("Return values:");
  Writer.Break();
  for (std::size_t Index = 0; Index < Record.ReturnCount(); ++Index) {
    const ReturnRecord Return = Record.Return(Index);
    Writer.Literal(Count(Index + 1));
    Writer.Literal(". ");
    if (!Return.Name().empty()) {
      static_cast<void>(Writer.Inline(Return.Name(),
                                      Context(Subject, "returned value name")));
      Writer.Literal(": ");
    }
    Writer.Literal(TypeText(Snapshot, Return.Type(), Return.Descriptor()));
    Writer.Break();
  }
}

void WriteRelations(GenerationWriter &Writer,
                    const ReflectionSnapshot &Snapshot,
                    const ReflectionRecord &Record, std::string_view Subject) {
  if (Record.RelationCount() == 0)
    return;
  Writer.Literal("Relations:");
  Writer.Break();
  for (std::size_t Index = 0; Index < Record.RelationCount(); ++Index) {
    const TypeRelation Relation = Record.Relation(Index);
    Writer.Literal("- ");
    Writer.Literal(TypeRelationKindText(Relation.Kind()));
    Writer.Literal(": ");
    Writer.Literal(TypeText(Snapshot, Relation.Type()));
    if (const std::string Declaration =
            SymbolText(Snapshot, Relation.Declaration());
        !Declaration.empty()) {
      Writer.Literal(" declared by ");
      static_cast<void>(
          Writer.Inline(Declaration, Context(Subject, "related declaration")));
    }
    if (!Relation.Note().empty()) {
      Writer.Literal(" - ");
      static_cast<void>(
          Writer.Inline(Relation.Note(), Context(Subject, "relation note")));
    }
    Writer.Break();
  }
}

void WriteMemberPolicy(GenerationWriter &Writer,
                       const ReflectionSnapshot &Snapshot,
                       const ReflectionRecord &Record,
                       std::string_view Subject) {
  if (!Record.Receiver().IsValid() && Record.AccessPolicy().empty() &&
      Record.Evaluation().empty() && Record.MemberOwnershipPolicy().empty())
    return;
  if (Record.Receiver().IsValid()) {
    static_cast<void>(Writer.Field("Receiver",
                                   TypeText(Snapshot, Record.Receiver()),
                                   Context(Subject, "receiver type")));
    Writer.Literal("Const receiver: ");
    Writer.Literal(Record.ReceiverPermitsConst() ? "yes" : "no");
    Writer.Break();
  }
  if (!Record.AccessPolicy().empty())
    static_cast<void>(Writer.Field("Access", Record.AccessPolicy(),
                                   Context(Subject, "access policy")));
  Writer.Literal("Readable: ");
  Writer.Literal(Record.IsReadable() ? "yes" : "no");
  Writer.Break();
  Writer.Literal("Writable: ");
  Writer.Literal(Record.IsWritable() ? "yes" : "no");
  Writer.Break();
  if (!Record.Evaluation().empty())
    static_cast<void>(Writer.Field("Evaluation", Record.Evaluation(),
                                   Context(Subject, "evaluation policy")));
  if (!Record.MemberOwnershipPolicy().empty())
    static_cast<void>(
        Writer.Field("Member ownership", Record.MemberOwnershipPolicy(),
                     Context(Subject, "member ownership policy")));
}

void WriteSymbol(GenerationWriter &Writer, const ReflectionSnapshot &Snapshot,
                 const ReflectionRecord &Record,
                 const DocumentationOptions &Options) {
  const std::string Subject(Record.QualifiedName());
  Writer.Literal("#### ");
  static_cast<void>(Writer.Inline(Subject, Context(Subject, "qualified name")));
  Writer.Break();

  static_cast<void>(Writer.Field("Kind", SymbolKindText(Record.Kind()),
                                 Context(Subject, "symbol kind")));
  if (!Record.Signature().empty())
    static_cast<void>(Writer.Field("Signature", Record.Signature(),
                                   Context(Subject, "canonical signature")));
  if (Options.IncludesIdentities())
    static_cast<void>(Writer.Field("Identity", Record.Id().ToString(),
                                   Context(Subject, "stable identity")));

  const std::string Scope = Record.Scope().IsRoot()
                                ? std::string(RootScopeText)
                                : SymbolText(Snapshot, Record.Scope().Owner());
  static_cast<void>(
      Writer.Field("Scope", Scope, Context(Subject, "enclosing scope")));

  if (Record.Declaration().IsValid() && Record.Declaration() != Record.Id())
    static_cast<void>(Writer.Field("Declaration",
                                   SymbolText(Snapshot, Record.Declaration()),
                                   Context(Subject, "declaration owner")));
  if (Record.OverloadSet().IsValid())
    static_cast<void>(Writer.Field("Overload set",
                                   SymbolText(Snapshot, Record.OverloadSet()),
                                   Context(Subject, "overload set")));
  if (Record.Type().IsValid())
    static_cast<void>(Writer.Field(
        "Type", TypeText(Snapshot, Record.Type(), Record.Descriptor()),
        Context(Subject, "canonical type")));
  if (DeclaresCallShape(Record.Kind()) || Record.ReturnCount() != 0)
    static_cast<void>(Writer.Field("Returns", ReturnShapeText(Record.Returns()),
                                   Context(Subject, "return shape")));
  if (Record.HasValue())
    static_cast<void>(Writer.Field("Value", Record.ValueText(),
                                   Context(Subject, "canonical value")));
  if (!Record.OwnershipResult().empty())
    static_cast<void>(Writer.Field("Ownership", Record.OwnershipResult(),
                                   Context(Subject, "ownership result")));
  if (!Record.AllocatorPolicy().empty())
    static_cast<void>(Writer.Field("Allocator", Record.AllocatorPolicy(),
                                   Context(Subject, "allocator policy")));
  WriteMemberPolicy(Writer, Snapshot, Record, Subject);
  if (Record.HasModule())
    static_cast<void>(Writer.Field("Module", ModuleKeyText(Record.Module()),
                                   Context(Subject, "module provenance")));

  WriteParameters(Writer, Snapshot, Record, Subject);
  WriteReturnValues(Writer, Snapshot, Record, Subject);
  WriteRelations(Writer, Snapshot, Record, Subject);

  if (Options.IncludesAttributes() && Record.AttributeCount() != 0) {
    Writer.Literal("Attributes:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Record.AttributeCount(); ++Index) {
      const AttributeRecord Attribute = Record.Attribute(Index);
      Writer.Literal("- ");
      static_cast<void>(
          Writer.Inline(Attribute.Name(), Context(Subject, "attribute name")));
      Writer.Literal(": ");
      static_cast<void>(Writer.Inline(Attribute.Value(),
                                      Context(Subject, "attribute value")));
      Writer.Break();
    }
  }

  if (Options.IncludesExamples() && Record.ExampleCount() != 0) {
    Writer.Literal("Examples:");
    Writer.Break();
    for (std::size_t Index = 0; Index < Record.ExampleCount(); ++Index) {
      Writer.Literal(Count(Index + 1));
      Writer.Literal(".");
      Writer.Break();
      static_cast<void>(Writer.Block(Record.Example(Index), ProseIndent,
                                     Context(Subject, "usage example")));
    }
  }

  WriteProse(Writer, "Documentation", Record.Documentation(), Subject,
             "documentation text");
  Writer.Break();
}

// Canonical provenance groups: the declarations no module contributed first,
// then one group per module in the canonical module order of the generation.
[[nodiscard]] std::vector<std::string>
ProvenanceGroups(const ReflectionSnapshot &Snapshot) {
  std::vector<std::string> Groups;
  Groups.emplace_back();
  const ModuleRecordRange Modules = Snapshot.Modules();
  for (std::size_t Index = 0; Index < Modules.Size(); ++Index)
    Groups.push_back(ModuleKeyText(Modules.At(Index)));
  return Groups;
}

[[nodiscard]] bool BelongsToGroup(const ReflectionRecord &Record,
                                  const std::string &Group) {
  if (!Record.HasModule())
    return Group.empty();
  return ModuleKeyText(Record.Module()) == Group;
}

void WriteSymbols(GenerationWriter &Writer, const ReflectionSnapshot &Snapshot,
                  const DocumentationOptions &Options) {
  Writer.Literal("## Symbols");
  Writer.Break();
  Writer.Break();
  const ReflectionRecordRange Symbols = Snapshot.Symbols();
  if (Symbols.IsEmpty()) {
    Writer.Literal("None.");
    Writer.Break();
    return;
  }
  for (const std::string &Group : ProvenanceGroups(Snapshot)) {
    bool WroteHeading = false;
    for (std::size_t Index = 0; Index < Symbols.Size(); ++Index) {
      const ReflectionRecord Record = Symbols.At(Index);
      if (!BelongsToGroup(Record, Group))
        continue;
      if (!WroteHeading) {
        Writer.Literal("### ");
        if (Group.empty())
          Writer.Literal(WithoutModuleText);
        else
          static_cast<void>(
              Writer.Inline(Group, Context(Group, "module provenance")));
        Writer.Break();
        Writer.Break();
        WroteHeading = true;
      }
      WriteSymbol(Writer, Snapshot, Record, Options);
    }
  }
}

} // namespace

GeneratedArtifact GenerateDocumentation(const ReflectionSnapshot &Snapshot,
                                        const DocumentationOptions &Options) {
  GenerationWriter Writer;
  const std::string Title(Options.Title());
  Writer.Literal("# ");
  static_cast<void>(Writer.Inline(Title, Context(Title, "document title")));
  Writer.Break();
  WriteCounts(Writer, Snapshot);
  Writer.Break();

  WriteModules(Writer, Snapshot);
  WriteTypes(Writer, Snapshot, Options);
  WriteSymbols(Writer, Snapshot, Options);

  return Writer.Release(ArtifactName);
}

} // namespace Luna
