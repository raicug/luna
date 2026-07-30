// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <iostream>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "enumerator object check failed: " << Description << '\n';
}

enum class Material { Wood = 0, Stone = 1, Metal = 2 };

[[nodiscard]] Luna::StableTypeKey MaterialKey() {
  return Luna::StableTypeKey("Studio.ObjectMaterial");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::EnumBuilder<Material> Enumeration =
      Studio.RegisterEnum<Material>("Material", MaterialKey());
  const bool Enumerated = Enumeration.AsObjects()
                              .Value("Wood", Material::Wood)
                              .Value("Stone", Material::Stone)
                              .Value("Metal", Material::Metal)
                              .Alias("Default", "Wood")
                              .Commit()
                              .IsSuccess();
  if (!Enumerated)
    return false;

  // A constant of the enumeration reaches a script as the very same
  // enumerator object its table publishes.
  Luna::NamespaceBuilder Constants = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &Staged =
      Constants.RegisterConstant("Heaviest", Material::Metal, MaterialKey());
  static_cast<void>(Staged.QualifiedName());
  return Constants.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "enumerator object source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckEnumeratorsArePublishedAsObjects() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "assert(typeof(Studio.Material.Stone) == 'EnumItem')"),
        "an enumerator reports its own type rather than a table or a number");
  Check(Succeeds(Owner, "assert(type(Studio.Material.Stone) == 'userdata')"),
        "an enumerator is a real value of its own, not a table");

  Check(Succeeds(Owner, "local Item = Studio.Material.Stone\n"
                        "assert(Item.Name == 'Stone', 'name')\n"
                        "assert(Item.Value == 1, 'value')\n"
                        "assert(Item.EnumName == 'Studio.Material', 'enum')"),
        "an enumerator names itself, its numeric value, and its enumeration");
  Check(Succeeds(Owner, "assert(tostring(Studio.Material.Metal) == "
                        "'Studio.Material.Metal')"),
        "an enumerator describes itself as its qualified name");

  Check(
      Succeeds(Owner, "assert(Studio.Material.Stone == Studio.Material.Stone)"),
      "the same enumerator read twice is one interned value");
  Check(
      Succeeds(Owner, "assert(Studio.Material.Stone ~= Studio.Material.Wood)"),
      "two enumerators of one enumeration are never equal");
  Check(Succeeds(Owner,
                 "assert(Studio.Material.Default == Studio.Material.Wood)"),
        "an alias reads the very enumerator object it names");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every enumerator read restores the entry stack depth");
}

void CheckEnumerationConstantsPublishTheSameObject() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "assert(typeof(Studio.Heaviest) == 'EnumItem')"),
        "a constant of the enumeration publishes an enumerator object");
  Check(Succeeds(Owner, "assert(Studio.Heaviest == Studio.Material.Metal)"),
        "a constant of the enumeration is the very object its table "
        "publishes, whichever installed first");
  Check(Succeeds(Owner, "assert(Studio.Heaviest.Name == 'Metal')"),
        "the constant's enumerator names itself the same way");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing an enumeration constant restores the entry stack depth");
}

void CheckEnumeratorObjectsAreImmutable() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");

  Check(!Owner.Execute("Studio.Material.Stone.Value = 9").IsSuccess(),
        "an enumerator refuses a write to a field it publishes");
  Check(!Owner.Execute("Studio.Material.Stone.Extra = 9").IsSuccess(),
        "an enumerator refuses a new field");
  Check(!Owner.Execute("setmetatable(Studio.Material.Stone, {})").IsSuccess(),
        "an enumerator's metatable cannot be replaced");
  Check(Succeeds(Owner, "assert(Studio.Material.Stone.Value == 1)"),
        "every refused write leaves the enumerator unchanged");
  Check(!Owner.Execute("Studio.Material.Stone = 5").IsSuccess(),
        "the enumeration table itself stays immutable");
}

void CheckNumericEnumerationsKeepTheirRepresentation() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::EnumBuilder<Material> Enumeration = Registry.RegisterEnum<Material>(
      "Numeric", Luna::StableTypeKey("Studio.NumericMaterial"));
  Check(Enumeration.Value("Wood", Material::Wood)
            .Value("Stone", Material::Stone)
            .Commit()
            .IsSuccess(),
        "an enumeration without the object opt-in publishes");
  Check(Succeeds(Owner, "assert(Numeric.Stone == 1)"),
        "an enumeration that did not opt in still publishes bare numbers");
  Check(Succeeds(Owner, "assert(type(Numeric.Stone) == 'number')"),
        "the numeric representation is unchanged by the new opt-in");
}

} // namespace

int RunEnumeratorObjectTests();

int RunEnumeratorObjectTests() {
  FailureCount = 0;
  CheckEnumeratorsArePublishedAsObjects();
  CheckEnumerationConstantsPublishTheSameObject();
  CheckEnumeratorObjectsAreImmutable();
  CheckNumericEnumerationsKeepTheirRepresentation();
  return FailureCount == 0 ? 0 : 1;
}
