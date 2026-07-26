// Property 30: frozen caches are equivalent to uncached generation lookups.
//
// Two States are generated from the same committed input and compared with an
// independent model written here rather than with Luna's own accounting.
//
// The generated input is one prelude - two classes, one of which declares the
// other as its base - plus a generated list of independent declaration units:
// two-candidate functions, namespaces holding a constant, classes holding a
// field and one explicitly lazy property, and loaded modules that register
// their own namespace and constant. The first State commits those units in the
// generated order and then takes a generated query history over them:
// reflection lookups by name and by kind, retained snapshots, real script
// calls, userdata exposure, lazy reads, member writes, explicit cache
// invalidation, and relationship enumeration. The second State commits exactly
// the same units in a generated permutation and takes no history at all.
//
// The model predicts, from the generated unit list alone, every namespace,
// every class, every loaded module identity and version, every callable path
// with its candidate count, and every public declaration with its symbol kind.
// Both the frozen caches and the uncached lookups are compared with that model,
// and then with each other: every cached lookup that names a reflected record
// must reach exactly the record the uncached canonical enumeration holds at
// that index, every cached namespace, class, module, and overload entry must
// agree with the live uncached query for it, and every reflected canonical type
// must be present in the frozen conversion table. Names the model never
// declared must be absent from both sides, so a refused lookup is refused
// identically cached and uncached.
//
// The generated lifecycle is checked at the same time. A generated number of
// injected preparation failures each leave the State Ready and unchanged and
// publish nothing; a generated number of repeated freezes return one identical
// already-frozen result without republishing; a foreign-thread freeze and a
// foreign-thread registration are both refused before any mutation; and after
// freeze, the documented owner-thread runtime state follows its owners - a
// dispatch-generation change makes every earlier lazy value unreachable while
// its entry is still owned, and retiring one exposed value drops its entries
// and its live identity before the value becomes unavailable, so the next
// access is refused rather than reaching released storage. None of that ever
// changes the published cache.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::FreezeCacheObservation;
using Luna::Detail::OwnershipModel;
using Luna::Detail::StateFaultPoint;

// Deterministic byte source. Equal bytes always drive the equal scenario, so a
// shrunk counterexample replays exactly the same committed input, the same
// query history, and the same lifecycle sequence.
class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 37U + 11U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

// ---------------------------------------------------------------------------
// The registered surface: one base class, one derived class always present, and
// four distinct generated classes.
// ---------------------------------------------------------------------------

struct Part {
  virtual ~Part() = default;
  int Serial = 7;
};

struct Gadget final : Part {
  int Charge = 3;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

template <int Ordinal> struct Widget final : Part {
  int Charge = Ordinal + 1;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

[[nodiscard]] int Measure(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Value, int Scale) { return Value * Scale; }

[[nodiscard]] Luna::StableTypeKey PartKey() {
  return Luna::StableTypeKey("tests.freeze.cache.Part");
}

[[nodiscard]] Luna::StableTypeKey GadgetKey() {
  return Luna::StableTypeKey("tests.freeze.cache.Gadget");
}

template <int Ordinal> [[nodiscard]] Luna::StableTypeKey WidgetKey() {
  return Luna::StableTypeKey("tests.freeze.cache.Widget" +
                             std::to_string(Ordinal));
}

[[nodiscard]] std::string CallableName(std::size_t Ordinal) {
  return "Fn" + std::to_string(Ordinal);
}

[[nodiscard]] std::string ScopeName(std::size_t Ordinal) {
  return "Ns" + std::to_string(Ordinal);
}

[[nodiscard]] std::string ClassName(std::size_t Ordinal) {
  return "Widget" + std::to_string(Ordinal);
}

[[nodiscard]] std::string ModuleScopeName(std::size_t Ordinal) {
  return "Mod" + std::to_string(Ordinal);
}

[[nodiscard]] std::string ModuleIdentity(std::size_t Ordinal) {
  return "tests.cache" + std::to_string(Ordinal);
}

constexpr std::string_view ModuleVersion = "1.0.0";

template <int Ordinal> void ConfigureModule(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Scope =
      Builder.RegisterNamespace(ModuleScopeName(Ordinal));
  static_cast<void>(Scope.RegisterConstant("Setting", Ordinal + 1));
}

template <int Ordinal> [[nodiscard]] Luna::ModuleManifest ManifestOf() {
  const auto Version = Luna::SemanticVersion::TryParse(ModuleVersion);
  const auto Manifest =
      Version ? Luna::ModuleManifest::TryCreate(ModuleIdentity(Ordinal),
                                                *Version, {}, "", {})
              : std::nullopt;
  return Manifest ? *Manifest : Luna::ModuleManifest();
}

// ---------------------------------------------------------------------------
// One generated declaration unit. Every unit is independent of every other one,
// so any permutation of the list is the same committed input.
// ---------------------------------------------------------------------------

enum class UnitKind : std::uint8_t { Callable, Scope, Class, Module };

struct Unit final {
  UnitKind Kind = UnitKind::Callable;
  std::size_t Ordinal = 0;
};

[[nodiscard]] Luna::RegistrationResult RegisterPrelude(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::ClassBuilder<Part> Base =
      Registry.RegisterClass<Part>("Part", PartKey());
  if (auto Staged = Base.Field("Serial", &Part::Serial).Commit();
      !Staged.IsSuccess())
    return Staged;

  Luna::ClassBuilder<Gadget> Derived =
      Registry.RegisterClass<Gadget>("Gadget", GadgetKey());
  return Derived.Base<Part>(PartKey())
      .Field("Charge", &Gadget::Charge)
      .Property("Level", Luna::PropertyPolicy::Lazy(), &Gadget::Level)
      .Commit();
}

template <int Ordinal>
[[nodiscard]] Luna::RegistrationResult
RegisterWidget(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Widget<Ordinal>> Class =
      Registry.RegisterClass<Widget<Ordinal>>(ClassName(Ordinal),
                                              WidgetKey<Ordinal>());
  return Class.template Base<Part>(PartKey())
      .Field("Charge", &Widget<Ordinal>::Charge)
      .Property("Level", Luna::PropertyPolicy::Lazy(), &Widget<Ordinal>::Level)
      .Commit();
}

[[nodiscard]] Luna::RegistrationResult
RegisterGeneratedClass(Luna::BindingRegistry &Registry, std::size_t Ordinal) {
  switch (Ordinal) {
  case 0:
    return RegisterWidget<0>(Registry);
  case 1:
    return RegisterWidget<1>(Registry);
  case 2:
    return RegisterWidget<2>(Registry);
  default:
    break;
  }
  return RegisterWidget<3>(Registry);
}

[[nodiscard]] Luna::RegistrationResult
RegisterGeneratedModule(Luna::BindingRegistry &Registry, std::size_t Ordinal) {
  switch (Ordinal) {
  case 0:
    return Registry.RegisterModule(ManifestOf<0>(), &ConfigureModule<0>);
  case 1:
    return Registry.RegisterModule(ManifestOf<1>(), &ConfigureModule<1>);
  case 2:
    return Registry.RegisterModule(ManifestOf<2>(), &ConfigureModule<2>);
  default:
    break;
  }
  return Registry.RegisterModule(ManifestOf<3>(), &ConfigureModule<3>);
}

[[nodiscard]] Luna::RegistrationResult RegisterUnit(Luna::State &Owner,
                                                    const Unit &Declared) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  switch (Declared.Kind) {
  case UnitKind::Callable: {
    const std::string Name = CallableName(Declared.Ordinal);
    if (auto First =
            Registry.RegisterFunction(Name, Luna::Overload<int(int)>(&Measure));
        !First.IsSuccess())
      return First;
    return Registry.RegisterFunction(Name,
                                     Luna::Overload<int(int, int)>(&Measure));
  }
  case UnitKind::Scope: {
    Luna::NamespaceBuilder Scope =
        Registry.RegisterNamespace(ScopeName(Declared.Ordinal));
    static_cast<void>(Scope.RegisterConstant(
        "Value", static_cast<int>(Declared.Ordinal) + 1));
    return Scope.Commit();
  }
  case UnitKind::Class:
    return RegisterGeneratedClass(Registry, Declared.Ordinal);
  case UnitKind::Module:
    break;
  }
  return RegisterGeneratedModule(Registry, Declared.Ordinal);
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// The independent model of the committed surface. It is built from the
// generated unit list alone, in canonical order, so it never depends on the
// order the units were committed in, on any query taken before freeze, or on
// anything Luna reports about itself.
// ---------------------------------------------------------------------------

struct ModelSurface final {
  std::vector<std::string> Namespaces;
  std::vector<std::string> Classes;
  std::vector<std::string> Modules; // identity|version
  std::vector<std::pair<std::string, std::size_t>> Callables;
  std::vector<std::pair<std::string, Luna::SymbolKind>> Declarations;
  std::size_t DerivedClasses = 0;
};

void Declare(ModelSurface &Model, std::string QualifiedName,
             Luna::SymbolKind Kind) {
  Model.Declarations.emplace_back(std::move(QualifiedName), Kind);
}

[[nodiscard]] ModelSurface ModelOf(const std::vector<Unit> &Units) {
  ModelSurface Model;

  // The prelude: one base class with one field, and one derived class with a
  // field and one explicitly lazy property.
  Model.Classes.push_back("Part");
  Declare(Model, "Part", Luna::SymbolKind::Class);
  Declare(Model, "Part.Serial", Luna::SymbolKind::Field);
  Model.Classes.push_back("Gadget");
  Declare(Model, "Gadget", Luna::SymbolKind::Class);
  Declare(Model, "Gadget.Charge", Luna::SymbolKind::Field);
  Declare(Model, "Gadget.Level", Luna::SymbolKind::Property);
  ++Model.DerivedClasses;

  for (const Unit &Declared : Units) {
    switch (Declared.Kind) {
    case UnitKind::Callable: {
      const std::string Name = CallableName(Declared.Ordinal);
      Model.Callables.emplace_back(Name, 2);
      Declare(Model, Name, Luna::SymbolKind::OverloadSet);
      break;
    }
    case UnitKind::Scope: {
      const std::string Name = ScopeName(Declared.Ordinal);
      Model.Namespaces.push_back(Name);
      Declare(Model, Name, Luna::SymbolKind::Namespace);
      Declare(Model, Name + ".Value", Luna::SymbolKind::Constant);
      break;
    }
    case UnitKind::Class: {
      const std::string Name = ClassName(Declared.Ordinal);
      Model.Classes.push_back(Name);
      Declare(Model, Name, Luna::SymbolKind::Class);
      Declare(Model, Name + ".Charge", Luna::SymbolKind::Field);
      Declare(Model, Name + ".Level", Luna::SymbolKind::Property);
      ++Model.DerivedClasses;
      break;
    }
    case UnitKind::Module: {
      const std::string Scope = ModuleScopeName(Declared.Ordinal);
      Model.Modules.push_back(ModuleIdentity(Declared.Ordinal) + "|" +
                              std::string(ModuleVersion));
      Model.Namespaces.push_back(Scope);
      Declare(Model, Scope, Luna::SymbolKind::Namespace);
      Declare(Model, Scope + ".Setting", Luna::SymbolKind::Constant);
      break;
    }
    }
  }

  std::sort(Model.Namespaces.begin(), Model.Namespaces.end());
  std::sort(Model.Classes.begin(), Model.Classes.end());
  std::sort(Model.Modules.begin(), Model.Modules.end());
  std::sort(Model.Callables.begin(), Model.Callables.end());
  std::sort(Model.Declarations.begin(), Model.Declarations.end());
  return Model;
}

[[nodiscard]] std::size_t CandidateTotal(const ModelSurface &Model) {
  std::size_t Total = 0;
  for (const auto &[Name, Count] : Model.Callables)
    Total += Count;
  return Total;
}

// ---------------------------------------------------------------------------
// Canonical text helpers. Every cached entry is observed as one `|`-separated
// text, so nothing here reads Luna's private storage layout.
// ---------------------------------------------------------------------------

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

[[nodiscard]] std::vector<std::string>
LeadingFields(const std::vector<std::string> &Texts, std::size_t Count) {
  std::vector<std::string> Extracted;
  Extracted.reserve(Texts.size());
  for (const std::string &Text : Texts) {
    const std::vector<std::string> Parts = Fields(Text);
    std::string Joined;
    for (std::size_t Index = 0; Index < Count && Index < Parts.size();
         ++Index) {
      if (Index != 0)
        Joined.push_back('|');
      Joined.append(Parts[Index]);
    }
    Extracted.push_back(std::move(Joined));
  }
  return Extracted;
}

[[nodiscard]] std::vector<std::string>
OrderedNames(const Luna::ReflectionRecordRange &Range) {
  std::vector<std::string> Names;
  Names.reserve(Range.Size());
  for (std::size_t Index = 0; Index < Range.Size(); ++Index)
    Names.push_back(std::string(Range.At(Index).QualifiedName()));
  return Names;
}

[[nodiscard]] bool CachesLookupNamed(const FreezeCacheObservation &Observed,
                                     const std::string &QualifiedName) {
  for (const std::string &Detail : Observed.LookupDetails) {
    if (FieldAt(Detail, 0) == QualifiedName)
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// One published cache, compared with the model that predicted it.
// ---------------------------------------------------------------------------

void VerifyCacheMatchesModel(const Luna::State &Owner,
                             const FreezeCacheObservation &Observed,
                             const ModelSurface &Model) {
  RC_ASSERT(Observed.Published);
  RC_ASSERT(Hooks::IsFrozen(Owner));

  const auto Generations = Hooks::GenerationsOf(Owner);
  RC_ASSERT(Generations != nullptr);
  RC_ASSERT(Observed.Key.State == Hooks::LogicalIdentityOf(Owner).value_or(
                                      Luna::Detail::StateIdentity()));
  RC_ASSERT(Observed.Key.Generation == Generations->Generation());
  RC_ASSERT(Observed.Key.ReflectionGeneration ==
            Hooks::ReflectionGeneration(Owner));
  RC_ASSERT(Observed.Key.LifecycleGeneration ==
            Hooks::LifecycleGenerationOf(Owner).value_or(0));

  RC_ASSERT(Observed.Lookups == Generations->Symbols().Size());
  RC_ASSERT(Observed.LookupDetails.size() == Observed.Lookups);
  RC_ASSERT(Observed.OrderedLookups.size() == Observed.Lookups);

  RC_ASSERT(LeadingFields(Observed.OrderedNamespaces, 1) == Model.Namespaces);
  RC_ASSERT(Observed.Namespaces == Model.Namespaces.size());
  RC_ASSERT(LeadingFields(Observed.OrderedMetatables, 1) == Model.Classes);
  RC_ASSERT(Observed.Metatables == Model.Classes.size());
  RC_ASSERT(Observed.MetatableIdentities.size() == Model.Classes.size());
  RC_ASSERT(LeadingFields(Observed.OrderedModules, 2) == Model.Modules);
  RC_ASSERT(Observed.Modules == Model.Modules.size());

  std::vector<std::pair<std::string, std::size_t>> CachedCallables;
  for (const std::string &Text : Observed.OrderedOverloads) {
    CachedCallables.emplace_back(
        FieldAt(Text, 0),
        static_cast<std::size_t>(std::stoull(FieldAt(Text, 1))));
  }
  RC_ASSERT(CachedCallables == Model.Callables);
  RC_ASSERT(Observed.Overloads == Model.Callables.size());

  // The foundation conversion table is always prepared, and every declared
  // class adds at least its own canonical type; each declared base edge adds at
  // least one viable cast path.
  RC_ASSERT(Observed.Conversions >= 5 + Model.Classes.size());
  RC_ASSERT(Observed.OrderedConversions.size() == Observed.Conversions);
  RC_ASSERT(Observed.CastPaths >= Model.DerivedClasses);
  RC_ASSERT(Observed.OrderedCastPaths.size() == Observed.CastPaths);
}

// One uncached generation, compared with the same model.
void VerifyUncachedMatchesModel(const Luna::ReflectionSnapshot &Snapshot,
                                const ModelSurface &Model) {
  for (const auto &[QualifiedName, Kind] : Model.Declarations) {
    const Luna::ReflectionRecord Record = Snapshot.Find(QualifiedName);
    RC_ASSERT(Record.IsValid());
    RC_ASSERT(Record.QualifiedName() == QualifiedName);
    RC_ASSERT(Record.Kind() == Kind);
  }

  RC_ASSERT(OrderedNames(Snapshot.Symbols(Luna::SymbolKind::Namespace)) ==
            Model.Namespaces);
  RC_ASSERT(OrderedNames(Snapshot.Symbols(Luna::SymbolKind::Class)) ==
            Model.Classes);
  RC_ASSERT(Snapshot.Symbols(Luna::SymbolKind::OverloadSet).Size() ==
            Model.Callables.size());
  RC_ASSERT(Snapshot.Symbols(Luna::SymbolKind::FunctionCandidate).Size() ==
            CandidateTotal(Model));

  std::vector<std::string> ObservedModules;
  const Luna::ModuleRecordRange Modules = Snapshot.Modules();
  for (std::size_t Index = 0; Index < Modules.Size(); ++Index) {
    ObservedModules.push_back(std::string(Modules.At(Index).Identity()) + "|" +
                              std::string(Modules.At(Index).Version()));
  }
  std::sort(ObservedModules.begin(), ObservedModules.end());
  RC_ASSERT(ObservedModules == Model.Modules);
}

// The published cache, compared with the uncached lookups of exactly the
// generations its key names.
void VerifyCachedMatchesUncached(const Luna::State &Owner,
                                 const FreezeCacheObservation &Observed,
                                 const Luna::ReflectionSnapshot &Snapshot) {
  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();

  for (const std::string &Detail : Observed.LookupDetails) {
    const std::vector<std::string> Parts = Fields(Detail);
    RC_ASSERT(Parts.size() == 5);
    if (Parts[4] == "-") {
      // A private committed identity retains no reflected record at all, so it
      // can never alias one either.
      RC_ASSERT(Parts[1] == "type" || Parts[1] == "dispatch_target" ||
                Parts[1] == "metatable");
      continue;
    }
    const std::size_t Index = static_cast<std::size_t>(std::stoull(Parts[4]));
    RC_ASSERT(Index < Symbols.Size());
    const Luna::ReflectionRecord Record = Symbols.At(Index);
    RC_ASSERT(Record.IsValid());
    RC_ASSERT(Record.QualifiedName() == Parts[0]);
    RC_ASSERT(std::string(Luna::SymbolKindText(Record.Kind())) == Parts[2]);
    RC_ASSERT(Record.Id().ToString() == Parts[3]);
    RC_ASSERT(Snapshot.Find(Record.Id()).QualifiedName() == Parts[0]);
  }

  RC_ASSERT(Hooks::NamespaceOwnershipCount(Owner) == Observed.Namespaces);
  for (const std::string &Text : Observed.OrderedNamespaces) {
    const std::string Name = FieldAt(Text, 0);
    RC_ASSERT(Hooks::NamespaceIsOwned(Owner, Name));
    const Luna::ReflectionRecord Record = Snapshot.Find(Name);
    RC_ASSERT(Record.IsValid());
    RC_ASSERT(Record.Kind() == Luna::SymbolKind::Namespace);
    RC_ASSERT(Record.Id().ToString() == FieldAt(Text, 1));
  }

  RC_ASSERT(Hooks::RegisteredClassCount(Owner) == Observed.Metatables);
  for (std::size_t Index = 0; Index < Observed.OrderedMetatables.size();
       ++Index) {
    const std::string &Text = Observed.OrderedMetatables[Index];
    const std::string Name = FieldAt(Text, 0);
    RC_ASSERT(Hooks::ClassIsRegistered(Owner, Name));
    const auto Type = Hooks::ClassTypeOf(Owner, Name);
    RC_ASSERT(Type.has_value());
    RC_ASSERT(Type->ToString() == FieldAt(Text, 1));
    RC_ASSERT(
        Hooks::ClassMetatableIdentityOf(Owner, Name) ==
        std::optional<std::uint64_t>(Observed.MetatableIdentities[Index]));
    const Luna::ReflectionRecord Record = Snapshot.Find(Name);
    RC_ASSERT(Record.IsValid());
    RC_ASSERT(Record.Kind() == Luna::SymbolKind::Class);
    RC_ASSERT(Record.Id().ToString() == FieldAt(Text, 2));
  }

  RC_ASSERT(Hooks::LoadedModuleCount(Owner) == Observed.Modules);
  const Luna::ModuleRecordRange Modules = Snapshot.Modules();
  for (const std::string &Text : Observed.OrderedModules) {
    const std::string Identity = FieldAt(Text, 0);
    RC_ASSERT(Hooks::ModuleIsLoaded(Owner, Identity));
    RC_ASSERT(Hooks::LoadedModuleVersion(Owner, Identity) ==
              std::optional<std::string>(FieldAt(Text, 1)));
    bool Reflected = false;
    for (std::size_t Index = 0; Index < Modules.Size(); ++Index) {
      const Luna::ModuleRecord Module = Modules.At(Index);
      if (Module.Identity() != Identity)
        continue;
      RC_ASSERT(Module.Version() == FieldAt(Text, 1));
      RC_ASSERT(Module.Symbol().ToString() == FieldAt(Text, 2));
      const Luna::ReflectionRecord Record = Snapshot.Find(Module.Symbol());
      RC_ASSERT(Record.IsValid());
      RC_ASSERT(Record.Kind() == Luna::SymbolKind::Module);
      Reflected = true;
    }
    RC_ASSERT(Reflected);
  }

  RC_ASSERT(Hooks::BindingCount(Owner) == Observed.Overloads);
  for (const std::string &Text : Observed.OrderedOverloads) {
    const std::string Name = FieldAt(Text, 0);
    const std::size_t Count =
        static_cast<std::size_t>(std::stoull(FieldAt(Text, 1)));
    RC_ASSERT(Hooks::OverloadCandidateCount(Owner, Name) == Count);
    RC_ASSERT(Hooks::OverloadCandidateSignatures(Owner, Name).size() == Count);
    RC_ASSERT(Hooks::StagedOverloadCandidateCount(Owner, Name) == 0);

    // Every cached candidate index names one lookup entry of exactly this
    // callable path, and every one of those is a reflected candidate.
    std::size_t Named = 0;
    const std::string Indices = FieldAt(Text, 2);
    std::size_t Start = 0;
    while (Start < Indices.size()) {
      const std::size_t Separator = Indices.find(',', Start);
      const std::string Number = Separator == std::string::npos
                                     ? Indices.substr(Start)
                                     : Indices.substr(Start, Separator - Start);
      const std::size_t Position =
          static_cast<std::size_t>(std::stoull(Number));
      RC_ASSERT(Position < Observed.LookupDetails.size());
      const std::vector<std::string> Parts =
          Fields(Observed.LookupDetails[Position]);
      RC_ASSERT(Parts[0] == Name);
      RC_ASSERT(Parts[1] == "function");
      RC_ASSERT(Parts[2] == std::string(Luna::SymbolKindText(
                                Luna::SymbolKind::FunctionCandidate)));
      ++Named;
      if (Separator == std::string::npos)
        break;
      Start = Separator + 1;
    }
    RC_ASSERT(Named == Count);
  }

  // Every canonical type the uncached generation reflects is present in the
  // frozen conversion table.
  const Luna::TypeRecordRange Types = Snapshot.Types();
  for (std::size_t Index = 0; Index < Types.Size(); ++Index) {
    const std::string Identity = Types.At(Index).Id().ToString();
    RC_ASSERT(std::find(Observed.OrderedConversions.begin(),
                        Observed.OrderedConversions.end(),
                        Identity) != Observed.OrderedConversions.end());
  }
}

// Two States committed from the same input publish byte-identical caches, apart
// from the logical State identity and the state-local metatable identities
// neither one promises to share.
void VerifyEquivalentCaches(const FreezeCacheObservation &First,
                            const FreezeCacheObservation &Second) {
  RC_ASSERT(First.Published && Second.Published);
  RC_ASSERT(!(First.Key.State == Second.Key.State));
  RC_ASSERT(First.Key.Generation == Second.Key.Generation);
  RC_ASSERT(First.Key.ReflectionGeneration == Second.Key.ReflectionGeneration);
  RC_ASSERT(First.Key.TypeGeneration == Second.Key.TypeGeneration);
  RC_ASSERT(First.Key.LifecycleGeneration == Second.Key.LifecycleGeneration);

  RC_ASSERT(First.Lookups == Second.Lookups);
  RC_ASSERT(First.OrderedLookups == Second.OrderedLookups);
  RC_ASSERT(First.LookupDetails == Second.LookupDetails);
  RC_ASSERT(First.OrderedOverloads == Second.OrderedOverloads);
  RC_ASSERT(First.OrderedConversions == Second.OrderedConversions);
  RC_ASSERT(First.OrderedCastPaths == Second.OrderedCastPaths);
  RC_ASSERT(First.OrderedMetatables == Second.OrderedMetatables);
  RC_ASSERT(First.OrderedNamespaces == Second.OrderedNamespaces);
  RC_ASSERT(First.OrderedModules == Second.OrderedModules);
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// The generated query history, and the owner-thread runtime state one frozen
// State is still allowed to keep.
// ---------------------------------------------------------------------------

enum class QueryKind : std::uint8_t {
  FindDeclared,
  FindAbsent,
  EnumerateKind,
  RetainSnapshot,
  CallFunction,
  ExposeValue,
  ReadLazyMember,
  WriteMember,
  InvalidateCache,
  AccessValue,
  EnumerateRelationships,
  ObserveOverloads
};

const std::vector<std::string> &AbsentNames() {
  static const std::vector<std::string> Names{"Missing", "Ns9", "Widget9",
                                              "Gadget.Absent", "tests.cache9"};
  return Names;
}

[[nodiscard]] std::string ExposeGadget(Luna::State &Owner,
                                       const std::string &Path, Gadget &Object,
                                       const std::uint64_t *Lifetime) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Path = Path;
  Request.Storage = &Object;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.LifetimeGeneration = Lifetime;
  return Hooks::ExposeClassUserdata(Owner, Request).Status;
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
ReadGadgetMember(Luna::State &Owner, const std::string &Path,
                 const std::string &Member) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  return Hooks::ReadClassMemberValue(Owner, Request);
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
WriteGadgetMember(Luna::State &Owner, const std::string &Path,
                  const std::string &Member, int Value) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  Request.Incoming = Luna::Value(Value);
  return Hooks::WriteClassMemberValue(Owner, Request);
}

// One generated query history. Nothing here may change the committed model, so
// the generations, the binding count, and the reflected generation are compared
// before and after the whole history.
void TakeQueryHistory(ByteCursor &Cursor, Luna::State &Owner,
                      const ModelSurface &Model, Gadget &Probe,
                      const std::uint64_t *Lifetime,
                      std::vector<Luna::ReflectionSnapshot> &Retained) {
  const std::string Path = "History_0";
  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const auto Lifecycle = Hooks::LifecycleGenerationOf(Owner);
  bool Exposed = false;

  const std::size_t Count = Cursor.Pick(12);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    const Luna::ReflectionSnapshot Snapshot = Owner.Bindings().Reflection();
    switch (static_cast<QueryKind>(Cursor.Pick(12))) {
    case QueryKind::FindDeclared: {
      const auto &[Name, Kind] =
          Model.Declarations[Cursor.Pick(Model.Declarations.size())];
      const Luna::ReflectionRecord Record = Snapshot.Find(Name);
      RC_ASSERT(Record.IsValid());
      RC_ASSERT(Record.Kind() == Kind);
      break;
    }
    case QueryKind::FindAbsent: {
      const std::string &Name =
          AbsentNames()[Cursor.Pick(AbsentNames().size())];
      RC_ASSERT(!Snapshot.Find(Name).IsValid());
      break;
    }
    case QueryKind::EnumerateKind:
      static_cast<void>(Snapshot.Symbols(Luna::SymbolKind::Class).Size());
      static_cast<void>(Snapshot.Symbols(Luna::SymbolKind::Namespace).Size());
      static_cast<void>(Snapshot.Types().Size());
      break;
    case QueryKind::RetainSnapshot:
      Retained.push_back(Snapshot);
      break;
    case QueryKind::CallFunction: {
      if (Model.Callables.empty())
        break;
      const std::string &Name =
          Model.Callables[Cursor.Pick(Model.Callables.size())].first;
      RC_ASSERT(Owner.Execute("assert(" + Name + "(3) == 4)").IsSuccess());
      RC_ASSERT(Owner.Execute("assert(" + Name + "(3, 4) == 12)").IsSuccess());
      break;
    }
    case QueryKind::ExposeValue: {
      const std::string Status = ExposeGadget(Owner, Path, Probe, Lifetime);
      RC_ASSERT(Status == (Exposed ? "reused" : "created"));
      Exposed = true;
      break;
    }
    case QueryKind::ReadLazyMember: {
      const auto Observed = ReadGadgetMember(Owner, Path, "Level");
      RC_ASSERT(Observed.Reached == Exposed);
      break;
    }
    case QueryKind::WriteMember: {
      const auto Observed = WriteGadgetMember(
          Owner, Path, "Charge", static_cast<int>(Cursor.Pick(20)) + 1);
      RC_ASSERT(Observed.Reached == Exposed);
      break;
    }
    case QueryKind::InvalidateCache:
      static_cast<void>(Hooks::InvalidateClassMemberCache(Owner, Path));
      break;
    case QueryKind::AccessValue: {
      Luna::Detail::ClassAccessRequest Request;
      Request.QualifiedName = "Gadget";
      Request.Path = Path;
      Request.ExpectedStorage = &Probe;
      const auto Observed = Hooks::AccessClassUserdata(Owner, Request);
      RC_ASSERT(Observed.ReachedNativeCode == Exposed);
      break;
    }
    case QueryKind::EnumerateRelationships: {
      const std::string &Name =
          Model.Classes[Cursor.Pick(Model.Classes.size())];
      static_cast<void>(Hooks::ClassBases(Owner, Name).size());
      static_cast<void>(Hooks::ClassCasts(Owner, Name).size());
      static_cast<void>(Hooks::ClassInheritedMembers(Owner, Name).size());
      break;
    }
    case QueryKind::ObserveOverloads: {
      if (Model.Callables.empty())
        break;
      const auto &[Name, Expected] =
          Model.Callables[Cursor.Pick(Model.Callables.size())];
      RC_ASSERT(Hooks::OverloadCandidateCount(Owner, Name) == Expected);
      break;
    }
    }
  }

  // A query history is exactly that: no query committed anything.
  RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);
  RC_ASSERT(Hooks::ReflectionGeneration(Owner) == Reflected);
  RC_ASSERT(Hooks::BindingCount(Owner) == Bindings);
  RC_ASSERT(Hooks::LifecycleGenerationOf(Owner) == Lifecycle);
  RC_ASSERT(!Hooks::IsFrozen(Owner));
  RC_ASSERT(!Hooks::ObserveFreezeCache(Owner).Published);
}

// Every injected preparation failure leaves the Ready State exactly as it was
// and publishes nothing at all.
void VerifyFailedFreezesChangeNothing(Luna::State &Owner,
                                      Luna::BindingRegistry &Registry,
                                      std::size_t Attempts) {
  if (Attempts == 0)
    return;

  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const std::size_t Classes = Hooks::RegisteredClassCount(Owner);
  const std::size_t Modules = Hooks::LoadedModuleCount(Owner);
  const std::size_t Namespaces = Hooks::NamespaceOwnershipCount(Owner);
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  std::string Reported;

  for (std::size_t Attempt = 0; Attempt < Attempts; ++Attempt) {
    Hooks::InjectFault(Owner, StateFaultPoint::FreezePreparation);
    const Luna::RegistrationResult Failed = Registry.Freeze();
    RC_ASSERT(!Failed.IsSuccess());
    RC_ASSERT(Failed.Diagnostic() != nullptr);
    RC_ASSERT(Failed.Diagnostic()->Category() == Luna::ErrorCategory::Internal);
    RC_ASSERT(Failed.Diagnostic()->Message().find("allocation_failure") !=
              std::string::npos);
    if (Attempt == 0)
      Reported = Failed.Diagnostic()->Message();
    RC_ASSERT(Failed.Diagnostic()->Message() == Reported);

    RC_ASSERT(!Hooks::IsFrozen(Owner));
    RC_ASSERT(!Hooks::ObserveFreezeCache(Owner).Published);
    RC_ASSERT(Owner.IsReady());
    RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);
    RC_ASSERT(Hooks::ReflectionGeneration(Owner) == Reflected);
    RC_ASSERT(Hooks::BindingCount(Owner) == Bindings);
    RC_ASSERT(Hooks::RegisteredClassCount(Owner) == Classes);
    RC_ASSERT(Hooks::LoadedModuleCount(Owner) == Modules);
    RC_ASSERT(Hooks::NamespaceOwnershipCount(Owner) == Namespaces);
    RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == Depth);
  }
  RC_ASSERT(Hooks::PendingFaults(Owner, StateFaultPoint::FreezePreparation) ==
            0);
}

// Every repeated freeze returns one identical already-frozen result and
// republishes nothing.
void VerifyRepeatedFreezeIsDeterministic(Luna::State &Owner,
                                         Luna::BindingRegistry &Registry,
                                         const FreezeCacheObservation &Cached,
                                         std::size_t Attempts) {
  std::string Reported;
  for (std::size_t Attempt = 0; Attempt < Attempts; ++Attempt) {
    const Luna::RegistrationResult Repeated = Registry.Freeze();
    RC_ASSERT(!Repeated.IsSuccess());
    RC_ASSERT(Repeated.Diagnostic() != nullptr);
    RC_ASSERT(Repeated.Diagnostic()->Category() ==
              Luna::ErrorCategory::StateNotReady);
    RC_ASSERT(Repeated.Diagnostic()->Message().find("already frozen") !=
              std::string::npos);
    if (Attempt == 0)
      Reported = Repeated.Diagnostic()->Message();
    RC_ASSERT(Repeated.Diagnostic()->Message() == Reported);

    const FreezeCacheObservation Again = Hooks::ObserveFreezeCache(Owner);
    RC_ASSERT(Again.Address == Cached.Address);
    RC_ASSERT(Again.LookupDetails == Cached.LookupDetails);
    RC_ASSERT(Again.MetatableIdentities == Cached.MetatableIdentities);
  }
}

// A foreign thread is refused before mutation, whether it asks to freeze or to
// register.
void VerifyForeignThreadNeverMutates(Luna::State &Owner) {
  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);

  Luna::RegistrationResult Frozen = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Registered = Luna::RegistrationResult::Success();
  std::thread Foreign([&Owner, &Frozen, &Registered] {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Frozen = Registry.Freeze();
    Registered = Registry.Register("ForeignThreadName", [] { return 1; });
  });
  Foreign.join();

  RC_ASSERT(!Frozen.IsSuccess());
  RC_ASSERT(Frozen.Diagnostic() != nullptr);
  RC_ASSERT(Frozen.Diagnostic()->Category() ==
            Luna::ErrorCategory::StateNotReady);
  RC_ASSERT(Frozen.Diagnostic()->Message().find("owner thread") !=
            std::string::npos);
  RC_ASSERT(!Registered.IsSuccess());
  RC_ASSERT(Registered.Diagnostic() != nullptr);
  RC_ASSERT(Registered.Diagnostic()->Category() ==
            Luna::ErrorCategory::StateNotReady);
  RC_ASSERT(Registered.Diagnostic()->Message().find("owner thread") !=
            std::string::npos);

  RC_ASSERT(!Hooks::IsFrozen(Owner));
  RC_ASSERT(!Hooks::ObserveFreezeCache(Owner).Published);
  RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);
  RC_ASSERT(Hooks::ReflectionGeneration(Owner) == Reflected);
  RC_ASSERT(Hooks::BindingCount(Owner) == Bindings);
  RC_ASSERT(!Hooks::BindingIsCommitted(Owner, "ForeignThreadName"));
  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == Depth);
}

// After freeze, the documented owner-thread runtime state still follows its
// owners: a dispatch-generation change makes every earlier lazy value
// unreachable, and retiring one exposed value withdraws its entries and its
// live identity before that value becomes unavailable. None of it republishes
// or mutates the frozen caches.
void VerifyInvalidationPrecedesUnavailability(
    Luna::State &Owner, const FreezeCacheObservation &Cached, Gadget &Probe,
    const std::uint64_t *Lifetime) {
  // The generated query history may already have exposed a value of its own, so
  // every count here is compared against what this State owned beforehand.
  const std::string Path = "Frozen_0";
  const std::size_t Identities = Hooks::LiveCachedIdentityCount(Owner);
  const std::size_t LiveEntries = Hooks::LiveLazyMemberCacheEntryCount(Owner);
  RC_ASSERT(ExposeGadget(Owner, Path, Probe, Lifetime) == "created");
  RC_ASSERT(Hooks::LiveCachedIdentityCount(Owner) == Identities + 1);

  const auto First = ReadGadgetMember(Owner, Path, "Level");
  RC_ASSERT(First.Reached);
  RC_ASSERT(First.Recorded);
  RC_ASSERT(!First.ServedFromCache);
  RC_ASSERT(Hooks::LiveLazyMemberCacheEntryCount(Owner) == LiveEntries + 1);

  const auto Reused = ReadGadgetMember(Owner, Path, "Level");
  RC_ASSERT(Reused.Reached);
  RC_ASSERT(Reused.ServedFromCache);

  // A dispatch-generation change invalidates by mismatch: the entry is still
  // owned by Luna, and it is no longer reachable by any access.
  RC_ASSERT(Hooks::AdvanceLifecycleGeneration(Owner));
  RC_ASSERT(Hooks::LazyMemberCacheEntryCountOf(Owner, &Probe) == 1);
  RC_ASSERT(Hooks::LiveLazyMemberCacheEntryCount(Owner) == 0);
  const auto AfterGeneration = ReadGadgetMember(Owner, Path, "Level");
  RC_ASSERT(AfterGeneration.Reached);
  RC_ASSERT(!AfterGeneration.ServedFromCache);
  RC_ASSERT(AfterGeneration.Recorded);
  RC_ASSERT(Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1);

  // Retirement invalidates before the value becomes unavailable.
  RC_ASSERT(Hooks::RetireClassUserdata(Owner, &Probe));
  RC_ASSERT(Hooks::LazyMemberCacheEntryCountOf(Owner, &Probe) == 0);
  RC_ASSERT(Hooks::LiveCachedIdentityCount(Owner) == Identities);
  const auto Refused = ReadGadgetMember(Owner, Path, "Level");
  RC_ASSERT(!Refused.Reached);
  RC_ASSERT(Refused.Receiver == "invalidated");
  RC_ASSERT(Refused.Boundary == "before_user_code");

  // Registration stays refused while frozen, and nothing above republished or
  // changed one cached entry.
  const Luna::RegistrationResult Rejected =
      Owner.Bindings().Register("AfterFreeze", [] { return 5; });
  RC_ASSERT(!Rejected.IsSuccess());
  RC_ASSERT(!Hooks::BindingIsCommitted(Owner, "AfterFreeze"));

  const FreezeCacheObservation Observed = Hooks::ObserveFreezeCache(Owner);
  RC_ASSERT(Observed.Address == Cached.Address);
  RC_ASSERT(Observed.LookupDetails == Cached.LookupDetails);
  RC_ASSERT(Observed.OrderedOverloads == Cached.OrderedOverloads);
  RC_ASSERT(Observed.OrderedConversions == Cached.OrderedConversions);
  RC_ASSERT(Observed.OrderedCastPaths == Cached.OrderedCastPaths);
  RC_ASSERT(Observed.OrderedMetatables == Cached.OrderedMetatables);
  RC_ASSERT(Observed.MetatableIdentities == Cached.MetatableIdentities);
  RC_ASSERT(Observed.OrderedNamespaces == Cached.OrderedNamespaces);
  RC_ASSERT(Observed.OrderedModules == Cached.OrderedModules);
}

} // namespace

int RunFrozenCacheEquivalenceProperties() {
  // **Validates: Requirements 15.1, 15.2, 15.4, 15.5, 15.7, 15.8, 15.9**
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 30: Frozen caches are equivalent to uncached generation lookups
  const bool Passed = rc::check(
      // clang-format on
      "Frozen caches are equivalent to uncached generation lookups",
      [](const std::vector<std::uint8_t> &Shape,
         const std::vector<std::uint8_t> &History) {
        ByteCursor Plan(Shape);
        ByteCursor Actions(History);

        // One generated committed input: independent units with distinct
        // ordinals, so any permutation of the list is the same input.
        std::vector<Unit> Units;
        std::vector<std::size_t> Available{0, 1, 2, 3};
        const std::size_t Count = 1 + Plan.Pick(4);
        for (std::size_t Index = 0; Index < Count; ++Index) {
          const std::size_t Position = Plan.Pick(Available.size());
          Unit Declared;
          Declared.Ordinal = Available[Position];
          Available.erase(Available.begin() +
                          static_cast<std::ptrdiff_t>(Position));
          switch (Plan.Pick(4)) {
          case 0:
            Declared.Kind = UnitKind::Callable;
            break;
          case 1:
            Declared.Kind = UnitKind::Scope;
            break;
          case 2:
            Declared.Kind = UnitKind::Class;
            break;
          default:
            Declared.Kind = UnitKind::Module;
            break;
          }
          Units.push_back(Declared);
        }

        std::vector<Unit> Permuted(Units.rbegin(), Units.rend());
        std::rotate(Permuted.begin(),
                    Permuted.begin() +
                        static_cast<std::ptrdiff_t>(Plan.Pick(Permuted.size())),
                    Permuted.end());

        const ModelSurface Model = ModelOf(Units);
        RC_ASSERT(ModelOf(Permuted).Declarations == Model.Declarations);

        const std::size_t FailedAttempts = Plan.Pick(3);
        const std::size_t RepeatedFreezes = 1 + Plan.Pick(2);

        // The generated objects outlive both States, so a borrowed value never
        // names storage that moved or was released early.
        Gadget HistoryProbe;
        Gadget FrozenProbe;
        std::uint64_t HistoryLifetime = 1;
        std::uint64_t FrozenLifetime = 1;

        std::vector<Luna::ReflectionSnapshot> Retained;
        FreezeCacheObservation FirstCache;
        FreezeCacheObservation SecondCache;

        {
          Luna::State First;
          RC_ASSERT(First.IsReady());
          Luna::BindingRegistry Registry = First.Bindings();
          RC_ASSERT(RegisterPrelude(First).IsSuccess());
          for (const Unit &Declared : Units)
            RC_ASSERT(RegisterUnit(First, Declared).IsSuccess());

          const std::vector<std::string> BeforeFreeze =
              OrderedNames(Registry.Reflection().Symbols());
          TakeQueryHistory(Actions, First, Model, HistoryProbe,
                           &HistoryLifetime, Retained);

          VerifyFailedFreezesChangeNothing(First, Registry, FailedAttempts);
          RC_ASSERT(Registry.Freeze().IsSuccess());
          FirstCache = Hooks::ObserveFreezeCache(First);

          const Luna::ReflectionSnapshot Frozen = Registry.Reflection();
          RC_ASSERT(OrderedNames(Frozen.Symbols()) == BeforeFreeze);
          VerifyCacheMatchesModel(First, FirstCache, Model);
          VerifyUncachedMatchesModel(Frozen, Model);
          VerifyCachedMatchesUncached(First, FirstCache, Frozen);
          for (const std::string &Absent : AbsentNames()) {
            RC_ASSERT(!Frozen.Find(Absent).IsValid());
            RC_ASSERT(!CachesLookupNamed(FirstCache, Absent));
          }

          VerifyRepeatedFreezeIsDeterministic(First, Registry, FirstCache,
                                              RepeatedFreezes);
          VerifyInvalidationPrecedesUnavailability(
              First, FirstCache, FrozenProbe, &FrozenLifetime);
          Retained.push_back(Registry.Reflection());
        }

        {
          Luna::State Second;
          RC_ASSERT(Second.IsReady());
          Luna::BindingRegistry Registry = Second.Bindings();
          RC_ASSERT(RegisterPrelude(Second).IsSuccess());
          for (const Unit &Declared : Permuted)
            RC_ASSERT(RegisterUnit(Second, Declared).IsSuccess());

          VerifyForeignThreadNeverMutates(Second);
          RC_ASSERT(Registry.Freeze().IsSuccess());
          SecondCache = Hooks::ObserveFreezeCache(Second);

          const Luna::ReflectionSnapshot Frozen = Registry.Reflection();
          VerifyCacheMatchesModel(Second, SecondCache, Model);
          VerifyUncachedMatchesModel(Frozen, Model);
          VerifyCachedMatchesUncached(Second, SecondCache, Frozen);
        }

        // Equivalent committed input, whatever order it was committed in and
        // whatever was queried first, freezes into the same cache.
        VerifyEquivalentCaches(FirstCache, SecondCache);

        // Every snapshot retained before freeze survives both States and still
        // reports exactly the surface the model predicted.
        for (const Luna::ReflectionSnapshot &Snapshot : Retained)
          VerifyUncachedMatchesModel(Snapshot, Model);

        if (FailedAttempts != 0 && RepeatedFreezes > 1)
          RC_TAG("freeze: preparation failed, recovered, and repeated");
        else if (FailedAttempts != 0)
          RC_TAG("freeze: preparation failed and recovered");
        else
          RC_TAG("freeze: prepared and published on the first attempt");
      });

  return Passed ? 0 : 1;
}
