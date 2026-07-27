// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/binding/value.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::FreezeCacheObservation;
using Luna::Detail::OwnershipModel;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "frozen cache lookup check failed: " << Description << '\n';
}

struct Part {
  virtual ~Part() = default;
  int Serial = 7;
};

struct Gadget final : Part {
  int Charge = 3;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

[[nodiscard]] int Measure(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Value, int Scale) { return Value * Scale; }

[[nodiscard]] Luna::StableTypeKey PartKey() {
  return Luna::StableTypeKey("tests.freeze.lookup.Part");
}

[[nodiscard]] Luna::StableTypeKey GadgetKey() {
  return Luna::StableTypeKey("tests.freeze.lookup.Gadget");
}

[[nodiscard]] Luna::ModuleManifest UnitsManifest() {
  const std::optional<Luna::SemanticVersion> Version =
      Luna::SemanticVersion::TryParse("2.1.0");
  const std::optional<Luna::ModuleManifest> Manifest =
      Version ? Luna::ModuleManifest::TryCreate("tests.freeze.units", *Version,
                                                {}, "", {})
              : std::nullopt;
  return Manifest ? *Manifest : Luna::ModuleManifest();
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Scale", 2));
}

[[nodiscard]] Luna::RegistrationResult RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  if (auto First = Registry.RegisterFunction(
          "Measure", Luna::Overload<int(int)>(&Measure));
      !First.IsSuccess())
    return First;
  if (auto Second = Registry.RegisterFunction(
          "Measure", Luna::Overload<int(int, int)>(&Measure));
      !Second.IsSuccess())
    return Second;

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterConstant("Version", 5));
  Luna::ClassBuilder<Part> Base = Studio.RegisterClass<Part>("Part", PartKey());
  static_cast<void>(Base.Field("Serial", &Part::Serial).QualifiedName());
  Luna::ClassBuilder<Gadget> Derived =
      Studio.RegisterClass<Gadget>("Gadget", GadgetKey());
  static_cast<void>(
      Derived.Base<Part>(PartKey())
          .Field("Charge", &Gadget::Charge)
          .Property("Level", Luna::PropertyPolicy::Lazy(), &Gadget::Level)
          .QualifiedName());
  if (auto Committed = Studio.Commit(); !Committed.IsSuccess())
    return Committed;
  return Registry.RegisterModule(UnitsManifest(), &ConfigureUnits);
}

[[nodiscard]] std::vector<std::string> Fields(const std::string &Text) {
  std::vector<std::string> Parts;
  std::size_t Start = 0;
  while (true) {
    const std::size_t Separator = Text.find('|', Start);
    if (Separator == std::string::npos) {
      Parts.push_back(Text.substr(Start));
      return Parts;
    }
    Parts.push_back(Text.substr(Start, Separator - Start));
    Start = Separator + 1;
  }
}

[[nodiscard]] std::string FieldAt(const std::string &Text, std::size_t Index) {
  const std::vector<std::string> Parts = Fields(Text);
  return Index < Parts.size() ? Parts[Index] : std::string();
}

[[nodiscard]] bool CachesLookupNamed(const FreezeCacheObservation &Observed,
                                     const std::string &QualifiedName) {
  return std::any_of(Observed.LookupDetails.begin(),
                     Observed.LookupDetails.end(),
                     [&QualifiedName](const std::string &Detail) {
                       return FieldAt(Detail, 0) == QualifiedName;
                     });
}

const std::vector<std::string> &AbsentNames() {
  static const std::vector<std::string> Names{
      "Missing", "Studio.Missing", "Studio.Gadget.Absent", "Units.Missing",
      "tests.freeze.absent"};
  return Names;
}

void CheckCachedLookupsMatchUncachedLookups() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterModel(Owner).IsSuccess(), "the representative model registers");
  Check(Registry.Freeze().IsSuccess(), "the populated State freezes");

  const FreezeCacheObservation Cached = Hooks::ObserveFreezeCache(Owner);
  const Luna::ReflectionSnapshot Uncached = Registry.Reflection();
  const Luna::ReflectionRecordRange Symbols = Uncached.Symbols();

  Check(Cached.Published && Cached.LookupDetails.size() == Cached.Lookups &&
            Cached.OrderedLookups.size() == Cached.Lookups,
        "one published cache describes every entry it holds");

  std::size_t Reflected = 0;
  for (const std::string &Detail : Cached.LookupDetails) {
    const std::vector<std::string> Parts = Fields(Detail);
    Check(Parts.size() == 5, "every cached lookup is fully described");
    if (Parts.size() != 5)
      continue;
    if (Parts[4] == "-") {
      Check(Parts[1] == "type" || Parts[1] == "dispatch_target" ||
                Parts[1] == "metatable",
            "only a private committed identity retains no reflection record");
      continue;
    }
    ++Reflected;
    const std::size_t Index = static_cast<std::size_t>(std::stoull(Parts[4]));
    Check(Index < Symbols.Size(), "a cached record index is within its "
                                  "captured reflection generation");
    if (Index >= Symbols.Size())
      continue;
    const Luna::ReflectionRecord Record = Symbols.At(Index);
    Check(Record.IsValid() && Record.QualifiedName() == Parts[0] &&
              std::string(Luna::SymbolKindText(Record.Kind())) == Parts[2] &&
              Record.Id().ToString() == Parts[3],
          "a cached lookup resolves to exactly the record it names");
    const Luna::SymbolId Primary =
        Record.OverloadSet().IsValid() ? Record.OverloadSet() : Record.Id();
    Check(Uncached.Find(Record.Id()).QualifiedName() == Parts[0] &&
              Uncached.Find(std::string(Parts[0])).Id() == Primary,
          "the cached and uncached lookups of one symbol agree by name and by "
          "identity");
  }
  Check(Reflected > 0, "the cache names reflected records");

  Check(Cached.Namespaces == Hooks::NamespaceOwnershipCount(Owner) &&
            Cached.Metatables == Hooks::RegisteredClassCount(Owner) &&
            Cached.Modules == Hooks::LoadedModuleCount(Owner) &&
            Cached.Overloads == Hooks::BindingCount(Owner),
        "the cached entry counts equal the uncached store counts");

  for (const std::string &Text : Cached.OrderedNamespaces) {
    const std::string Name = FieldAt(Text, 0);
    const Luna::ReflectionRecord Record = Uncached.Find(Name);
    Check(Hooks::NamespaceIsOwned(Owner, Name) && Record.IsValid() &&
              Record.Kind() == Luna::SymbolKind::Namespace &&
              Record.Id().ToString() == FieldAt(Text, 1),
          "every cached namespace is the owned namespace the uncached "
          "generation reflects");
  }

  for (std::size_t Index = 0; Index < Cached.OrderedMetatables.size();
       ++Index) {
    const std::string &Text = Cached.OrderedMetatables[Index];
    const std::string Name = FieldAt(Text, 0);
    const std::optional<Luna::TypeId> Type = Hooks::ClassTypeOf(Owner, Name);
    const Luna::ReflectionRecord Record = Uncached.Find(Name);
    Check(Hooks::ClassIsRegistered(Owner, Name) && Type.has_value() &&
              Type->ToString() == FieldAt(Text, 1) &&
              Hooks::ClassMetatableIdentityOf(Owner, Name) ==
                  std::optional<std::uint64_t>(
                      Cached.MetatableIdentities[Index]) &&
              Record.IsValid() && Record.Kind() == Luna::SymbolKind::Class &&
              Record.Id().ToString() == FieldAt(Text, 2),
          "every cached metatable entry is the registered class the uncached "
          "generation reflects");
    Check(Uncached.FindType(*Type).IsValid(),
          "every cached class type resolves in the uncached type generation");
  }

  for (const std::string &Text : Cached.OrderedModules) {
    const std::string Identity = FieldAt(Text, 0);
    Check(Hooks::ModuleIsLoaded(Owner, Identity) &&
              Hooks::LoadedModuleVersion(Owner, Identity) ==
                  std::optional<std::string>(FieldAt(Text, 1)),
          "every cached module entry is the loaded module the uncached store "
          "holds");
  }

  for (const std::string &Text : Cached.OrderedOverloads) {
    const std::string Name = FieldAt(Text, 0);
    const std::size_t Count =
        static_cast<std::size_t>(std::stoull(FieldAt(Text, 1)));
    Check(Hooks::OverloadCandidateCount(Owner, Name) == Count &&
              Hooks::OverloadCandidateSignatures(Owner, Name).size() == Count,
          "every cached overload index agrees with the uncached candidate set");
  }

  const Luna::TypeRecordRange Types = Uncached.Types();
  for (std::size_t Index = 0; Index < Types.Size(); ++Index) {
    const std::string Identity = Types.At(Index).Id().ToString();
    Check(std::find(Cached.OrderedConversions.begin(),
                    Cached.OrderedConversions.end(),
                    Identity) != Cached.OrderedConversions.end(),
          "every reflected canonical type is in the frozen conversion table");
  }

  for (const std::string &Absent : AbsentNames()) {
    Check(!Uncached.Find(Absent).IsValid() &&
              !CachesLookupNamed(Cached, Absent),
          "a name the model never declared is absent from both sides");
  }

  Check(Owner
            .Execute("assert(Measure(4) == 5)\n"
                     "assert(Measure(4, 3) == 12)\n"
                     "assert(Studio.Version == 5)\n"
                     "assert(Units.Scale == 2)")
            .IsSuccess(),
        "the cached paths answer exactly what the uncached paths answered");
}

[[nodiscard]] std::string ExposeGadget(Luna::State &Owner,
                                       const std::string &Path, Gadget &Object,
                                       const std::uint64_t *Lifetime) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Studio.Gadget";
  Request.Path = Path;
  Request.Storage = &Object;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.LifetimeGeneration = Lifetime;
  return Hooks::ExposeClassUserdata(Owner, Request).Status;
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
ReadMember(Luna::State &Owner, const std::string &Path,
           const std::string &Member) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Studio.Gadget";
  Request.Member = Member;
  Request.Path = Path;
  return Hooks::ReadClassMemberValue(Owner, Request);
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
WriteMember(Luna::State &Owner, const std::string &Path,
            const std::string &Member, int Value) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Studio.Gadget";
  Request.Member = Member;
  Request.Path = Path;
  Request.Incoming = Luna::Value(Value);
  return Hooks::WriteClassMemberValue(Owner, Request);
}

void CheckInvalidationPrecedesUnavailability() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterModel(Owner).IsSuccess(), "the representative model registers");
  Check(Registry.Freeze().IsSuccess(), "the populated State freezes");
  const FreezeCacheObservation Published = Hooks::ObserveFreezeCache(Owner);

  Gadget Value;
  std::uint64_t Lifetime = 1;
  const std::string Path = "Exposed";
  Check(ExposeGadget(Owner, Path, Value, &Lifetime) == "created" &&
            Hooks::LiveCachedIdentityCount(Owner) == 1,
        "one exposure records one live identity entry");

  const auto First = ReadMember(Owner, Path, "Level");
  Check(First.Reached && First.Recorded && !First.ServedFromCache &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1,
        "a lazy read records its value under the current generation");
  const auto Reused = ReadMember(Owner, Path, "Level");
  Check(Reused.Reached && Reused.ServedFromCache,
        "the recorded value serves the next read");

  const auto Written = WriteMember(Owner, Path, "Charge", 5);
  Check(Written.Reached && Written.Invalidated == 1 &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &Value) == 0,
        "a successful write invalidates every cached value of that object");
  const auto Recomputed = ReadMember(Owner, Path, "Level");
  Check(Recomputed.Reached && !Recomputed.ServedFromCache &&
            Recomputed.Produced.has_value() &&
            std::get<int>(*Recomputed.Produced) == 10,
        "the next read recomputes from the object rather than from a stale "
        "value");

  Check(Hooks::AdvanceLifecycleGeneration(Owner),
        "the test advances the dispatch generation");
  Check(Hooks::LazyMemberCacheEntryCountOf(Owner, &Value) == 1 &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 0,
        "an earlier generation's value is unreachable while still owned");
  const auto AfterGeneration = ReadMember(Owner, Path, "Level");
  Check(AfterGeneration.Reached && !AfterGeneration.ServedFromCache &&
            AfterGeneration.Recorded &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1,
        "the later generation records its own value");

  Check(Hooks::RetireClassUserdata(Owner, &Value) &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &Value) == 0 &&
            Hooks::LiveCachedIdentityCount(Owner) == 0,
        "retirement drops the identity entry and every value of that object");
  const auto Refused = ReadMember(Owner, Path, "Level");
  Check(!Refused.Reached && Refused.Receiver == "invalidated" &&
            Refused.Boundary == "before_user_code",
        "an access after invalidation is refused before user code");

  Luna::Detail::ClassAccessRequest Access;
  Access.QualifiedName = "Studio.Gadget";
  Access.Path = Path;
  Access.ExpectedStorage = &Value;
  Check(!Hooks::AccessClassUserdata(Owner, Access).ReachedNativeCode,
        "no receiver conversion reaches an invalidated object");

  const FreezeCacheObservation After = Hooks::ObserveFreezeCache(Owner);
  Check(After.Address == Published.Address &&
            After.LookupDetails == Published.LookupDetails &&
            After.OrderedMetatables == Published.OrderedMetatables &&
            After.MetatableIdentities == Published.MetatableIdentities &&
            After.OrderedNamespaces == Published.OrderedNamespaces &&
            After.OrderedModules == Published.OrderedModules,
        "documented runtime state never mutates one published cache");
}

struct SnapshotReading final {
  std::vector<std::string> Names;
  std::uint64_t Generation = 0;
  std::size_t Types = 0;
  std::size_t Modules = 0;
  bool FoundByName = false;
  bool FoundById = false;
  bool RefusedAbsent = true;
};

[[nodiscard]] SnapshotReading Read(const Luna::ReflectionSnapshot &Snapshot) {
  SnapshotReading Observed;
  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
  Observed.Names.reserve(Symbols.Size());
  for (std::size_t Index = 0; Index < Symbols.Size(); ++Index)
    Observed.Names.push_back(std::string(Symbols.At(Index).QualifiedName()));
  Observed.Generation = Snapshot.Generation();
  Observed.Types = Snapshot.Types().Size();
  Observed.Modules = Snapshot.Modules().Size();

  const Luna::ReflectionRecord Gadget = Snapshot.Find("Studio.Gadget");
  Observed.FoundByName = Gadget.IsValid();
  Observed.FoundById = Snapshot.Find(Gadget.Id()).QualifiedName() ==
                       std::string("Studio.Gadget");
  for (const std::string &Absent : AbsentNames())
    Observed.RefusedAbsent =
        Observed.RefusedAbsent && !Snapshot.Find(Absent).IsValid();
  return Observed;
}

[[nodiscard]] bool Same(const SnapshotReading &Left,
                        const SnapshotReading &Right) {
  return Left.Names == Right.Names && Left.Generation == Right.Generation &&
         Left.Types == Right.Types && Left.Modules == Right.Modules &&
         Left.FoundByName == Right.FoundByName &&
         Left.FoundById == Right.FoundById &&
         Left.RefusedAbsent == Right.RefusedAbsent;
}

[[nodiscard]] SnapshotReading
ReadOnAnotherThread(const Luna::ReflectionSnapshot &Snapshot) {
  SnapshotReading Observed;
  std::thread Reader([&Snapshot, &Observed] { Observed = Read(Snapshot); });
  Reader.join();
  return Observed;
}

void CheckSnapshotsReadFromAnyThread() {
  Luna::ReflectionSnapshot Retained;
  SnapshotReading OwnerSideAfterFreeze;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(RegisterModel(Owner).IsSuccess(),
          "the representative model registers");

    const Luna::ReflectionSnapshot BeforeFreeze = Registry.Reflection();
    const SnapshotReading OwnerSide = Read(BeforeFreeze);
    Check(Same(OwnerSide, ReadOnAnotherThread(BeforeFreeze)),
          "a snapshot taken before freeze reads identically from another "
          "thread");

    Check(Registry.Freeze().IsSuccess(), "the populated State freezes");
    Retained = Registry.Reflection();
    OwnerSideAfterFreeze = Read(Retained);
    Check(Same(OwnerSideAfterFreeze, OwnerSide),
          "freeze changes nothing an owning snapshot reports");
    Check(Same(OwnerSideAfterFreeze, ReadOnAnotherThread(Retained)),
          "a snapshot taken after freeze reads identically from another "
          "thread");

    SnapshotReading Captured;
    std::thread Foreign([&Owner, &Captured] {
      Captured = Read(Owner.Bindings().Reflection());
    });
    Foreign.join();
    Check(Same(Captured, OwnerSideAfterFreeze),
          "capturing an owning snapshot from another thread is permitted");
  }

  Check(Same(OwnerSideAfterFreeze, ReadOnAnotherThread(Retained)),
        "a retained snapshot still reads from another thread after its frozen "
        "State is destroyed");
}

} // namespace

int RunFrozenCacheLookupTests() {
  FailureCount = 0;
  CheckCachedLookupsMatchUncachedLookups();
  CheckInvalidationPrecedesUnavailability();
  CheckSnapshotsReadFromAnyThread();
  return FailureCount == 0 ? 0 : 1;
}
