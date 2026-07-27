// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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
  std::cerr << "constant and enumeration check failed: " << Description << '\n';
}

enum class Alignment { Left = 0, Center = 1, Right = 2 };
enum class Narrow : signed char { Small = 1 };
enum class Access : int { None = 0, Read = 1, Write = 2, Execute = 4 };
enum Unscoped { First = 1, Second = 2 };

enum class Wide : long long { Zero = 0 };
enum class Mask : unsigned char { None = 0 };

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

[[nodiscard]] Luna::StableTypeKey AlignmentKey() {
  return Luna::StableTypeKey("Studio.Alignment");
}

[[nodiscard]] Luna::RegistrationResult
RegisterAlignment(Luna::BindingRegistry &Registry) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::EnumBuilder<Alignment> Enumeration =
      Studio.RegisterEnum<Alignment>("Alignment", AlignmentKey());
  return Enumeration.Value("Left", Alignment::Left)
      .Value("Center", Alignment::Center)
      .Value("Right", Alignment::Right)
      .Documentation("Horizontal alignment.")
      .Documentation("Center", "Centered content.")
      .Attribute("Center", "Default", "true")
      .Commit();
}

void CheckConstantsPublishAndReflect() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner)
                             ? *Hooks::ObserveRootStackDepth(Owner)
                             : -1;

  Check(Registry.RegisterConstant("Version", 7).IsSuccess(),
        "a root-scope integer constant commits immediately");
  Check(Registry.RegisterConstant("Name", "Luna").IsSuccess(),
        "a root-scope text constant commits immediately");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Check(Studio.RegisterConstant("Pi", 3.5)
            .RegisterConstant("Enabled", true)
            .Commit()
            .IsSuccess(),
        "namespace constants commit with their plan");

  Check(Owner.Execute("assert(Version == 7)").IsSuccess() &&
            Owner.Execute("assert(Name == 'Luna')").IsSuccess() &&
            Owner.Execute("assert(Studio.Pi == 3.5)").IsSuccess() &&
            Owner.Execute("assert(Studio.Enabled == true)").IsSuccess(),
        "every published constant is readable from a script");
  Check(Hooks::ObserveRootStackDepth(Owner) &&
            *Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing constants restores the exact entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Pi = Snapshot.Find("Studio.Pi");
  Check(Pi.IsValid() && Pi.Kind() == Luna::SymbolKind::Constant,
        "a constant reflects one constant record");
  Check(Pi.Name() == "Pi" && Pi.QualifiedName() == "Studio.Pi",
        "a constant record keeps its local and qualified names");
  Check(Pi.Scope().Owner() == Snapshot.Find("Studio").Id(),
        "a namespace constant is reflected inside its namespace scope");
  Check(Pi.HasValue() && Pi.ValueText() == "3.5",
        "a constant reflects its value availability and canonical text");
  Check(Pi.Type().IsValid() &&
            Pi.Descriptor().FixedKey() == Luna::FixedTypeKey::Double,
        "a constant reflects its canonical type");
  Check(Snapshot.Find("Studio.Enabled").Documentation().empty(),
        "an undocumented constant reflects no documentation");
}

void CheckUnsupportedAndOutOfRangeConstantsAreRefused() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto OutOfRange = Registry.RegisterConstant(
      "Huge", static_cast<std::int64_t>(5000000000LL));
  Check(!OutOfRange.IsSuccess(),
        "a constant outside the canonical integer domain is refused");
  Check(OutOfRange.Diagnostic() && OutOfRange.Diagnostic()->Message().find(
                                       "5000000000") != std::string::npos,
        "an out-of-range constant reports the value it received");
  Check(PathKind(Owner, "Huge") == "absent",
        "a refused constant installs nothing");

  const auto MissingKey = Registry.RegisterConstant("Default", Alignment::Left);
  Check(!MissingKey.IsSuccess(),
        "an enumeration constant without its stable key is refused");

  const auto Unregistered =
      Registry.RegisterConstant("Default", Alignment::Left, AlignmentKey());
  Check(!Unregistered.IsSuccess(),
        "a constant of an unregistered enumeration has no available type");
  Check(Registry.Reflection().IsEmpty(),
        "no refused constant contributes a reflection record");
  Check(Registry.RegisterConstant("Version", 1).IsSuccess(),
        "the State stays usable after refused constants");
}

void CheckScopedEnumerationPublishesTypedTable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Check(RegisterAlignment(Registry).IsSuccess(),
        "one scoped enumeration commits with its namespace");
  Check(PathKind(Owner, "Studio.Alignment") == "table",
        "an enumeration installs one table at its exact path");
  Check(Owner.Execute("assert(Studio.Alignment.Left == 0)").IsSuccess() &&
            Owner.Execute("assert(Studio.Alignment.Center == 1)").IsSuccess() &&
            Owner.Execute("assert(Studio.Alignment.Right == 2)").IsSuccess(),
        "every enumerator is readable through the published table");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Enumeration = Snapshot.Find("Studio.Alignment");
  Check(Enumeration.IsValid() &&
            Enumeration.Kind() == Luna::SymbolKind::Enumeration,
        "an enumeration reflects one enumeration record");
  Check(Enumeration.Documentation() == "Horizontal alignment.",
        "an enumeration reflects its documentation");
  Check(Enumeration.Descriptor().Kind() == Luna::TypeKind::Enumeration &&
            Enumeration.Descriptor().Key() == AlignmentKey(),
        "an enumeration reflects its canonical enumeration type");
  Check(Snapshot.FindType(Enumeration.Type()).IsValid(),
        "an enumeration contributes one canonical type to the generation");

  const Luna::ReflectionRecord Center =
      Snapshot.Find("Studio.Alignment.Center");
  Check(Center.IsValid() && Center.Kind() == Luna::SymbolKind::Enumerator,
        "each canonical enumerator reflects one enumerator record");
  Check(Center.Scope().Owner() == Enumeration.Id(),
        "an enumerator is reflected inside its enumeration scope");
  Check(Center.Type() == Enumeration.Type(),
        "an enumerator keeps the enumeration's canonical type identity");
  Check(Center.HasValue() && Center.ValueText() == "1",
        "an enumerator reflects its value availability and canonical text");
  Check(Center.Documentation() == "Centered content.",
        "an enumerator reflects its own documentation");
  Check(Center.AttributeCount() == 1 &&
            Center.Attribute(0).Name() == "Default" &&
            Center.Attribute(0).Value() == "true",
        "an enumerator reflects its own attributes");

  const Luna::ReflectionRecordRange Members =
      Snapshot.Symbols(Luna::ScopeId(Enumeration.Id()));
  Check(Members.Size() == 3, "the enumeration scope holds its three members");
  Check(Members.At(0).Name() == "Center" && Members.At(1).Name() == "Left" &&
            Members.At(2).Name() == "Right",
        "enumeration members are enumerated in canonical name order");
}

void CheckEnumerationTableRefusesScriptWrites() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterAlignment(Registry).IsSuccess(), "the enumeration publishes");
  const std::uint64_t Generation = Hooks::ReflectionGeneration(Owner);

  const auto Overwrite = Owner.Execute("Studio.Alignment.Left = 5");
  Check(!Overwrite.IsSuccess(),
        "a supported script write to an enumeration table fails");
  Check(Overwrite.Diagnostic() && Overwrite.Diagnostic()->Message().find(
                                      "immutable") != std::string::npos,
        "a refused write reports a deterministic immutable-value diagnostic");
  Check(Owner.Execute("assert(Studio.Alignment.Left == 0)").IsSuccess(),
        "a refused write leaves the enumeration table unchanged");

  Check(!Owner.Execute("Studio.Alignment.Extra = 9").IsSuccess(),
        "adding a new field to an enumeration table fails");
  Check(Owner.Execute("assert(Studio.Alignment.Extra == nil)").IsSuccess(),
        "a refused new field is never stored");
  Check(!Owner.Execute("setmetatable(Studio.Alignment, {})").IsSuccess(),
        "the protected metatable cannot be replaced");
  Check(
      Owner.Execute("assert(type(getmetatable(Studio.Alignment)) == 'string')")
          .IsSuccess(),
      "the private backing storage is not reachable through the metatable");
  Check(Hooks::ReflectionGeneration(Owner) == Generation,
        "a refused write leaves Luna's metadata unchanged");
}

void CheckDuplicateNamesValuesAndAliases() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    const auto Result = Enumeration.Value("Left", Alignment::Left)
                            .Value("Left", Alignment::Right)
                            .Commit();
    Check(!Result.IsSuccess(), "a duplicate enumerator name is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
          "a duplicate enumerator name is a deterministic collision");
    Check(PathKind(Owner, "Alignment") == "absent",
          "a refused enumeration installs no table");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    const auto Result = Enumeration.Value("Left", Alignment::Left)
                            .Value("Start", Alignment::Left)
                            .Commit();
    Check(!Result.IsSuccess(),
          "a duplicate numeric value without an alias is refused");
    Check(Result.Diagnostic() &&
              Result.Diagnostic()->Message().find("alias") != std::string::npos,
          "a duplicate value points at the explicit alias declaration");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    Check(Enumeration.Value("Left", Alignment::Left)
              .Value("Right", Alignment::Right)
              .Alias("Start", "Left")
              .Alias("End", "Right")
              .Commit()
              .IsSuccess(),
          "an explicit alias of a canonical enumerator is accepted");
    Check(
        Owner.Execute("assert(Alignment.Start == Alignment.Left)").IsSuccess(),
        "an alias exposes the canonical enumerator's value");

    const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
    const Luna::ReflectionRecord Canonical = Snapshot.Find("Alignment.Left");
    const Luna::ReflectionRecord Alias = Snapshot.Find("Alignment.Start");
    Check(Alias.IsValid() && Alias.Kind() == Luna::SymbolKind::EnumeratorAlias,
          "an alias reflects one alias record");
    Check(Alias.Declaration() == Canonical.Id(),
          "an alias record preserves its canonical enumerator");
    Check(Alias.HasValue() && Alias.ValueText() == Canonical.ValueText(),
          "an alias reflects the canonical enumerator's value");

    const Luna::ReflectionRecordRange Members =
        Snapshot.Symbols(Luna::ScopeId(Snapshot.Find("Alignment").Id()));
    Check(Members.Size() == 4,
          "the enumeration scope holds both canonical enumerators and aliases");
    Check(Members.At(0).Name() == "End" && Members.At(1).Name() == "Left" &&
              Members.At(2).Name() == "Right" &&
              Members.At(3).Name() == "Start",
          "aliases are enumerated together with enumerators in canonical name "
          "order");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    const auto Result = Enumeration.Value("Left", Alignment::Left)
                            .Alias("Start", "Middle")
                            .Commit();
    Check(!Result.IsSuccess(),
          "an alias of an undeclared enumerator is refused");
  }
}

void CheckUnscopedEnumerationsRequireOptIn() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Unscoped> Enumeration = Registry.RegisterEnum<Unscoped>(
        "Legacy", Luna::StableTypeKey("Studio.Legacy"));
    const auto Result = Enumeration.Value("First", First).Commit();
    Check(!Result.IsSuccess(),
          "an unscoped enumeration without its opt-in is refused");
    Check(PathKind(Owner, "Legacy") == "absent",
          "a refused unscoped enumeration installs nothing");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Unscoped> Enumeration = Registry.RegisterEnum<Unscoped>(
        "Legacy", Luna::StableTypeKey("Studio.Legacy"));
    Check(Enumeration.AllowUnscoped()
              .Value("First", First)
              .Value("Second", Second)
              .Commit()
              .IsSuccess(),
          "an unscoped enumeration with its explicit opt-in is accepted");
    Check(Owner.Execute("assert(Legacy.Second == 2)").IsSuccess(),
          "an opted-in unscoped enumeration publishes its enumerators");
  }
}

void CheckBitflagsRequireDeclarationAndRejectUnsupportedBits() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Access> Enumeration = Registry.RegisterEnum<Access>(
        "Access", Luna::StableTypeKey("Studio.Access"));
    Check(Enumeration.Value("None", Access::None)
              .Value("Read", Access::Read)
              .Value("Write", Access::Write)
              .Bitflags(static_cast<std::int64_t>(3))
              .Commit()
              .IsSuccess(),
          "an explicit bitflag declaration with a declared mask is accepted");

    const Luna::ReflectionRecord Reflected =
        Registry.Reflection().Find("Access");
    bool DeclaresFlags = false;
    for (std::size_t Index = 0; Index < Reflected.AttributeCount(); ++Index) {
      if (Reflected.Attribute(Index).Name() == "Bitflags" &&
          Reflected.Attribute(Index).Value() == "3")
        DeclaresFlags = true;
    }
    Check(DeclaresFlags,
          "a bitflag enumeration reflects its declared supported bits");

    Check(Registry
              .RegisterConstant("Full", static_cast<Access>(3),
                                Luna::StableTypeKey("Studio.Access"))
              .IsSuccess(),
          "a combination of declared supported bits converts");
    const auto Unsupported = Registry.RegisterConstant(
        "All", static_cast<Access>(7), Luna::StableTypeKey("Studio.Access"));
    Check(!Unsupported.IsSuccess(),
          "a value carrying an unsupported bit is refused");
    Check(Unsupported.Diagnostic() && Unsupported.Diagnostic()->Message().find(
                                          "truncate") != std::string::npos,
          "an unsupported bit is refused without truncation");
    Check(PathKind(Owner, "All") == "absent",
          "a refused bitflag constant installs nothing");
    Check(Owner.Execute("assert(Full == 3)").IsSuccess(),
          "the accepted combination stays published");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Access> Enumeration = Registry.RegisterEnum<Access>(
        "Access", Luna::StableTypeKey("Studio.Access"));
    const auto Result = Enumeration.Value("Read", Access::Read)
                            .Value("Execute", Access::Execute)
                            .Bitflags(static_cast<std::int64_t>(3))
                            .Commit();
    Check(!Result.IsSuccess(),
          "a declared enumerator outside the declared mask is refused");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Access> Enumeration = Registry.RegisterEnum<Access>(
        "Access", Luna::StableTypeKey("Studio.Access"));
    Check(Enumeration.Value("Read", Access::Read)
              .Value("Write", Access::Write)
              .Commit()
              .IsSuccess(),
          "an enumeration without bitflag behavior publishes");
    Check(!Registry
               .RegisterConstant("Both", static_cast<Access>(3),
                                 Luna::StableTypeKey("Studio.Access"))
               .IsSuccess(),
          "a combination is not a value of a non-bitflag enumeration");
  }
}

void CheckOutOfRangeEnumeratorsAreRefused() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Narrow> Enumeration = Registry.RegisterEnum<Narrow>(
        "Narrow", Luna::StableTypeKey("Studio.Narrow"));
    const auto Result = Enumeration.Value("Small", Narrow::Small)
                            .Value("TooLarge", static_cast<std::int64_t>(300))
                            .Commit();
    Check(!Result.IsSuccess(),
          "a value outside the declared C++ underlying type is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "[-128, 127]") != std::string::npos,
          "the refusal reports the received value and the permitted range");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "narrows") != std::string::npos,
          "the refusal states that Luna never narrows a declared value");
    Check(PathKind(Owner, "Narrow") == "absent",
          "a refused enumeration installs no table");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    const auto Result =
        Enumeration.Value("Left", Alignment::Left)
            .Value("Beyond", static_cast<std::int64_t>(4294967296LL))
            .Commit();
    Check(!Result.IsSuccess(),
          "a value outside the exact-integer domain Luna converts through is "
          "refused");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Alignment> Enumeration =
        Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
    Check(!Enumeration.Documentation("Empty.").Commit().IsSuccess(),
          "an enumeration without any canonical enumerator is refused");
  }
}

void CheckEnumeratorRangeBoundaries() {
  constexpr auto NarrowLowest =
      static_cast<std::int64_t>(std::numeric_limits<signed char>::min());
  constexpr auto NarrowHighest =
      static_cast<std::int64_t>(std::numeric_limits<signed char>::max());
  constexpr auto CanonicalLowest =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr auto CanonicalHighest =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Narrow> Enumeration = Registry.RegisterEnum<Narrow>(
        "Narrow", Luna::StableTypeKey("Studio.Narrow"));
    Luna::EnumBuilder<Narrow> &Declared =
        Enumeration.Value("Lowest", NarrowLowest)
            .Value("Highest", NarrowHighest);
    Check(Declared.Commit().IsSuccess(),
          "both limits of the declared C++ underlying type are accepted");
    Check(Owner.Execute("assert(Narrow.Lowest == -128)").IsSuccess() &&
              Owner.Execute("assert(Narrow.Highest == 127)").IsSuccess(),
          "a value exactly at a declared limit publishes unchanged");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Narrow> Enumeration = Registry.RegisterEnum<Narrow>(
        "Narrow", Luna::StableTypeKey("Studio.Narrow"));
    Luna::EnumBuilder<Narrow> &Declared =
        Enumeration.Value("Small", Narrow::Small)
            .Value("PastHighest", NarrowHighest + 1);
    const auto Result = Declared.Commit();
    Check(!Result.IsSuccess(),
          "the first value past the declared underlying maximum is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "[-128, 127]") != std::string::npos,
          "the refusal names the exact permitted declared range");
    Check(PathKind(Owner, "Narrow") == "absent",
          "a boundary refusal installs no enumeration table");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Narrow> Enumeration = Registry.RegisterEnum<Narrow>(
        "Narrow", Luna::StableTypeKey("Studio.Narrow"));
    Luna::EnumBuilder<Narrow> &Declared =
        Enumeration.Value("Small", Narrow::Small)
            .Value("PastLowest", NarrowLowest - 1);
    Check(!Declared.Commit().IsSuccess(),
          "the first value below the declared underlying minimum is refused");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Mask> Enumeration =
        Registry.RegisterEnum<Mask>("Mask", Luna::StableTypeKey("Studio.Mask"));
    Luna::EnumBuilder<Mask> &Declared =
        Enumeration.Value("None", Mask::None)
            .Value("Highest", static_cast<std::int64_t>(
                                  std::numeric_limits<unsigned char>::max()));
    Check(Declared.Commit().IsSuccess(),
          "both limits of an unsigned underlying type are accepted");
    Check(Owner.Execute("assert(Mask.Highest == 255)").IsSuccess(),
          "an unsigned limit publishes unchanged");

    Luna::EnumBuilder<Mask> Negative = Registry.RegisterEnum<Mask>(
        "Other", Luna::StableTypeKey("Studio.Other"));
    Luna::EnumBuilder<Mask> &Refused =
        Negative.Value("Below", static_cast<std::int64_t>(-1));
    const auto Result = Refused.Commit();
    Check(!Result.IsSuccess(),
          "a negative value is refused for an unsigned underlying type");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "[0, 255]") != std::string::npos,
          "the refusal names the unsigned permitted range");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Wide> Enumeration =
        Registry.RegisterEnum<Wide>("Wide", Luna::StableTypeKey("Studio.Wide"));
    Luna::EnumBuilder<Wide> &Declared =
        Enumeration.Value("Lowest", CanonicalLowest)
            .Value("Highest", CanonicalHighest);
    Check(Declared.Commit().IsSuccess(),
          "both limits of the exact-integer domain are accepted");
    Check(Owner.Execute("assert(Wide.Lowest == -2147483648)").IsSuccess() &&
              Owner.Execute("assert(Wide.Highest == 2147483647)").IsSuccess(),
          "a value exactly at a domain limit publishes without narrowing");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Wide> Enumeration =
        Registry.RegisterEnum<Wide>("Wide", Luna::StableTypeKey("Studio.Wide"));
    Luna::EnumBuilder<Wide> &Declared =
        Enumeration.Value("Zero", Wide::Zero)
            .Value("PastHighest", CanonicalHighest + 1);
    const auto Result = Declared.Commit();
    Check(!Result.IsSuccess(),
          "the first value past the exact-integer domain is refused");
    Check(Result.Diagnostic() &&
              Result.Diagnostic()->Message().find(
                  "[-2147483648, 2147483647]") != std::string::npos,
          "the refusal names the exact permitted conversion domain");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "narrows") != std::string::npos,
          "a domain refusal states that Luna never narrows a declared value");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Wide> Enumeration =
        Registry.RegisterEnum<Wide>("Wide", Luna::StableTypeKey("Studio.Wide"));
    Luna::EnumBuilder<Wide> &Declared =
        Enumeration.Value("Zero", Wide::Zero)
            .Value("PastLowest", CanonicalLowest - 1);
    Check(!Declared.Commit().IsSuccess(),
          "the first value below the exact-integer domain is refused");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::EnumBuilder<Wide> Enumeration =
        Registry.RegisterEnum<Wide>("Wide", Luna::StableTypeKey("Studio.Wide"));
    Luna::EnumBuilder<Wide> &Declared =
        Enumeration.Value("Low", static_cast<std::int64_t>(1))
            .Value("High", static_cast<std::int64_t>(2))
            .Bitflags(CanonicalHighest);
    Check(Declared.Commit().IsSuccess(),
          "a supported mask exactly at the domain maximum is accepted");

    Luna::EnumBuilder<Wide> Beyond = Registry.RegisterEnum<Wide>(
        "Other", Luna::StableTypeKey("Studio.OtherWide"));
    Luna::EnumBuilder<Wide> &Refused =
        Beyond.Value("Low", static_cast<std::int64_t>(1))
            .Bitflags(CanonicalHighest + 2);
    Check(!Refused.Commit().IsSuccess(),
          "a supported mask past the domain maximum is refused");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterConstant("Highest", CanonicalHighest).IsSuccess() &&
              Registry.RegisterConstant("Lowest", CanonicalLowest).IsSuccess(),
          "a constant exactly at either domain limit commits");
    Check(Owner.Execute("assert(Highest == 2147483647)").IsSuccess() &&
              Owner.Execute("assert(Lowest == -2147483648)").IsSuccess(),
          "a constant at a domain limit publishes unchanged");
    Check(!Registry.RegisterConstant("Beyond", CanonicalHighest + 1)
                  .IsSuccess() &&
              !Registry.RegisterConstant("Below", CanonicalLowest - 1)
                   .IsSuccess(),
          "the first constant past either domain limit is refused");
  }
}

void CheckEnumerationScopesAreNotNamespaces() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterAlignment(Registry).IsSuccess(), "the enumeration publishes");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  const auto Adoption = Studio.RegisterNamespace("Alignment").Commit();
  Check(!Adoption.IsSuccess(),
        "a namespace never reopens an enumeration table as its own scope");
  Check(Adoption.Diagnostic() && Adoption.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
        "a namespace over an enumeration is a deterministic collision");

  Luna::NamespaceBuilder Nested = Registry.RegisterNamespace("Studio");
  const auto Inside = Nested.RegisterConstant("Alignment", 1).Commit();
  Check(!Inside.IsSuccess(),
        "a constant never replaces a published enumeration table");
  Check(Owner.Execute("assert(Studio.Alignment.Center == 1)").IsSuccess(),
        "every refused request leaves the enumeration table intact");
}

void CheckEnumerationConstantsKeepTheirTypeIdentity() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterAlignment(Registry).IsSuccess(), "the enumeration publishes");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Check(Studio
            .RegisterConstant("DefaultAlignment", Alignment::Center,
                              AlignmentKey())
            .Commit()
            .IsSuccess(),
        "a constant of a registered enumeration commits");
  Check(Owner.Execute("assert(Studio.DefaultAlignment == 1)").IsSuccess(),
        "an enumeration constant publishes its numeric representation");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Constant =
      Snapshot.Find("Studio.DefaultAlignment");
  const Luna::ReflectionRecord Enumeration = Snapshot.Find("Studio.Alignment");
  Check(Constant.IsValid() && Constant.Type() == Enumeration.Type(),
        "an enumeration constant keeps the enumeration's canonical type "
        "identity instead of an untyped integer");
  Check(Constant.Descriptor().Kind() == Luna::TypeKind::Enumeration,
        "an enumeration constant reflects the enumeration descriptor");

  const auto Undeclared = Registry.RegisterConstant(
      "Stray", static_cast<Alignment>(9), AlignmentKey());
  Check(!Undeclared.IsSuccess(),
        "a constant naming an undeclared enumerator is refused");
  Check(PathKind(Owner, "Stray") == "absent",
        "a refused enumeration constant installs nothing");
}

void CheckFailedPlansLeaveNothingBehind() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const std::uint64_t Generation = Hooks::GenerationsOf(Owner)->Generation();

  Check(Owner.Execute("Alignment = { Marker = 3 }").IsSuccess(),
        "the script creates its own table");
  Luna::EnumBuilder<Alignment> Enumeration =
      Registry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
  const auto Collision = Enumeration.Value("Left", Alignment::Left).Commit();
  Check(!Collision.IsSuccess(),
        "an enumeration never adopts a script-created table");
  Check(Owner.Execute("assert(Alignment.Marker == 3)").IsSuccess(),
        "the script's table keeps its contents after the collision");
  Check(Hooks::GenerationsOf(Owner)->Generation() == Generation,
        "a refused enumeration publishes no generation");
  Check(Registry.Reflection().IsEmpty(),
        "a refused enumeration publishes no reflection record");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::EnumBuilder<Alignment> Nested =
      Studio.RegisterEnum<Alignment>("Alignment", AlignmentKey());
  static_cast<void>(Nested.Value("Left", Alignment::Left));
  static_cast<void>(Studio.RegisterConstant("Pi", 3.5));
  static_cast<void>(Studio.RegisterConstant("Broken", Alignment::Left));
  const auto Result = Studio.Commit();
  Check(!Result.IsSuccess(), "one refused declaration fails the whole plan");
  Check(PathKind(Owner, "Studio") == "absent" &&
            PathKind(Owner, "Studio.Alignment") == "absent" &&
            PathKind(Owner, "Studio.Pi") == "absent",
        "a failed plan installs none of its declarations");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "a failed plan records no namespace ownership");

  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  Check(Depth && *Depth == 0,
        "a failed plan restores the exact entry stack depth");

  Luna::NamespaceBuilder Retry = Registry.RegisterNamespace("Studio");
  Luna::EnumBuilder<Alignment> Valid =
      Retry.RegisterEnum<Alignment>("Alignment", AlignmentKey());
  static_cast<void>(Valid.Value("Left", Alignment::Left));
  static_cast<void>(Retry.RegisterConstant("Pi", 3.5));
  Check(Retry.Commit().IsSuccess(),
        "the State stays reusable after a failed plan");
  Check(PathKind(Owner, "Studio.Alignment") == "table" &&
            Owner.Execute("assert(Studio.Pi == 3.5)").IsSuccess(),
        "the retried plan publishes every declaration");
}

} // namespace

int RunConstantAndEnumerationTests() {
  FailureCount = 0;
  CheckConstantsPublishAndReflect();
  CheckUnsupportedAndOutOfRangeConstantsAreRefused();
  CheckScopedEnumerationPublishesTypedTable();
  CheckEnumerationTableRefusesScriptWrites();
  CheckDuplicateNamesValuesAndAliases();
  CheckUnscopedEnumerationsRequireOptIn();
  CheckBitflagsRequireDeclarationAndRejectUnsupportedBits();
  CheckOutOfRangeEnumeratorsAreRefused();
  CheckEnumeratorRangeBoundaries();
  CheckEnumerationScopesAreNotNamespaces();
  CheckEnumerationConstantsKeepTheirTypeIdentity();
  CheckFailedPlansLeaveNothingBehind();
  return FailureCount == 0 ? 0 : 1;
}
