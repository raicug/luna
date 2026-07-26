// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Builder = Luna::Detail::ReflectionGenerationBuilder;
using Database = Luna::Detail::ReflectionDatabase;
using RecordFields = Luna::Detail::ReflectionRecordFields;
using Status = Luna::Detail::ReflectionGenerationStatus;
using Hooks = Luna::Detail::StateTestHooks;

// Snapshots are owning values, so they copy, move, and outlive their State.
static_assert(std::is_copy_constructible_v<Luna::ReflectionSnapshot>);
static_assert(std::is_copy_assignable_v<Luna::ReflectionSnapshot>);
static_assert(std::is_default_constructible_v<Luna::ReflectionSnapshot>);
static_assert(std::is_copy_constructible_v<Luna::ReflectionRecord>);
static_assert(std::is_default_constructible_v<Luna::ReflectionRecord>);

[[nodiscard]] Luna::SymbolId Symbol(std::uint8_t Value) {
  Luna::SymbolId::Storage Bytes{};
  Bytes[Luna::SymbolId::ByteCount - 1] = Value;
  return Luna::SymbolId::FromBytes(Bytes);
}

[[nodiscard]] Luna::TypeId TypeOf(std::uint8_t Value) {
  Luna::TypeId::Storage Bytes{};
  Bytes[Luna::TypeId::ByteCount - 1] = Value;
  return Luna::TypeId::FromBytes(Bytes);
}

const Luna::SymbolId NamespaceId = Symbol(1);
const Luna::SymbolId OverloadSetId = Symbol(2);
const Luna::SymbolId CandidateId = Symbol(3);
const Luna::SymbolId ModuleSymbolId = Symbol(4);
const Luna::SymbolId ConstantId = Symbol(5);

[[nodiscard]] Luna::TypeDescriptor NumberDescriptor() {
  return Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
}

[[nodiscard]] Luna::TypeDescriptor TextDescriptor() {
  return Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::String);
}

[[nodiscard]] RecordFields NamespaceRecord() {
  RecordFields Fields;
  Fields.Kind = Luna::SymbolKind::Namespace;
  Fields.Id = NamespaceId;
  Fields.Name = "Studio";
  Fields.QualifiedName = "Studio";
  Fields.Documentation = "Studio root namespace.";
  return Fields;
}

[[nodiscard]] RecordFields OverloadSetRecord() {
  RecordFields Fields;
  Fields.Kind = Luna::SymbolKind::OverloadSet;
  Fields.Id = OverloadSetId;
  Fields.Name = "Find";
  Fields.QualifiedName = "Studio.Find";
  Fields.Scope = Luna::ScopeId(NamespaceId);
  return Fields;
}

[[nodiscard]] RecordFields CandidateRecord() {
  RecordFields Fields;
  Fields.Kind = Luna::SymbolKind::FunctionCandidate;
  Fields.Id = CandidateId;
  Fields.Name = "Find";
  Fields.QualifiedName = "Studio.Find";
  Fields.Signature = "number(string,number)";
  Fields.Scope = Luna::ScopeId(OverloadSetId);
  Fields.OverloadSet = OverloadSetId;
  Fields.Returns = Luna::ReturnShape::Scalar;

  Luna::Detail::ReflectionParameterFields Text;
  Text.Name = "Query";
  Text.Type = TypeOf(2);
  Text.Descriptor = TextDescriptor();
  Text.Documentation = "Search text.";
  Fields.Parameters.push_back(std::move(Text));

  Luna::Detail::ReflectionParameterFields Limit;
  Limit.Name = "Limit";
  Limit.Type = TypeOf(1);
  Limit.Descriptor = NumberDescriptor();
  Limit.Disposition = Luna::ParameterDisposition::Defaulted;
  Limit.HasDefault = true;
  Limit.DefaultText = "8";
  Fields.Parameters.push_back(std::move(Limit));

  Luna::Detail::ReflectionReturnFields Result;
  Result.Name = "Match";
  Result.Type = TypeOf(1);
  Result.Descriptor = NumberDescriptor();
  Fields.ReturnValues.push_back(std::move(Result));

  Luna::Detail::ReflectionRelationFields Relation;
  Relation.Kind = Luna::TypeRelationKind::Operand;
  Relation.Type = TypeOf(2);
  Relation.Declaration = OverloadSetId;
  Relation.Note = "query operand";
  Fields.Relations.push_back(std::move(Relation));
  return Fields;
}

[[nodiscard]] RecordFields ModuleRecordFields() {
  RecordFields Fields;
  Fields.Kind = Luna::SymbolKind::Module;
  Fields.Id = ModuleSymbolId;
  Fields.Name = "Physics";
  Fields.QualifiedName = "Physics";
  return Fields;
}

[[nodiscard]] RecordFields ConstantRecord() {
  RecordFields Fields;
  Fields.Kind = Luna::SymbolKind::Constant;
  Fields.Id = ConstantId;
  Fields.Name = "Gravity";
  Fields.QualifiedName = "Studio.Gravity";
  Fields.Scope = Luna::ScopeId(NamespaceId);
  Fields.Type = TypeOf(1);
  Fields.Descriptor = NumberDescriptor();
  Fields.Documentation = "Gravity constant.";
  Fields.Examples.push_back("print(Studio.Gravity)");
  Fields.Attributes.push_back({"Unit", "m/s^2"});
  Fields.Module = 0;
  return Fields;
}

[[nodiscard]] Luna::Detail::ReflectionModuleFields PhysicsModule() {
  Luna::Detail::ReflectionModuleFields Fields;
  Fields.Identity = "studio.physics";
  Fields.Version = "1.2.0";
  Fields.Symbol = ModuleSymbolId;
  Fields.Documentation = "Physics module.";
  return Fields;
}

[[nodiscard]] Luna::Detail::ReflectionTypeFields NumberType() {
  Luna::Detail::ReflectionTypeFields Fields;
  Fields.Id = TypeOf(1);
  Fields.Name = "number";
  Fields.Descriptor = NumberDescriptor();
  return Fields;
}

[[nodiscard]] Luna::Detail::ReflectionTypeFields TextType() {
  Luna::Detail::ReflectionTypeFields Fields;
  Fields.Id = TypeOf(2);
  Fields.Name = "string";
  Fields.Descriptor = TextDescriptor();
  return Fields;
}

// The same logical generation, assembled either in canonical order or in
// reverse, so ordering can never depend on submission order.
[[nodiscard]] Builder Baseline(bool Reversed = false) {
  Builder Candidate;
  if (Reversed) {
    Candidate.AddType(TextType());
    Candidate.AddType(NumberType());
    Candidate.AddModule(PhysicsModule());
    Candidate.AddRecord(ConstantRecord());
    Candidate.AddRecord(ModuleRecordFields());
    Candidate.AddRecord(CandidateRecord());
    Candidate.AddRecord(OverloadSetRecord());
    Candidate.AddRecord(NamespaceRecord());
    return Candidate;
  }
  Candidate.AddModule(PhysicsModule());
  Candidate.AddType(NumberType());
  Candidate.AddType(TextType());
  Candidate.AddRecord(NamespaceRecord());
  Candidate.AddRecord(OverloadSetRecord());
  Candidate.AddRecord(CandidateRecord());
  Candidate.AddRecord(ModuleRecordFields());
  Candidate.AddRecord(ConstantRecord());
  return Candidate;
}

[[nodiscard]] std::vector<std::string>
OrderedQualifiedNames(const Luna::ReflectionSnapshot &Snapshot) {
  std::vector<std::string> Names;
  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
  for (std::size_t Index = 0; Index < Symbols.Size(); ++Index) {
    const Luna::ReflectionRecord Record = Symbols.At(Index);
    Names.push_back(std::string(Record.QualifiedName()) + ":" +
                    std::string(Luna::SymbolKindText(Record.Kind())) + ":" +
                    std::string(Record.Signature()) + ":" +
                    Record.Id().ToString());
  }
  return Names;
}

[[nodiscard]] bool VerifyEmptyGeneration() {
  Database Empty;
  if (Empty.Generation() != 0 || Empty.Count() != 0)
    return false;

  const Luna::ReflectionSnapshot Snapshot = Empty.Snapshot();
  if (!Snapshot.IsEmpty() || Snapshot.Size() != 0 || Snapshot.Generation() != 0)
    return false;
  if (Snapshot.Find(NamespaceId).IsValid() ||
      Snapshot.Find("Studio").IsValid() ||
      Snapshot.FindType(TypeOf(1)).IsValid())
    return false;
  if (!Snapshot.Symbols().IsEmpty() || !Snapshot.Types().IsEmpty() ||
      !Snapshot.Modules().IsEmpty())
    return false;
  if (!Snapshot.Symbols(Luna::ScopeId::Root()).IsEmpty() ||
      !Snapshot.Symbols(Luna::SymbolKind::Namespace).IsEmpty())
    return false;

  // A default snapshot and every view obtained from it answer
  // deterministically.
  const Luna::ReflectionSnapshot Default;
  if (!Default.IsEmpty() || Default.Generation() != 0)
    return false;
  const Luna::ReflectionRecord Missing = Default.Find("Studio");
  if (Missing.IsValid() || !Missing.Name().empty() ||
      Missing.ParameterCount() != 0 || Missing.HasModule() ||
      Missing.Parameter(0).IsValid() || Missing.Module().IsValid() ||
      Missing.Kind() != Luna::SymbolKind::Namespace)
    return false;
  return !Default.Symbols().At(0).IsValid();
}

[[nodiscard]] bool VerifyPublishedLookupAndOrdering() {
  Database Reflection;
  if (Reflection.PublishGeneration(Baseline()) != Status::Valid)
    return false;
  if (Reflection.Generation() != 1 || Reflection.Count() != 5)
    return false;

  const Luna::ReflectionSnapshot Snapshot = Reflection.Snapshot();
  if (Snapshot.Generation() != 1 || Snapshot.Size() != 5)
    return false;

  // Lookup by identity and by qualified name reaches the same record.
  const Luna::ReflectionRecord Constant = Snapshot.Find(ConstantId);
  if (!Constant.IsValid() || Constant.QualifiedName() != "Studio.Gravity")
    return false;
  if (Snapshot.Find("Studio.Gravity").Id() != ConstantId)
    return false;
  if (Snapshot.Find(Symbol(99)).IsValid() ||
      Snapshot.Find("Studio.Missing").IsValid())
    return false;

  // Canonical order puts an overload set ahead of its candidates, so a
  // qualified-name lookup resolves to the set.
  const Luna::ReflectionRecord Found = Snapshot.Find("Studio.Find");
  if (Found.Kind() != Luna::SymbolKind::OverloadSet ||
      Found.Id() != OverloadSetId)
    return false;

  const std::vector<std::string> Expected{
      "Physics:module::" + ModuleSymbolId.ToString(),
      "Studio:namespace::" + NamespaceId.ToString(),
      "Studio.Find:overload_set::" + OverloadSetId.ToString(),
      "Studio.Find:function_candidate:number(string,number):" +
          CandidateId.ToString(),
      "Studio.Gravity:constant::" + ConstantId.ToString(),
  };
  if (OrderedQualifiedNames(Snapshot) != Expected)
    return false;

  // The same logical generation submitted in reverse order is byte-for-byte
  // identical after canonical sorting.
  Database Reversed;
  if (Reversed.PublishGeneration(Baseline(true)) != Status::Valid)
    return false;
  if (OrderedQualifiedNames(Reversed.Snapshot()) != Expected)
    return false;

  // Enumeration by scope and by kind uses the same canonical order.
  const Luna::ReflectionRecordRange Root =
      Snapshot.Symbols(Luna::ScopeId::Root());
  if (Root.Size() != 2 || Root.At(0).QualifiedName() != "Physics" ||
      Root.At(1).QualifiedName() != "Studio")
    return false;
  const Luna::ReflectionRecordRange Nested =
      Snapshot.Symbols(Luna::ScopeId(NamespaceId));
  if (Nested.Size() != 2 || Nested.At(0).Id() != OverloadSetId ||
      Nested.At(1).Id() != ConstantId)
    return false;
  if (Snapshot.Symbols(Luna::ScopeId(OverloadSetId)).Size() != 1)
    return false;
  if (!Snapshot.Symbols(Luna::ScopeId(Symbol(99))).IsEmpty())
    return false;
  if (Snapshot.Symbols(Luna::SymbolKind::FunctionCandidate).Size() != 1 ||
      Snapshot.Symbols(Luna::SymbolKind::Namespace).At(0).Id() != NamespaceId ||
      !Snapshot.Symbols(Luna::SymbolKind::Class).IsEmpty())
    return false;
  if (Snapshot.Symbols().At(Snapshot.Size()).IsValid())
    return false;

  // Types and modules enumerate canonically and resolve by identity.
  const Luna::TypeRecordRange Types = Snapshot.Types();
  if (Types.Size() != 2 || Types.At(0).Name() != "number" ||
      Types.At(1).Name() != "string")
    return false;
  if (Types.At(0).Kind() != Luna::TypeKind::Fixed ||
      Types.At(0).Descriptor() != NumberDescriptor())
    return false;
  if (Snapshot.FindType(TypeOf(2)).Name() != "string" ||
      Snapshot.FindType(TypeOf(9)).IsValid())
    return false;
  const Luna::ModuleRecordRange Modules = Snapshot.Modules();
  if (Modules.Size() != 1 || Modules.At(0).Identity() != "studio.physics" ||
      Modules.At(0).Version() != "1.2.0" ||
      Modules.At(0).Symbol() != ModuleSymbolId)
    return false;
  return true;
}

[[nodiscard]] bool VerifyRecordMetadata() {
  Database Reflection;
  if (Reflection.PublishGeneration(Baseline(true)) != Status::Valid)
    return false;
  const Luna::ReflectionSnapshot Snapshot = Reflection.Snapshot();

  const Luna::ReflectionRecord Candidate = Snapshot.Find(CandidateId);
  if (!Candidate.IsValid() || Candidate.Name() != "Find" ||
      Candidate.Signature() != "number(string,number)")
    return false;
  if (Candidate.Scope() != Luna::ScopeId(OverloadSetId) ||
      Candidate.Scope().IsRoot() || Candidate.OverloadSet() != OverloadSetId)
    return false;
  // A record without an explicit declaration owner declares itself.
  if (Candidate.Declaration() != CandidateId)
    return false;
  if (Candidate.Returns() != Luna::ReturnShape::Scalar ||
      Candidate.ReturnCount() != 1)
    return false;
  if (Candidate.Return(0).Name() != "Match" ||
      Candidate.Return(0).Type() != TypeOf(1) ||
      Candidate.Return(0).Descriptor() != NumberDescriptor())
    return false;
  if (Candidate.Return(1).IsValid())
    return false;

  if (Candidate.ParameterCount() != 2)
    return false;
  const Luna::ParameterRecord Query = Candidate.Parameter(0);
  if (Query.Name() != "Query" || Query.Type() != TypeOf(2) ||
      Query.Disposition() != Luna::ParameterDisposition::Required ||
      Query.HasDefault() || !Query.DefaultText().empty() ||
      Query.Documentation() != "Search text.")
    return false;
  const Luna::ParameterRecord Limit = Candidate.Parameter(1);
  if (Limit.Disposition() != Luna::ParameterDisposition::Defaulted ||
      !Limit.HasDefault() || Limit.DefaultText() != "8" ||
      Limit.Descriptor() != NumberDescriptor())
    return false;
  if (Candidate.Parameter(2).IsValid())
    return false;

  if (Candidate.RelationCount() != 1)
    return false;
  const Luna::TypeRelation Relation = Candidate.Relation(0);
  if (Relation.Kind() != Luna::TypeRelationKind::Operand ||
      Relation.Type() != TypeOf(2) || Relation.Declaration() != OverloadSetId ||
      Relation.Note() != "query operand")
    return false;

  const Luna::ReflectionRecord Constant = Snapshot.Find("Studio.Gravity");
  if (Constant.Type() != TypeOf(1) ||
      Constant.Descriptor() != NumberDescriptor())
    return false;
  if (Constant.Documentation() != "Gravity constant." ||
      Constant.ExampleCount() != 1 ||
      Constant.Example(0) != "print(Studio.Gravity)" ||
      !Constant.Example(1).empty())
    return false;
  if (Constant.AttributeCount() != 1 ||
      Constant.Attribute(0).Name() != "Unit" ||
      Constant.Attribute(0).Value() != "m/s^2" ||
      Constant.Attribute(1).IsValid())
    return false;
  if (!Constant.HasModule() || Constant.Module().Identity() != "studio.physics")
    return false;
  const Luna::ReflectionRecord Namespace = Snapshot.Find(NamespaceId);
  if (Namespace.HasModule() || Namespace.Module().IsValid() ||
      Namespace.Returns() != Luna::ReturnShape::Zero)
    return false;

  // Text and views obtained from a record keep reading the same generation
  // after the snapshot value that produced them is gone.
  const Luna::ParameterRecord Retained =
      Snapshot.Find(CandidateId).Parameter(1);
  const Luna::ModuleRecord RetainedModule = Snapshot.Modules().At(0);
  if (Reflection.PublishGeneration(Builder()) != Status::Valid ||
      Reflection.Count() != 0)
    return false;
  return Retained.DefaultText() == "8" &&
         RetainedModule.Identity() == "studio.physics" && Snapshot.Size() == 5;
}

[[nodiscard]] bool VerifyRejectionsPreserveCommittedGeneration() {
  Database Reflection;
  if (Reflection.PublishGeneration(Baseline()) != Status::Valid)
    return false;

  const auto Reject = [&Reflection](const Builder &Candidate, Status Expected) {
    std::shared_ptr<const Luna::Detail::ReflectionStorage> Prepared;
    if (Reflection.Prepare(Candidate, Prepared) != Expected || Prepared)
      return false;
    if (Reflection.PublishGeneration(Candidate) != Expected)
      return false;
    // A rejected candidate publishes nothing at all.
    return Reflection.Generation() == 1 && Reflection.Count() == 5;
  };

  {
    Builder Candidate;
    RecordFields Fields = NamespaceRecord();
    Fields.Id = Luna::SymbolId();
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InvalidIdentity))
      return false;
  }
  {
    Builder Candidate;
    RecordFields Fields = NamespaceRecord();
    Fields.QualifiedName = "Other.Root";
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::IncompleteMetadata))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(OverloadSetRecord());
    RecordFields Fields = CandidateRecord();
    Fields.Signature.clear();
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::IncompleteMetadata))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    RecordFields Fields = ConstantRecord();
    Fields.Id = NamespaceId;
    Fields.Module.reset();
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::DuplicateIdentity))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    RecordFields Fields = NamespaceRecord();
    Fields.Id = Symbol(42);
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::DuplicateQualifiedName))
      return false;
  }
  {
    Builder Candidate;
    RecordFields Fields = ConstantRecord();
    Fields.Module.reset();
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentScope))
      return false;
  }
  {
    // A constant is not a scope, so it can never own nested symbols.
    Builder Candidate;
    RecordFields Owner = ConstantRecord();
    Owner.Module.reset();
    Owner.Scope = Luna::ScopeId();
    Owner.QualifiedName = "Gravity";
    Candidate.AddRecord(std::move(Owner));
    RecordFields Nested = NamespaceRecord();
    Nested.Scope = Luna::ScopeId(ConstantId);
    Nested.QualifiedName = "Gravity.Studio";
    Candidate.AddRecord(std::move(Nested));
    if (!Reject(std::move(Candidate), Status::InconsistentScope))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(ConstantRecord());
    if (!Reject(std::move(Candidate), Status::InconsistentModule))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(ModuleRecordFields());
    Candidate.AddRecord(ConstantRecord());
    Luna::Detail::ReflectionModuleFields Module = PhysicsModule();
    Module.Version.clear();
    Candidate.AddModule(std::move(Module));
    if (!Reject(std::move(Candidate), Status::IncompleteMetadata))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(OverloadSetRecord());
    RecordFields Fields = CandidateRecord();
    Fields.Parameters[1].Disposition = Luna::ParameterDisposition::Required;
    Fields.Parameters[1].HasDefault = false;
    Fields.Parameters[0].Disposition = Luna::ParameterDisposition::Optional;
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentParameters))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(OverloadSetRecord());
    RecordFields Fields = CandidateRecord();
    Fields.Parameters[0].Disposition = Luna::ParameterDisposition::Variadic;
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentParameters))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(OverloadSetRecord());
    RecordFields Fields = CandidateRecord();
    Fields.Parameters[1].HasDefault = false;
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentParameters))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddRecord(OverloadSetRecord());
    RecordFields Fields = CandidateRecord();
    Fields.ReturnValues.clear();
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentReturns))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    RecordFields Fields = ConstantRecord();
    Fields.Module.reset();
    Fields.Parameters.push_back({});
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::IncompleteMetadata))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Luna::Detail::ReflectionTypeFields Fields = NumberType();
    Fields.Descriptor = Luna::TypeDescriptor::Unsupported();
    Candidate.AddType(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentType))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    Candidate.AddType(NumberType());
    Candidate.AddType(NumberType());
    if (!Reject(std::move(Candidate), Status::DuplicateIdentity))
      return false;
  }
  {
    Builder Candidate;
    Candidate.AddRecord(NamespaceRecord());
    RecordFields Fields = CandidateRecord();
    Fields.Scope = Luna::ScopeId(NamespaceId);
    Fields.OverloadSet = Symbol(77);
    Candidate.AddRecord(std::move(Fields));
    if (!Reject(std::move(Candidate), Status::InconsistentOverloadSet))
      return false;
  }

  // The committed generation still answers exactly as before.
  const Luna::ReflectionSnapshot Snapshot = Reflection.Snapshot();
  return Snapshot.Generation() == 1 && Snapshot.Size() == 5 &&
         Snapshot.Find("Studio.Gravity").IsValid();
}

[[nodiscard]] bool VerifySnapshotsRetainTheirGeneration() {
  Database Reflection;
  if (Reflection.PublishGeneration(Baseline()) != Status::Valid)
    return false;
  const Luna::ReflectionSnapshot First = Reflection.Snapshot();

  Builder Extended = Reflection.BeginGeneration();
  if (Extended.RecordCount() != 5 || Extended.TypeCount() != 2 ||
      Extended.ModuleCount() != 1)
    return false;
  RecordFields Added = OverloadSetRecord();
  Added.Id = Symbol(6);
  Added.Name = "Reset";
  Added.QualifiedName = "Studio.Reset";
  Extended.AddRecord(std::move(Added));
  if (Reflection.PublishGeneration(Extended) != Status::Valid)
    return false;

  const Luna::ReflectionSnapshot Second = Reflection.Snapshot();
  if (Second.Generation() != 2 || Second.Size() != 6 ||
      !Second.Find("Studio.Reset").IsValid())
    return false;
  // The earlier snapshot never observes the later generation.
  if (First.Generation() != 1 || First.Size() != 5 ||
      First.Find("Studio.Reset").IsValid())
    return false;
  return true;
}

[[nodiscard]] bool VerifyStateOwnershipAndSurvival() {
  Luna::ReflectionSnapshot Retained;
  Luna::ReflectionSnapshot Later;
  {
    Luna::State First;
    Luna::State Second;
    Database *FirstReflection = Hooks::ReflectionDatabaseOf(First);
    Database *SecondReflection = Hooks::ReflectionDatabaseOf(Second);
    if (!FirstReflection || !SecondReflection ||
        FirstReflection == SecondReflection)
      return false;
    if (First.Bindings().Reflection().Size() != 0 ||
        Hooks::ReflectionGeneration(First) != 0)
      return false;

    if (FirstReflection->PublishGeneration(Baseline()) != Status::Valid)
      return false;
    Retained = First.Bindings().Reflection();
    if (Retained.Generation() != 1 || Retained.Size() != 5)
      return false;
    // Each State owns one isolated database.
    if (!Second.Bindings().Reflection().IsEmpty() ||
        Hooks::ReflectionGeneration(Second) != 0)
      return false;

    Builder Extended = FirstReflection->BeginGeneration();
    RecordFields Added = OverloadSetRecord();
    Added.Id = Symbol(7);
    Added.Name = "Clear";
    Added.QualifiedName = "Studio.Clear";
    Extended.AddRecord(std::move(Added));
    if (FirstReflection->PublishGeneration(Extended) != Status::Valid)
      return false;
    if (Retained.Generation() != 1 || Retained.Size() != 5)
      return false;

    // A move transfers the exact current generation and keeps prior snapshots
    // valid, while the moved-from State reports an empty model.
    Luna::State Moved = std::move(First);
    if (Moved.Bindings().Reflection().Generation() != 2 ||
        Moved.Bindings().Reflection().Size() != 6)
      return false;
    if (!First.Bindings().Reflection().IsEmpty() ||
        Hooks::ReflectionGeneration(First) != 0 || First.IsReady())
      return false;
    Later = Moved.Bindings().Reflection();
    if (Retained.Generation() != 1)
      return false;
  }

  // Both snapshots survive destruction of every originating State.
  if (Retained.Generation() != 1 || Retained.Size() != 5)
    return false;
  if (Retained.Find("Studio.Gravity").Documentation() != "Gravity constant.")
    return false;
  if (Later.Generation() != 2 || Later.Size() != 6 ||
      !Later.Find("Studio.Clear").IsValid())
    return false;

  // An owning snapshot holds no VM or mutable State storage, so another thread
  // may read it after acquisition.
  bool CrossThreadMatches = false;
  std::thread Reader([&Retained, &CrossThreadMatches] {
    const Luna::ReflectionRecord Record = Retained.Find("Studio.Find");
    CrossThreadMatches = Retained.Generation() == 1 && Retained.Size() == 5 &&
                         Record.Kind() == Luna::SymbolKind::OverloadSet &&
                         Retained.Symbols().Size() == 5;
  });
  Reader.join();
  return CrossThreadMatches;
}

// Requirements 3.5, 3.9, 19.8: a rejected identity or reflection preparation
// publishes nothing and leaves the State fully usable through the real VM.
[[nodiscard]] bool VerifyFailedPreparationKeepsStateReusable() {
  Luna::State Owner;
  if (!Owner.IsReady())
    return false;
  if (!Owner.Bindings()
           .Register("Doubler", [](int Value) { return Value * 2; })
           .IsSuccess())
    return false;
  if (!Owner.Execute("First = Doubler(21)").IsSuccess() ||
      Hooks::ObserveIntegerGlobal(Owner, "First") != 42)
    return false;

  // The committed generation is exactly the one the callable's own publication
  // produced: its overload set and its one candidate.
  Database *Reflection = Hooks::ReflectionDatabaseOf(Owner);
  if (Reflection == nullptr || Reflection->Count() != 2)
    return false;
  const Luna::ReflectionSnapshot Committed = Owner.Bindings().Reflection();
  const std::vector<std::string> CommittedOrder =
      OrderedQualifiedNames(Committed);
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  if (!EntryDepth || Committed.Generation() != 1)
    return false;

  // A rejected identity resolution stores no descriptor and no fallback
  // identity, so the next resolution still sees the untouched registry.
  Luna::Detail::TypeIdentityRegistry Types;
  const auto Incomplete = Types.Resolve(Luna::TypeDescriptor::Unsupported());
  if (Incomplete.IsSuccess() || !Incomplete.Diagnostic || Types.Size() != 0)
    return false;
  const auto Accepted = Types.Resolve(NumberDescriptor());
  if (!Accepted.IsSuccess() || Types.Size() != 1)
    return false;
  Types.CollisionInjection().InjectIdentity(Accepted.Value->Bytes());
  const auto Collided = Types.Resolve(TextDescriptor());
  if (Collided.IsSuccess() || !Collided.Diagnostic || Types.Size() != 1)
    return false;
  const Luna::TypeDescriptor *Stored = Types.Find(*Accepted.Value);
  if (Stored == nullptr || *Stored != NumberDescriptor())
    return false;

  // Every rejected reflection preparation leaves the committed generation and
  // its canonical order exactly as they were.
  const auto RejectThrough = [&](const Builder &Candidate, Status Expected) {
    std::shared_ptr<const Luna::Detail::ReflectionStorage> Prepared;
    if (Reflection->Prepare(Candidate, Prepared) != Expected || Prepared)
      return false;
    if (Reflection->PublishGeneration(Candidate) != Expected)
      return false;
    const Luna::ReflectionSnapshot After = Owner.Bindings().Reflection();
    return Hooks::ReflectionGeneration(Owner) == 1 && After.Generation() == 1 &&
           After.Size() == Committed.Size() &&
           OrderedQualifiedNames(After) == CommittedOrder;
  };

  {
    Builder Candidate;
    RecordFields Fields = NamespaceRecord();
    Fields.Id = Luna::SymbolId();
    Candidate.AddRecord(std::move(Fields));
    if (!RejectThrough(std::move(Candidate), Status::InvalidIdentity))
      return false;
  }
  {
    // A second overload set for a qualified name the committed generation
    // already describes is a duplicate, not a second reflected set.
    Builder Candidate = Reflection->BeginGeneration();
    RecordFields Fields;
    Fields.Kind = Luna::SymbolKind::OverloadSet;
    Fields.Id = Symbol(21);
    Fields.Name = "Doubler";
    Fields.QualifiedName = "Doubler";
    Candidate.AddRecord(std::move(Fields));
    if (!RejectThrough(std::move(Candidate), Status::DuplicateQualifiedName))
      return false;
  }
  {
    Builder Candidate = Reflection->BeginGeneration();
    Luna::Detail::ReflectionTypeFields Fields = TextType();
    Fields.Id = TypeOf(3);
    Fields.Descriptor = Luna::TypeDescriptor::Unsupported();
    Candidate.AddType(std::move(Fields));
    if (!RejectThrough(std::move(Candidate), Status::InconsistentType))
      return false;
  }

  // The State is still ready, still holds its original binding, and still
  // accepts new registrations and executions through the real compiler and VM.
  if (!Owner.IsReady() || Hooks::ObserveRootStackDepth(Owner) != *EntryDepth ||
      Hooks::BindingCount(Owner) != 1 ||
      !Hooks::BindingIsCommitted(Owner, "Doubler"))
    return false;
  if (!Owner.Bindings()
           .Register("Tripler", [](int Value) { return Value * 3; })
           .IsSuccess())
    return false;
  if (!Owner.Execute("Second = Tripler(Doubler(2))").IsSuccess() ||
      Hooks::ObserveIntegerGlobal(Owner, "Second") != 12)
    return false;
  if (Hooks::ObserveRootStackDepth(Owner) != *EntryDepth)
    return false;

  // The database itself is reusable: a later registration publishes its own
  // generation normally while the retained snapshot keeps its own.
  if (!Owner.Bindings()
           .Register("Scale", [](int Value) { return Value + 1; })
           .IsSuccess())
    return false;
  const Luna::ReflectionSnapshot Republished = Owner.Bindings().Reflection();
  return Republished.Generation() == 3 && Republished.Find("Scale").IsValid() &&
         Republished.Find("Scale").Kind() == Luna::SymbolKind::OverloadSet &&
         Committed.Generation() == 1 &&
         OrderedQualifiedNames(Committed) == CommittedOrder;
}

} // namespace

int RunReflectionDatabaseTests() {
  if (!VerifyEmptyGeneration())
    return 1;
  if (!VerifyPublishedLookupAndOrdering())
    return 2;
  if (!VerifyRecordMetadata())
    return 3;
  if (!VerifyRejectionsPreserveCommittedGeneration())
    return 4;
  if (!VerifySnapshotsRetainTheirGeneration())
    return 5;
  if (!VerifyStateOwnershipAndSurvival())
    return 6;
  if (!VerifyFailedPreparationKeepsStateReusable())
    return 7;
  return 0;
}
