// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Builder = Luna::Detail::ReflectionGenerationBuilder;
using Database = Luna::Detail::ReflectionDatabase;
using Hooks = Luna::Detail::StateTestHooks;
using RecordFields = Luna::Detail::ReflectionRecordFields;
using Status = Luna::Detail::ReflectionGenerationStatus;
using Storage = Luna::Detail::ReflectionStorage;

constexpr std::array AllSymbolKinds{
    Luna::SymbolKind::Namespace,    Luna::SymbolKind::Module,
    Luna::SymbolKind::OverloadSet,  Luna::SymbolKind::FunctionCandidate,
    Luna::SymbolKind::Constant,     Luna::SymbolKind::Enumeration,
    Luna::SymbolKind::Enumerator,   Luna::SymbolKind::EnumeratorAlias,
    Luna::SymbolKind::Class,        Luna::SymbolKind::Constructor,
    Luna::SymbolKind::Factory,      Luna::SymbolKind::Method,
    Luna::SymbolKind::StaticMethod, Luna::SymbolKind::Property,
    Luna::SymbolKind::Field,        Luna::SymbolKind::Operator,
    Luna::SymbolKind::Type};

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

[[nodiscard]] Luna::SymbolId Symbol(std::size_t Ordinal) {
  Luna::SymbolId::Storage Bytes{};
  Bytes[Luna::SymbolId::ByteCount - 1] = static_cast<std::uint8_t>(Ordinal);
  Bytes[Luna::SymbolId::ByteCount - 2] = 0x5aU;
  return Luna::SymbolId::FromBytes(Bytes);
}

[[nodiscard]] Luna::TypeId TypeOf(std::size_t Ordinal) {
  Luna::TypeId::Storage Bytes{};
  Bytes[Luna::TypeId::ByteCount - 1] = static_cast<std::uint8_t>(Ordinal);
  Bytes[Luna::TypeId::ByteCount - 2] = 0xa5U;
  return Luna::TypeId::FromBytes(Bytes);
}

[[nodiscard]] Luna::TypeDescriptor NumberDescriptor() {
  return Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
}

[[nodiscard]] std::string Ordinal(std::size_t Value) {
  std::string Text = std::to_string(Value);
  return Text.size() < 2 ? "0" + Text : Text;
}

struct ModelParameter final {
  std::string Name;
  Luna::ParameterDisposition Disposition = Luna::ParameterDisposition::Required;
  bool HasDefault = false;
  std::string DefaultText;
};

struct ModelRecord final {
  Luna::SymbolKind Kind = Luna::SymbolKind::Namespace;
  Luna::SymbolId Id;
  std::string Name;
  std::string QualifiedName;
  std::string Signature;
  std::string Documentation;
  Luna::ScopeId Scope;
  Luna::SymbolId OverloadSet;
  Luna::ReturnShape Returns = Luna::ReturnShape::Zero;
  std::size_t ReturnCount = 0;
  std::vector<ModelParameter> Parameters;
  std::optional<std::string> ModuleIdentity;
};

struct ModelType final {
  Luna::TypeId Id;
  std::string Name;
};

struct ModelModule final {
  std::string Identity;
  std::string Version;
  Luna::SymbolId Symbol;
};

struct ModelGeneration final {
  std::uint64_t Generation = 0;
  std::vector<ModelRecord> Records;
  std::vector<ModelType> Types;
  std::vector<ModelModule> Modules;
};

[[nodiscard]] std::vector<ModelRecord>
CanonicalRecords(std::vector<ModelRecord> Records) {
  std::sort(Records.begin(), Records.end(),
            [](const ModelRecord &Left, const ModelRecord &Right) {
              if (Left.QualifiedName != Right.QualifiedName)
                return Left.QualifiedName < Right.QualifiedName;
              if (Left.Kind != Right.Kind)
                return static_cast<int>(Left.Kind) <
                       static_cast<int>(Right.Kind);
              if (Left.Signature != Right.Signature)
                return Left.Signature < Right.Signature;
              return Left.Id < Right.Id;
            });
  return Records;
}

[[nodiscard]] std::vector<ModelType>
CanonicalTypes(std::vector<ModelType> Types) {
  std::sort(Types.begin(), Types.end(),
            [](const ModelType &Left, const ModelType &Right) {
              if (Left.Name != Right.Name)
                return Left.Name < Right.Name;
              return Left.Id < Right.Id;
            });
  return Types;
}

[[nodiscard]] std::vector<ModelModule>
CanonicalModules(std::vector<ModelModule> Modules) {
  std::sort(Modules.begin(), Modules.end(),
            [](const ModelModule &Left, const ModelModule &Right) {
              if (Left.Identity != Right.Identity)
                return Left.Identity < Right.Identity;
              if (Left.Version != Right.Version)
                return Left.Version < Right.Version;
              return Left.Symbol < Right.Symbol;
            });
  return Modules;
}

void AppendSymbol(ByteCursor &Cursor, Builder &Candidate,
                  ModelGeneration &Model, std::size_t &SymbolCounter,
                  std::size_t &TypeCounter) {
  std::vector<std::size_t> Scopes;
  std::vector<std::size_t> Sets;
  for (std::size_t Index = 0; Index < Model.Records.size(); ++Index) {
    const Luna::SymbolKind Kind = Model.Records[Index].Kind;
    if (Kind == Luna::SymbolKind::Namespace || Kind == Luna::SymbolKind::Module)
      Scopes.push_back(Index);
    if (Kind == Luna::SymbolKind::OverloadSet)
      Sets.push_back(Index);
  }

  const std::size_t Position = ++SymbolCounter;
  const std::size_t Choice = Cursor.Pick(10);

  if (Choice >= 8 && !Sets.empty()) {
    const ModelRecord &Set = Model.Records[Sets[Cursor.Pick(Sets.size())]];
    RecordFields Fields;
    Fields.Kind = Luna::SymbolKind::FunctionCandidate;
    Fields.Id = Symbol(Position);
    Fields.Name = Set.Name;
    Fields.QualifiedName = Set.QualifiedName;
    Fields.Signature = "number(a" + Ordinal(Position) + ")";
    Fields.Scope = Luna::ScopeId(Set.Id);
    Fields.OverloadSet = Set.Id;
    Fields.Returns = Luna::ReturnShape::Scalar;

    ModelRecord Expected;
    Expected.Kind = Fields.Kind;
    Expected.Id = Fields.Id;
    Expected.Name = Fields.Name;
    Expected.QualifiedName = Fields.QualifiedName;
    Expected.Signature = Fields.Signature;
    Expected.Scope = Fields.Scope;
    Expected.OverloadSet = Fields.OverloadSet;
    Expected.Returns = Luna::ReturnShape::Scalar;
    Expected.ReturnCount = 1;

    Luna::Detail::ReflectionParameterFields Argument;
    Argument.Name = "Arg" + Ordinal(Position);
    Argument.Type = TypeOf(1);
    Argument.Descriptor = NumberDescriptor();
    Fields.Parameters.push_back(Argument);
    Expected.Parameters.push_back({Argument.Name,
                                   Luna::ParameterDisposition::Required, false,
                                   std::string()});

    if (Cursor.Pick(2) == 0) {
      Luna::Detail::ReflectionParameterFields Limit;
      Limit.Name = "Limit";
      Limit.Type = TypeOf(1);
      Limit.Descriptor = NumberDescriptor();
      Limit.Disposition = Luna::ParameterDisposition::Defaulted;
      Limit.HasDefault = true;
      Limit.DefaultText = Ordinal(Position);
      Fields.Parameters.push_back(Limit);
      Expected.Parameters.push_back(
          {Limit.Name, Limit.Disposition, true, Limit.DefaultText});
    }

    Luna::Detail::ReflectionReturnFields Result;
    Result.Name = "Result";
    Result.Type = TypeOf(1);
    Result.Descriptor = NumberDescriptor();
    Fields.ReturnValues.push_back(std::move(Result));

    Candidate.AddRecord(std::move(Fields));
    Model.Records.push_back(std::move(Expected));
    return;
  }

  if (Choice == 7) {
    RecordFields Fields;
    Fields.Kind = Luna::SymbolKind::Module;
    Fields.Id = Symbol(Position);
    Fields.Name = "M" + Ordinal(Position);
    Fields.QualifiedName = Fields.Name;
    Fields.Documentation = "Module " + Ordinal(Position) + ".";

    Luna::Detail::ReflectionModuleFields Provenance;
    Provenance.Identity = "studio.m" + Ordinal(Position);
    Provenance.Version = "1." + Ordinal(Position) + ".0";
    Provenance.Symbol = Fields.Id;
    Provenance.Documentation = Fields.Documentation;
    Fields.Module = Candidate.AddModule(Provenance);

    ModelRecord Expected;
    Expected.Kind = Fields.Kind;
    Expected.Id = Fields.Id;
    Expected.Name = Fields.Name;
    Expected.QualifiedName = Fields.QualifiedName;
    Expected.Documentation = Fields.Documentation;
    Expected.ModuleIdentity = Provenance.Identity;

    Candidate.AddRecord(std::move(Fields));
    Model.Records.push_back(std::move(Expected));
    Model.Modules.push_back(
        {Provenance.Identity, Provenance.Version, Provenance.Symbol});
    return;
  }

  const bool Nested = !Scopes.empty() && Choice < 6;
  const ModelRecord *Parent =
      Nested ? &Model.Records[Scopes[Cursor.Pick(Scopes.size())]] : nullptr;
  const bool Namespaced = Choice < 2 || Choice == 6;

  RecordFields Fields;
  Fields.Kind = Namespaced ? Luna::SymbolKind::Namespace
                           : (Choice < 4 ? Luna::SymbolKind::OverloadSet
                                         : Luna::SymbolKind::Constant);
  Fields.Id = Symbol(Position);
  Fields.Name =
      (Namespaced ? "N" : (Choice < 4 ? "F" : "C")) + Ordinal(Position);
  Fields.QualifiedName =
      Parent ? Parent->QualifiedName + "." + Fields.Name : Fields.Name;
  if (Parent)
    Fields.Scope = Luna::ScopeId(Parent->Id);
  Fields.Documentation = "Symbol " + Ordinal(Position) + ".";

  ModelRecord Expected;
  Expected.Kind = Fields.Kind;
  Expected.Id = Fields.Id;
  Expected.Name = Fields.Name;
  Expected.QualifiedName = Fields.QualifiedName;
  Expected.Scope = Fields.Scope;
  Expected.Documentation = Fields.Documentation;

  if (Fields.Kind == Luna::SymbolKind::Constant) {
    Fields.Type = TypeOf(1);
    Fields.Descriptor = NumberDescriptor();
    Fields.Examples.push_back("print(" + Fields.QualifiedName + ")");
    Fields.Attributes.push_back({"Unit", "m/s^2"});
  }

  Candidate.AddRecord(std::move(Fields));
  Model.Records.push_back(std::move(Expected));

  if (Cursor.Pick(3) == 0) {
    const std::size_t TypePosition = ++TypeCounter;
    Luna::Detail::ReflectionTypeFields Type;
    Type.Id = TypeOf(TypePosition + 1);
    Type.Name = "t" + Ordinal(TypePosition);
    Type.Descriptor = NumberDescriptor();
    Candidate.AddType(Type);
    Model.Types.push_back({Type.Id, Type.Name});
  }
}

void VerifyRecord(const Luna::ReflectionRecord &Observed,
                  const ModelRecord &Expected) {
  RC_ASSERT(Observed.IsValid());
  RC_ASSERT(Observed.Kind() == Expected.Kind);
  RC_ASSERT(Observed.Id() == Expected.Id);
  RC_ASSERT(Observed.Name() == Expected.Name);
  RC_ASSERT(Observed.QualifiedName() == Expected.QualifiedName);
  RC_ASSERT(Observed.Signature() == Expected.Signature);
  RC_ASSERT(Observed.Documentation() == Expected.Documentation);
  RC_ASSERT(Observed.Scope() == Expected.Scope);
  RC_ASSERT(Observed.OverloadSet() == Expected.OverloadSet);
  RC_ASSERT(Observed.Declaration() == Expected.Id);
  RC_ASSERT(Observed.Returns() == Expected.Returns);
  RC_ASSERT(Observed.ReturnCount() == Expected.ReturnCount);
  RC_ASSERT(Observed.ParameterCount() == Expected.Parameters.size());
  for (std::size_t Index = 0; Index < Expected.Parameters.size(); ++Index) {
    const Luna::ParameterRecord Parameter = Observed.Parameter(Index);
    const ModelParameter &Model = Expected.Parameters[Index];
    RC_ASSERT(Parameter.IsValid());
    RC_ASSERT(Parameter.Name() == Model.Name);
    RC_ASSERT(Parameter.Disposition() == Model.Disposition);
    RC_ASSERT(Parameter.HasDefault() == Model.HasDefault);
    RC_ASSERT(Parameter.DefaultText() == Model.DefaultText);
  }
  RC_ASSERT(!Observed.Parameter(Expected.Parameters.size()).IsValid());
  RC_ASSERT(Observed.HasModule() == Expected.ModuleIdentity.has_value());
  if (Expected.ModuleIdentity)
    RC_ASSERT(Observed.Module().Identity() == *Expected.ModuleIdentity);
}

void VerifyGeneration(const Luna::ReflectionSnapshot &Snapshot,
                      const ModelGeneration &Model,
                      const ModelGeneration &Final) {
  const std::vector<ModelRecord> Ordered = CanonicalRecords(Model.Records);
  const std::vector<ModelType> OrderedTypes = CanonicalTypes(Model.Types);
  const std::vector<ModelModule> OrderedModules =
      CanonicalModules(Model.Modules);

  RC_ASSERT(Snapshot.Generation() == Model.Generation);
  RC_ASSERT(Snapshot.Size() == Ordered.size());
  RC_ASSERT(Snapshot.IsEmpty() == Ordered.empty());

  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
  RC_ASSERT(Symbols.Size() == Ordered.size());
  for (std::size_t Index = 0; Index < Ordered.size(); ++Index)
    VerifyRecord(Symbols.At(Index), Ordered[Index]);
  RC_ASSERT(!Symbols.At(Ordered.size()).IsValid());

  for (const ModelRecord &Expected : Ordered) {
    VerifyRecord(Snapshot.Find(Expected.Id), Expected);
    const auto First = std::find_if(
        Ordered.begin(), Ordered.end(), [&Expected](const ModelRecord &Other) {
          return Other.QualifiedName == Expected.QualifiedName;
        });
    RC_ASSERT(First != Ordered.end());
    VerifyRecord(Snapshot.Find(Expected.QualifiedName), *First);
  }

  for (const Luna::SymbolKind Kind : AllSymbolKinds) {
    std::vector<ModelRecord> Expected;
    for (const ModelRecord &Record : Ordered) {
      if (Record.Kind == Kind)
        Expected.push_back(Record);
    }
    const Luna::ReflectionRecordRange Observed = Snapshot.Symbols(Kind);
    RC_ASSERT(Observed.Size() == Expected.size());
    for (std::size_t Index = 0; Index < Expected.size(); ++Index)
      VerifyRecord(Observed.At(Index), Expected[Index]);
  }

  std::vector<Luna::ScopeId> Scopes{Luna::ScopeId::Root()};
  for (const ModelRecord &Record : Ordered) {
    if (std::find(Scopes.begin(), Scopes.end(), Record.Scope) == Scopes.end())
      Scopes.push_back(Record.Scope);
  }
  for (const Luna::ScopeId &Scope : Scopes) {
    std::vector<ModelRecord> Expected;
    for (const ModelRecord &Record : Ordered) {
      if (Record.Scope == Scope)
        Expected.push_back(Record);
    }
    const Luna::ReflectionRecordRange Observed = Snapshot.Symbols(Scope);
    RC_ASSERT(Observed.Size() == Expected.size());
    for (std::size_t Index = 0; Index < Expected.size(); ++Index)
      VerifyRecord(Observed.At(Index), Expected[Index]);
  }

  const Luna::TypeRecordRange Types = Snapshot.Types();
  RC_ASSERT(Types.Size() == OrderedTypes.size());
  for (std::size_t Index = 0; Index < OrderedTypes.size(); ++Index) {
    RC_ASSERT(Types.At(Index).Id() == OrderedTypes[Index].Id);
    RC_ASSERT(Types.At(Index).Name() == OrderedTypes[Index].Name);
    RC_ASSERT(Snapshot.FindType(OrderedTypes[Index].Id).Name() ==
              OrderedTypes[Index].Name);
  }

  const Luna::ModuleRecordRange Modules = Snapshot.Modules();
  RC_ASSERT(Modules.Size() == OrderedModules.size());
  for (std::size_t Index = 0; Index < OrderedModules.size(); ++Index) {
    RC_ASSERT(Modules.At(Index).Identity() == OrderedModules[Index].Identity);
    RC_ASSERT(Modules.At(Index).Version() == OrderedModules[Index].Version);
    RC_ASSERT(Modules.At(Index).Symbol() == OrderedModules[Index].Symbol);
  }

  for (const ModelRecord &Later : Final.Records) {
    const bool Present = std::find_if(Ordered.begin(), Ordered.end(),
                                      [&Later](const ModelRecord &Record) {
                                        return Record.Id == Later.Id;
                                      }) != Ordered.end();
    if (Present)
      continue;
    RC_ASSERT(!Snapshot.Find(Later.Id).IsValid());
    const bool NameShared =
        std::find_if(Ordered.begin(), Ordered.end(),
                     [&Later](const ModelRecord &Record) {
                       return Record.QualifiedName == Later.QualifiedName;
                     }) != Ordered.end();
    if (!NameShared)
      RC_ASSERT(!Snapshot.Find(Later.QualifiedName).IsValid());
  }
  for (const ModelType &Later : Final.Types) {
    const bool Present = std::find_if(OrderedTypes.begin(), OrderedTypes.end(),
                                      [&Later](const ModelType &Type) {
                                        return Type.Id == Later.Id;
                                      }) != OrderedTypes.end();
    if (!Present)
      RC_ASSERT(!Snapshot.FindType(Later.Id).IsValid());
  }
}

void VerifyRetained(const std::vector<Luna::ReflectionSnapshot> &Retained,
                    const std::vector<ModelGeneration> &Models,
                    const ModelGeneration &Final) {
  RC_ASSERT(Retained.size() == Models.size());
  for (std::size_t Index = 0; Index < Retained.size(); ++Index)
    VerifyGeneration(Retained[Index], Models[Index], Final);
}

void VerifyRejectionPublishesNothing(Database &Reflection,
                                     const ModelGeneration &Committed) {
  Builder Rejected = Reflection.BeginGeneration();
  RecordFields Duplicate;
  Duplicate.Kind = Luna::SymbolKind::Namespace;
  Duplicate.Id = Luna::SymbolId();
  Duplicate.Name = "Broken";
  Duplicate.QualifiedName = "Broken";
  Rejected.AddRecord(std::move(Duplicate));

  std::shared_ptr<const Storage> Prepared;
  RC_ASSERT(Reflection.Prepare(Rejected, Prepared) == Status::InvalidIdentity);
  RC_ASSERT(!Prepared);
  RC_ASSERT(Reflection.PublishGeneration(Rejected) == Status::InvalidIdentity);
  RC_ASSERT(Reflection.Generation() == Committed.Generation);
  RC_ASSERT(Reflection.Count() == Committed.Records.size());
}

} // namespace

int RunOwningReflectionSnapshotProperties() {

  const bool Passed = rc::check(

      "Reflection snapshots retain one complete immutable generation",
      [](const std::vector<std::uint8_t> &CommitShape,
         const std::vector<std::uint8_t> &ActionShape) {
        ByteCursor Plan(CommitShape);
        ByteCursor Actions(ActionShape);

        std::vector<Luna::ReflectionSnapshot> Retained;
        std::vector<ModelGeneration> Models;
        ModelGeneration Current;
        std::size_t SymbolCounter = 0;
        std::size_t TypeCounter = 0;

        {
          Luna::State Origin;
          RC_ASSERT(Origin.IsReady());
          Database *Reflection = Hooks::ReflectionDatabaseOf(Origin);
          RC_ASSERT(Reflection != nullptr);

          RC_ASSERT(Hooks::ReflectionGeneration(Origin) == 0);
          const Luna::ReflectionSnapshot Initial =
              Origin.Bindings().Reflection();
          Retained.push_back(Initial);
          Models.push_back(Current);

          const std::size_t CommitCount = 2U + Plan.Pick(3);
          for (std::size_t Commit = 0; Commit < CommitCount; ++Commit) {
            Builder Candidate = Reflection->BeginGeneration();
            RC_ASSERT(Candidate.RecordCount() == Current.Records.size());
            RC_ASSERT(Candidate.TypeCount() == Current.Types.size());
            RC_ASSERT(Candidate.ModuleCount() == Current.Modules.size());

            ModelGeneration Next = Current;
            const std::size_t Additions = 1U + Plan.Pick(3);
            for (std::size_t Index = 0; Index < Additions; ++Index)
              AppendSymbol(Plan, Candidate, Next, SymbolCounter, TypeCounter);

            RC_ASSERT(Reflection->PublishGeneration(Candidate) ==
                      Status::Valid);
            Next.Generation = Current.Generation + 1;
            Current = std::move(Next);

            RC_ASSERT(Reflection->Generation() == Current.Generation);
            RC_ASSERT(Hooks::ReflectionGeneration(Origin) ==
                      Current.Generation);
            RC_ASSERT(Reflection->Count() == Current.Records.size());

            Retained.push_back(Origin.Bindings().Reflection());
            Models.push_back(Current);
            VerifyRetained(Retained, Models, Current);

            if (Actions.Pick(2) == 0) {
              VerifyRejectionPublishesNothing(*Reflection, Current);
              VerifyRetained(Retained, Models, Current);
            }

            if (Actions.Pick(3) == 0) {
              RC_ASSERT(Reflection->Publish(Reflection->Capture()));
              RC_ASSERT(Reflection->Generation() == Current.Generation);
              VerifyRetained(Retained, Models, Current);
            }
          }

          Luna::State Moved = std::move(Origin);
          RC_ASSERT(!Origin.IsReady());
          RC_ASSERT(Hooks::ReflectionGeneration(Origin) == 0);
          RC_ASSERT(Origin.Bindings().Reflection().IsEmpty());
          RC_ASSERT(Hooks::ReflectionGeneration(Moved) == Current.Generation);
          const Luna::ReflectionSnapshot AfterMove =
              Moved.Bindings().Reflection();
          VerifyGeneration(AfterMove, Current, Current);
          VerifyRetained(Retained, Models, Current);

          Retained.push_back(AfterMove);
          Models.push_back(Current);
        }

        VerifyRetained(Retained, Models, Current);

        const Luna::ReflectionSnapshot &Last = Retained.back();
        const std::vector<ModelRecord> Expected =
            CanonicalRecords(Current.Records);
        bool CrossThreadMatches = false;
        std::thread Reader([&Last, &Expected, &Current, &CrossThreadMatches] {
          CrossThreadMatches = Last.Generation() == Current.Generation &&
                               Last.Size() == Expected.size() &&
                               Last.Symbols().Size() == Expected.size();
          for (const ModelRecord &Record : Expected) {
            CrossThreadMatches =
                CrossThreadMatches &&
                Last.Find(Record.Id).QualifiedName() == Record.QualifiedName;
          }
        });
        Reader.join();
        RC_ASSERT(CrossThreadMatches);
      });

  return Passed ? 0 : 1;
}
