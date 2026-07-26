// Deterministic golden, structural, and lifecycle coverage for both
// generators over one complete reflected surface: modules with dependencies and
// exports, nested namespaces, constants of several canonical types, overloaded
// callables with optional, defaulted, and variadic parameters and zero, scalar,
// and multiple returns, a class hierarchy with construction, factories,
// methods, static methods, properties, fields, and an operator, an enumeration
// with an alias, and declared attributes, examples, and prose.
//
// The pinned artifacts under `generation/golden` are compared byte for byte, so
// every drift in either generator is one visible diff. Structural parsing then
// checks the shape those bytes must have - canonical section order, provenance
// grouping, one Luau class declaration per reflected class, and a base declared
// before the class that extends it - against the captured snapshot itself
// rather than against a second copy of the expectation.
//
// Every artifact here is generated from one retained snapshot, including after
// later registration, freeze, a State move, destruction of the originating
// State, and from another thread, so generation demonstrably reads only the
// captured generation and never the virtual machine.

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/generation/declaration.hpp>
#include <luna/generation/documentation.hpp>
#include <luna/generation/publication.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

int FailureCount = 0;

// Set to true only while regenerating the pinned artifacts below.
constexpr bool DumpGoldenArtifacts = false;

constexpr std::string_view DocumentationGolden = "complete_surface.md";
constexpr std::string_view DeclarationGolden = "complete_surface.d.lua";

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "generator golden check failed: " << Description << '\n';
}

[[nodiscard]] bool Contains(const std::string &Text, std::string_view Needle) {
  return Text.find(Needle) != std::string::npos;
}

// One reflected surface, stated in three different registration orders. Only
// the order changes: the declarations, their metadata, and their prose are
// identical, so every artifact generated from any of them must be identical
// too.
enum class Palette : int { Red = 1, Green = 2, Blue = 4 };

struct Shape {
  int Sides = 3;

  [[nodiscard]] int Corners() const { return Sides; }
};

struct Gadget final : Shape {
  int Charge = 3;

  [[nodiscard]] int Level() const { return Charge * 2; }
  [[nodiscard]] int Boost(int Amount) const { return Charge + Amount; }
  [[nodiscard]] int Length() const { return Charge; }
};

[[nodiscard]] std::string DescribeGadget() { return "gadget"; }

[[nodiscard]] Gadget MakeGadget(int Charge) {
  Gadget Made;
  Made.Charge = Charge;
  return Made;
}

[[nodiscard]] int Trim(int Value, std::optional<int> Limit) {
  return Limit ? (Value < *Limit ? Value : *Limit) : Value;
}

[[nodiscard]] int Offset(int Value, int Amount) { return Value + Amount; }

[[nodiscard]] int Total(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] std::pair<int, std::string> Split(std::string Text) {
  return {static_cast<int>(Text.size()), std::move(Text)};
}

[[nodiscard]] Luna::StableTypeKey ShapeKey() {
  return Luna::StableTypeKey("Studio.GoldenShape");
}

[[nodiscard]] Luna::StableTypeKey GadgetKey() {
  return Luna::StableTypeKey("Studio.GoldenGadget");
}

[[nodiscard]] Luna::StableTypeKey PaletteKey() {
  return Luna::StableTypeKey("Studio.GoldenPalette");
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency Dependency(std::string Identity,
                                                std::string_view Constraint) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  if (const auto Parsed = Luna::VersionConstraint::TryParse(Constraint))
    Declared.Constraints.push_back(*Parsed);
  return Declared;
}

[[nodiscard]] Luna::ModuleManifest
Manifest(std::string Identity, std::string_view VersionText,
         std::string Documentation,
         std::vector<Luna::ModuleDependency> Dependencies = {}) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), std::move(Dependencies),
      std::move(Documentation), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder, bool Reversed) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  if (Reversed) {
    static_cast<void>(Units.RegisterConstant("Name", "metric"));
    static_cast<void>(Units.RegisterConstant("Metre", 1));
  } else {
    static_cast<void>(Units.RegisterConstant("Metre", 1));
    static_cast<void>(Units.RegisterConstant("Name", "metric"));
  }
  static_cast<void>(Units.Documentation("Base units.")
                        .Documentation("Metre", "One metre.")
                        .Attribute("Metre", "Unit", "m"));
}

// The declarations of one namespace, stated in the given order. Overload
// candidates of one name are stated in both orders too, so the canonical
// candidate order can never inherit registration order.
void DeclarePhysics(Luna::NamespaceBuilder &Physics, bool Reversed) {
  if (Reversed) {
    static_cast<void>(Physics.RegisterFunction("Split", &Split));
    static_cast<void>(Physics.RegisterFunction("Total", &Total));
    static_cast<void>(
        Physics.RegisterFunction("Offset", Luna::WithDefaults(&Offset, 5)));
    static_cast<void>(Physics.RegisterFunction("Trim", &Trim));
    static_cast<void>(Physics.RegisterFunction("Reset", [] {}));
    static_cast<void>(Physics.RegisterFunction(
        "Blend", [](std::string Text) { return Text; }));
    static_cast<void>(Physics.RegisterFunction(
        "Blend", [](int Left, int Right) { return Left + Right; }));
    static_cast<void>(
        Physics.RegisterFunction("Scale", [](int Value) { return Value * 2; }));
    static_cast<void>(Physics.RegisterConstant("Enabled", true));
    static_cast<void>(Physics.RegisterConstant("Epsilon", 0.5));
    static_cast<void>(Physics.RegisterConstant("Gravity", 10));
    return;
  }
  static_cast<void>(Physics.RegisterConstant("Gravity", 10));
  static_cast<void>(Physics.RegisterConstant("Epsilon", 0.5));
  static_cast<void>(Physics.RegisterConstant("Enabled", true));
  static_cast<void>(
      Physics.RegisterFunction("Scale", [](int Value) { return Value * 2; }));
  static_cast<void>(Physics.RegisterFunction(
      "Blend", [](int Left, int Right) { return Left + Right; }));
  static_cast<void>(
      Physics.RegisterFunction("Blend", [](std::string Text) { return Text; }));
  static_cast<void>(Physics.RegisterFunction("Reset", [] {}));
  static_cast<void>(Physics.RegisterFunction("Trim", &Trim));
  static_cast<void>(
      Physics.RegisterFunction("Offset", Luna::WithDefaults(&Offset, 5)));
  static_cast<void>(Physics.RegisterFunction("Total", &Total));
  static_cast<void>(Physics.RegisterFunction("Split", &Split));
}

void ConfigurePhysics(Luna::NamespaceBuilder &Builder, bool Reversed) {
  Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
  if (Reversed) {
    Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
    static_cast<void>(
        Solver.RegisterFunction("Solve", [](int Value) { return Value + 1; }));
    static_cast<void>(Solver.RegisterConstant("Iterations", 4));
    static_cast<void>(Solver.Documentation("The iterative solver."));
  }
  DeclarePhysics(Physics, Reversed);
  static_cast<void>(Physics.Documentation("Physics helpers.")
                        .Attribute("Stability", "stable")
                        .Documentation("Gravity", "Metres per second squared.")
                        .Attribute("Gravity", "Unit", "m/s^2")
                        .Example("Gravity", "print(Physics.Gravity)")
                        .Documentation("Scale", "Doubles one value.")
                        .Example("Scale", "local Scaled = Physics.Scale(21)"));

  if (Reversed)
    return;
  Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
  static_cast<void>(Solver.RegisterConstant("Iterations", 4));
  static_cast<void>(
      Solver.RegisterFunction("Solve", [](int Value) { return Value + 1; }));
  static_cast<void>(Solver.Documentation("The iterative solver."));
}

// The module group: one available dependency definition and one loaded graph
// that names it, so every declaration either carries module provenance or
// deliberately carries none.
[[nodiscard]] bool RegisterModules(Luna::BindingRegistry &Registry,
                                   bool Reversed) {
  const bool Provided =
      Registry
          .ProvideModule(Manifest("studio.units", "1.0.0", "The unit module."),
                         [Reversed](Luna::NamespaceBuilder &Builder) {
                           ConfigureUnits(Builder, Reversed);
                         })
          .IsSuccess();
  const bool Loaded =
      Registry
          .RegisterModule(Manifest("studio.physics", "2.1.0",
                                   "The physics module.",
                                   {Dependency("studio.units", ">=1.0.0")}),
                          [Reversed](Luna::NamespaceBuilder &Builder) {
                            ConfigurePhysics(Builder, Reversed);
                          })
          .IsSuccess();
  return Provided && Loaded;
}

// The class group: one base class, one class that extends it, and one scoped
// enumeration with an alias, all inside one namespace plan.
[[nodiscard]] bool RegisterStudio(Luna::BindingRegistry &Registry,
                                  bool Reversed) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  const auto RegisterEnumeration = [&Studio] {
    Luna::EnumBuilder<Palette> Colours =
        Studio.RegisterEnum<Palette>("Palette", PaletteKey());
    static_cast<void>(Colours.Value("Red", Palette::Red)
                          .Value("Green", Palette::Green)
                          .Value("Blue", Palette::Blue)
                          .Alias("Primary", "Red")
                          .Documentation("The palette.")
                          .Documentation("Red", "The red enumerator.")
                          .Attribute("Red", "Channel", "0"));
  };

  const auto RegisterClasses = [&Studio, Reversed] {
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(Base.Constructor<>()
                          .Property("Corners", &Shape::Corners)
                          .Documentation("One declared shape."));

    Luna::ClassBuilder<Gadget> Class =
        Studio.RegisterClass<Gadget>("Gadget", GadgetKey());
    if (Reversed) {
      static_cast<void>(
          Class.Operator(Luna::ClassOperator::Length, &Gadget::Length)
              .StaticMethod("Describe", &DescribeGadget)
              .Method("Boost", &Gadget::Boost)
              .Field("Charge", &Gadget::Charge)
              .Property("Level", Luna::PropertyPolicy::Lazy(), &Gadget::Level)
              .Factory("FromCharge", &MakeGadget)
              .Base<Shape>(ShapeKey())
              .Constructor<>());
    } else {
      static_cast<void>(
          Class.Constructor<>()
              .Base<Shape>(ShapeKey())
              .Factory("FromCharge", &MakeGadget)
              .Property("Level", Luna::PropertyPolicy::Lazy(), &Gadget::Level)
              .Field("Charge", &Gadget::Charge)
              .Method("Boost", &Gadget::Boost)
              .StaticMethod("Describe", &DescribeGadget)
              .Operator(Luna::ClassOperator::Length, &Gadget::Length));
    }
    static_cast<void>(Class.Documentation("One documented gadget.")
                          .Example("local Value = Gadget.new()")
                          .Attribute("Stability", "stable")
                          .Documentation("Level", "Twice the stored charge.")
                          .Attribute("Level", "Access", "read-only")
                          .Documentation("Boost", "Adds to the charge."));
  };

  if (Reversed) {
    RegisterEnumeration();
    RegisterClasses();
  } else {
    RegisterClasses();
    RegisterEnumeration();
  }
  return Studio.Commit().IsSuccess();
}

// The root group: declarations no module and no namespace owns.
[[nodiscard]] bool RegisterRoot(Luna::BindingRegistry &Registry,
                                bool Reversed) {
  const auto Function = [&Registry] {
    return Registry
        .RegisterFunction("Describe", [](std::string Text) { return Text; })
        .IsSuccess();
  };
  const auto Constant = [&Registry] {
    return Registry.RegisterConstant("Version", "1.0").IsSuccess();
  };
  if (Reversed)
    return Constant() && Function();
  return Function() && Constant();
}

// One complete reflected surface, stated in one of three registration orders.
void RegisterCompleteSurface(Luna::BindingRegistry &Registry, unsigned Order) {
  const bool Reversed = Order != 0;
  switch (Order % 3) {
  case 1:
    Check(RegisterRoot(Registry, Reversed), "the root declarations commit");
    Check(RegisterStudio(Registry, Reversed), "the class plan commits");
    Check(RegisterModules(Registry, Reversed), "the module graph loads");
    return;
  case 2:
    Check(RegisterStudio(Registry, Reversed), "the class plan commits");
    Check(RegisterModules(Registry, Reversed), "the module graph loads");
    Check(RegisterRoot(Registry, Reversed), "the root declarations commit");
    return;
  default:
    Check(RegisterModules(Registry, Reversed), "the module graph loads");
    Check(RegisterStudio(Registry, Reversed), "the class plan commits");
    Check(RegisterRoot(Registry, Reversed), "the root declarations commit");
    return;
  }
}

[[nodiscard]] Luna::ReflectionSnapshot CompleteSurface(Luna::State &Owner,
                                                       unsigned Order) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  RegisterCompleteSurface(Registry, Order);
  return Registry.Reflection();
}

[[nodiscard]] std::filesystem::path GoldenPath(std::string_view Name) {
  return std::filesystem::path(LUNA_GENERATION_GOLDEN_DIRECTORY) /
         std::filesystem::path(std::string(Name));
}

[[nodiscard]] std::optional<std::string>
ReadBytes(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  if (!Stream.is_open())
    return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(Stream)),
                     std::istreambuf_iterator<char>());
}

// One line and column of one byte offset, so a golden mismatch names exactly
// where the artifact drifted.
[[nodiscard]] std::string Position(const std::string &Text,
                                   std::size_t Offset) {
  std::size_t Line = 1;
  std::size_t Column = 1;
  for (std::size_t Index = 0; Index < Offset && Index < Text.size(); ++Index) {
    if (Text[Index] == '\n') {
      ++Line;
      Column = 1;
      continue;
    }
    ++Column;
  }
  std::string Where("line ");
  Where.append(std::to_string(Line));
  Where.append(", column ");
  Where.append(std::to_string(Column));
  return Where;
}

[[nodiscard]] std::string Excerpt(const std::string &Text, std::size_t Offset) {
  if (Offset >= Text.size())
    return "<end of artifact>";
  return Text.substr(Offset, 48);
}

// Compares one generated artifact with its pinned golden file byte for byte.
[[nodiscard]] bool MatchesGolden(std::string_view Name,
                                 const std::string &Bytes) {
  const std::optional<std::string> Golden = ReadBytes(GoldenPath(Name));
  if (!Golden) {
    ++FailureCount;
    std::cerr << "generator golden check failed: the pinned artifact "
              << GoldenPath(Name).string() << " is missing\n";
    return false;
  }
  if (*Golden == Bytes)
    return true;
  ++FailureCount;
  std::size_t Offset = 0;
  while (Offset < Golden->size() && Offset < Bytes.size() &&
         (*Golden)[Offset] == Bytes[Offset])
    ++Offset;
  std::cerr << "generator golden check failed: " << Name
            << " differs from its pinned artifact at "
            << Position(*Golden, Offset) << " (pinned " << Golden->size()
            << " bytes, generated " << Bytes.size() << " bytes)\n"
            << "  pinned:    " << Excerpt(*Golden, Offset) << '\n'
            << "  generated: " << Excerpt(Bytes, Offset) << '\n';
  return false;
}

// Regenerates the pinned artifacts through the ordinary publication service, so
// a golden file is only ever the exact bytes generation produced.
[[nodiscard]] bool DumpEveryGoldenArtifact() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  const Luna::ArtifactPublication Documentation =
      Luna::PublishDocumentation(Snapshot, Luna::DocumentationOptions(),
                                 GoldenPath(DocumentationGolden).string());
  const Luna::ArtifactPublication Declarations =
      Luna::PublishDeclarations(Snapshot, Luna::DeclarationOptions(),
                                GoldenPath(DeclarationGolden).string());
  if (!Documentation.IsPublished())
    std::cerr << "documentation dump refused: "
              << Documentation.Diagnostic()->Message() << '\n';
  if (!Declarations.IsPublished())
    std::cerr << "declaration dump refused: "
              << Declarations.Diagnostic()->Message() << '\n';
  return Documentation.IsPublished() && Declarations.IsPublished();
}

[[nodiscard]] std::vector<std::string> Lines(const std::string &Text) {
  std::vector<std::string> Found;
  std::size_t Start = 0;
  while (Start < Text.size()) {
    const std::size_t Break = Text.find('\n', Start);
    if (Break == std::string::npos) {
      Found.push_back(Text.substr(Start));
      break;
    }
    Found.push_back(Text.substr(Start, Break - Start));
    Start = Break + 1;
  }
  return Found;
}

// Every line that opens with `Prefix`, in order, with the prefix removed.
[[nodiscard]] std::vector<std::string> Entries(const std::string &Text,
                                               std::string_view Prefix) {
  std::vector<std::string> Found;
  for (const std::string &Line : Lines(Text)) {
    if (Line.size() > Prefix.size() && Line.starts_with(Prefix))
      Found.push_back(Line.substr(Prefix.size()));
  }
  return Found;
}

[[nodiscard]] std::size_t IndexOf(const std::vector<std::string> &Values,
                                  std::string_view Value) {
  for (std::size_t Index = 0; Index < Values.size(); ++Index) {
    if (Values[Index] == Value)
      return Index;
  }
  return Values.size();
}

// The Luau type name one canonical class name flattens to.
[[nodiscard]] std::string Flattened(std::string_view CanonicalName) {
  std::string Text(CanonicalName);
  for (char &Character : Text) {
    if (Character == '.')
      Character = '_';
  }
  return Text;
}

[[nodiscard]] std::string ModuleKey(const Luna::ModuleRecord &Module) {
  std::string Key(Module.Identity());
  Key.push_back('@');
  Key.append(Module.Version());
  return Key;
}

// The canonical bytes rule every accepted artifact obeys.
void CheckCanonicalBytes(const std::string &Bytes) {
  Check(!Bytes.empty(), "an accepted artifact carries bytes");
  Check(Bytes.find('\r') == std::string::npos,
        "the artifact uses LF line endings only");
  Check(!Bytes.starts_with("\xef\xbb\xbf") &&
            Bytes.find("\xef\xbb\xbf") == std::string::npos,
        "the artifact carries no byte-order mark anywhere");
  Check(!Bytes.empty() && Bytes.back() == '\n',
        "the last line of the artifact is LF terminated");
}

// One private directory per case, removed again when the case ends, so no
// temporary file survives the suite.
class ScratchDirectory final {
public:
  explicit ScratchDirectory(std::string_view Name) {
    static unsigned Counter = 0;
    std::string Leaf("luna-generation-golden-");
    Leaf.append(Name);
    Leaf.push_back('-');
    Leaf.append(std::to_string(++Counter));
    PathValue = std::filesystem::temp_directory_path() / Leaf;
    std::error_code Error;
    std::filesystem::remove_all(PathValue, Error);
    Check(std::filesystem::create_directories(PathValue, Error) && !Error,
          "the scratch directory is created");
  }

  ScratchDirectory(const ScratchDirectory &) = delete;
  ScratchDirectory &operator=(const ScratchDirectory &) = delete;

  ~ScratchDirectory() {
    std::error_code Error;
    std::filesystem::remove_all(PathValue, Error);
  }

  [[nodiscard]] std::filesystem::path File(std::string_view Name) const {
    return PathValue / std::filesystem::path(std::string(Name));
  }

  [[nodiscard]] std::vector<std::string> Names() const {
    std::vector<std::string> Found;
    std::error_code Error;
    for (const auto &Entry :
         std::filesystem::directory_iterator(PathValue, Error))
      Found.push_back(Entry.path().filename().string());
    return Found;
  }

private:
  std::filesystem::path PathValue;
};

// Requirements 16.2, 16.3, 19.10: the complete reflected surface generates
// exactly its pinned documentation and declarations, repeatedly, and both
// artifacts are canonical UTF-8 without a byte-order mark and with LF endings.
void CheckGoldenArtifactsPinTheCompleteSurface() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  const Luna::GeneratedArtifact Documentation =
      Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions());
  const Luna::GeneratedArtifact Declarations =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());

  Check(Documentation.IsComplete() &&
            Documentation.Status() == Luna::GenerationStatus::Valid,
        "the complete surface documents completely");
  Check(Declarations.IsComplete() &&
            Declarations.Status() == Luna::GenerationStatus::Valid,
        "the complete surface declares completely");
  if (!Documentation.IsComplete() && Documentation.Diagnostic() != nullptr)
    std::cerr << "  documentation rejection: "
              << Documentation.Diagnostic()->Message() << '\n';
  if (!Declarations.IsComplete() && Declarations.Diagnostic() != nullptr)
    std::cerr << "  declaration rejection: "
              << Declarations.Diagnostic()->Message() << '\n';
  if (!Documentation.IsComplete() || !Declarations.IsComplete())
    return;

  CheckCanonicalBytes(Documentation.Bytes());
  CheckCanonicalBytes(Declarations.Bytes());
  static_cast<void>(MatchesGolden(DocumentationGolden, Documentation.Bytes()));
  static_cast<void>(MatchesGolden(DeclarationGolden, Declarations.Bytes()));

  Check(Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions())
                    .Bytes() == Documentation.Bytes() &&
            Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions())
                    .Bytes() == Declarations.Bytes(),
        "repeated generation from one snapshot and equal options is "
        "byte-identical");
}

// Requirement 16.4: canonical ordering follows names and declaration metadata,
// so every registration order of one surface reproduces the pinned artifacts.
void CheckEveryRegistrationOrderReproducesTheGolden() {
  for (unsigned Order = 0; Order < 3; ++Order) {
    Luna::State Owner;
    const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, Order);
    const Luna::GeneratedArtifact Documentation =
        Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions());
    const Luna::GeneratedArtifact Declarations =
        Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());
    Check(Documentation.IsComplete() && Declarations.IsComplete(),
          "every registration order of the surface generates completely");
    if (!Documentation.IsComplete() || !Declarations.IsComplete())
      continue;
    static_cast<void>(
        MatchesGolden(DocumentationGolden, Documentation.Bytes()));
    static_cast<void>(MatchesGolden(DeclarationGolden, Declarations.Bytes()));
  }
}

// Requirements 16.4, 16.9, 19.10: the documentation artifact has exactly the
// structure the captured snapshot describes - canonical sections, provenance
// groups in module order, and one entry per reflected symbol in the canonical
// order that snapshot enumerates.
void CheckDocumentationStructureFollowsTheSnapshot() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  const std::string Text =
      Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions())
          .Bytes();
  if (Text.empty())
    return;

  Check(Text.starts_with("# Luna API Reference\n"),
        "the artifact opens with its canonical title");
  const std::size_t Modules = Text.find("## Modules\n");
  const std::size_t Types = Text.find("## Types\n");
  const std::size_t Symbols = Text.find("## Symbols\n");
  Check(Modules != std::string::npos && Types != std::string::npos &&
            Symbols != std::string::npos && Modules < Types && Types < Symbols,
        "the canonical sections appear once each, in canonical order");

  // The provenance groups are the declarations no module contributed, then one
  // group per module in the canonical module order of this generation.
  std::vector<std::string> ExpectedGroups{"(no module)"};
  for (std::size_t Index = 0; Index < Snapshot.Modules().Size(); ++Index)
    ExpectedGroups.push_back(ModuleKey(Snapshot.Modules().At(Index)));
  const std::vector<std::string> Groups = Entries(Text.substr(Symbols), "### ");
  Check(Groups == ExpectedGroups,
        "the symbol groups name every module provenance in canonical order");

  // Every symbol of the generation is documented exactly once, grouped by
  // provenance and otherwise in the canonical order the snapshot enumerates.
  std::vector<std::string> ExpectedSymbols;
  for (const std::string &Group : ExpectedGroups) {
    const bool WithoutModule = Group == "(no module)";
    for (std::size_t Index = 0; Index < Snapshot.Symbols().Size(); ++Index) {
      const Luna::ReflectionRecord Record = Snapshot.Symbols().At(Index);
      const bool Belongs = Record.HasModule()
                               ? ModuleKey(Record.Module()) == Group
                               : WithoutModule;
      if (Belongs)
        ExpectedSymbols.emplace_back(Record.QualifiedName());
    }
  }
  const std::vector<std::string> Documented = Entries(Text, "#### ");
  Check(Documented == ExpectedSymbols,
        "every captured symbol is documented once, in canonical order");
  Check(Documented.size() == Snapshot.Symbols().Size() && !Documented.empty(),
        "the documented symbol count is exactly the captured symbol count");

  // Provenance, prose, attributes, and examples of the captured surface all
  // reach the artifact.
  Check(Contains(Text, "### studio.physics@2.1.0\n") &&
            Contains(Text, "- studio.units@1.0.0 [>=1.0.0]\n") &&
            Contains(Text, "Module: studio.units@1.0.0\n"),
        "module identity, version, dependency, and provenance are documented");
  Check(Contains(Text, "Kind: overload_set\n") &&
            Contains(Text, "Kind: function_candidate\n") &&
            Contains(Text, "Kind: constructor\n") &&
            Contains(Text, "Kind: factory\n") &&
            Contains(Text, "Kind: method\n") &&
            Contains(Text, "Kind: static_method\n") &&
            Contains(Text, "Kind: property\n") &&
            Contains(Text, "Kind: field\n") &&
            Contains(Text, "Kind: operator\n") &&
            Contains(Text, "Kind: enumerator_alias\n") &&
            Contains(Text, "Kind: constant\n") &&
            Contains(Text, "Kind: namespace\n") &&
            Contains(Text, "Kind: class\n") && Contains(Text, "Kind: enum\n"),
        "every reflected symbol kind of the surface is documented");
  Check(Contains(Text, "- Unit: m/s^2\n") &&
            Contains(Text, "local Scaled = Physics.Scale(21)") &&
            Contains(Text, "Twice the stored charge."),
        "declared attributes, examples, and prose reach the artifact");
}

// Requirements 16.1, 16.4, 19.10: the declaration artifact has exactly the
// structure Luau requires of the captured surface - one class type per
// reflected class, each base declared before the class that extends it, and
// balanced declaration blocks.
void CheckDeclarationStructureFollowsTheSnapshot() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  const std::string Text =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions()).Bytes();
  if (Text.empty())
    return;

  Check(Text.starts_with("--!strict\n"),
        "the artifact opens with its canonical mode line");
  std::size_t Depth = 0;
  std::size_t Deepest = 0;
  bool Balanced = true;
  for (const char Character : Text) {
    if (Character == '{')
      ++Depth;
    else if (Character == '}') {
      if (Depth == 0)
        Balanced = false;
      else
        --Depth;
    }
    Deepest = Depth > Deepest ? Depth : Deepest;
  }
  Check(Balanced && Depth == 0 && Deepest != 0,
        "every declared table block opens and closes exactly once");

  // Each reflected class declares exactly one Luau class type, and a base is
  // declared before the class that extends it.
  std::vector<std::string> Declared;
  for (const std::string &Entry : Entries(Text, "declare class "))
    Declared.push_back(Entry.substr(0, Entry.find(' ')));
  const Luna::ReflectionRecordRange Classes =
      Snapshot.Symbols(Luna::SymbolKind::Class);
  Check(Declared.size() == Classes.Size() && !Declared.empty(),
        "every reflected class declares exactly one Luau class type");
  for (std::size_t Index = 0; Index < Classes.Size(); ++Index) {
    const Luna::ReflectionRecord Record = Classes.At(Index);
    const std::string Name = Flattened(Record.QualifiedName());
    const std::size_t Position = IndexOf(Declared, Name);
    Check(Position != Declared.size(),
          "each reflected class is declared under its flattened Luau name");
    for (std::size_t Relation = 0; Relation < Record.RelationCount();
         ++Relation) {
      const Luna::TypeRelation Edge = Record.Relation(Relation);
      if (Edge.Kind() != Luna::TypeRelationKind::Base)
        continue;
      const std::string Base = Flattened(Snapshot.FindType(Edge.Type()).Name());
      Check(IndexOf(Declared, Base) < Position,
            "a base class is declared before the class that extends it");
    }
  }

  Check(Contains(Text, "-- Module: studio.physics@2.1.0\n") &&
            Contains(Text, "-- Module: studio.units@1.0.0\n") &&
            Contains(Text, "-- studio.units@1.0.0\n"),
        "the artifact names every module of the generation and the provenance "
        "of each declaration");
  Check(Contains(Text, "declare class Studio_Gadget extends Studio_Shape") &&
            Contains(Text, "  Charge: number\n") &&
            Contains(Text, "  read Level: number\n") &&
            Contains(Text, "  function __len(self: Studio_Gadget): number\n"),
        "the class hierarchy, its members, and its operator are declared");
  Check(Contains(Text, "declare Physics: {") &&
            Contains(Text, "declare Studio: {") &&
            Contains(Text, "declare function Describe(") &&
            Contains(Text, "declare Version: string\n"),
        "namespaces, root callables, and root constants are declared");
  Check(Contains(Text, "  Blend: ((number, number) -> number) & ((string) -> "
                       "string),") &&
            Contains(Text, "  Reset: () -> (),") &&
            Contains(Text, "  Split: (string) -> (number, string),") &&
            Contains(Text, "  Trim: (number, number?) -> number,") &&
            Contains(Text, "  Offset: (number, number?) -> number,") &&
            Contains(Text, "  Total: (...any) -> number,"),
        "overloads, empty returns, multiple returns, omittable parameters, and "
        "variadic parameters are declared");
}

// Requirements 3.6, 16.2: generation reads only the captured generation, so one
// retained snapshot still generates exactly the pinned artifacts after later
// registration, a freeze, a State move, destruction of the originating State,
// and from a thread that never owned that State.
void CheckRetainedSnapshotGeneratesTheGoldenAcrossEveryLifecycleEvent() {
  const Luna::DocumentationOptions Documentation;
  const Luna::DeclarationOptions Declarations;
  Luna::ReflectionSnapshot Retained;

  // The whole matrix is one retained snapshot generating one pair of artifacts.
  const auto Matches = [&](std::string_view Stage) {
    const Luna::GeneratedArtifact Text =
        Luna::GenerateDocumentation(Retained, Documentation);
    const Luna::GeneratedArtifact Lua =
        Luna::GenerateDeclarations(Retained, Declarations);
    Check(Text.IsComplete() && Lua.IsComplete(), Stage);
    if (!Text.IsComplete() || !Lua.IsComplete())
      return;
    static_cast<void>(MatchesGolden(DocumentationGolden, Text.Bytes()));
    static_cast<void>(MatchesGolden(DeclarationGolden, Lua.Bytes()));
  };

  auto Owner = std::make_unique<Luna::State>();
  Retained = CompleteSurface(*Owner, 0);
  const std::uint64_t Captured = Retained.Generation();
  const std::size_t Size = Retained.Size();
  Matches("the captured snapshot generates completely");

  // Later registration publishes a new generation the retained snapshot never
  // observes.
  {
    Luna::BindingRegistry Registry = Owner->Bindings();
    Check(Registry.Register("Later", [] { return 1; }).IsSuccess(),
          "a later registration commits after the snapshot was captured");
    const std::string Live =
        Luna::GenerateDeclarations(Registry.Reflection(), Declarations).Bytes();
    Check(Contains(Live, "declare function Later(): number"),
          "a newly captured generation declares the later symbol");
    Check(!Contains(Luna::GenerateDeclarations(Retained, Declarations).Bytes(),
                    "Later"),
          "the retained generation never declares it");
  }
  Matches("the retained snapshot generates the same artifacts after later "
          "registration");

  // Freeze publishes caches and the frozen lifecycle transition, and changes no
  // captured generation.
  {
    Luna::BindingRegistry Registry = Owner->Bindings();
    Check(Registry.Freeze().IsSuccess(), "the populated State freezes");
  }
  Matches("the retained snapshot generates the same artifacts after freeze");

  // A moved State keeps the current generation; the retained snapshot is
  // unaffected either way.
  Luna::State Moved = std::move(*Owner);
  Check(!Owner->IsReady() && Moved.IsReady(),
        "the destination State is ready and the moved-from State is not");
  Matches("the retained snapshot generates the same artifacts after a State "
          "move");

  Owner.reset();
  Matches("the retained snapshot generates the same artifacts after the "
          "originating State is destroyed");

  {
    Luna::State Discarded = std::move(Moved);
    static_cast<void>(Discarded.IsReady());
  }
  Matches("the retained snapshot outlives every State that held its "
          "generation");

  Check(Retained.Generation() == Captured && Retained.Size() == Size,
        "the retained generation and its record count never changed");

  // Nothing generation reads belongs to a virtual machine or to one owning
  // thread, so another thread generates exactly the same artifacts.
  std::string CrossThreadText;
  std::string CrossThreadLua;
  std::thread Reader([&] {
    CrossThreadText =
        Luna::GenerateDocumentation(Retained, Documentation).Bytes();
    CrossThreadLua = Luna::GenerateDeclarations(Retained, Declarations).Bytes();
  });
  Reader.join();
  static_cast<void>(MatchesGolden(DocumentationGolden, CrossThreadText));
  static_cast<void>(MatchesGolden(DeclarationGolden, CrossThreadLua));
}

// Requirement 16.5: unsupported and unencodable metadata each reject generation
// deterministically, with no bytes and one repeatable diagnostic, and leave
// every other generation in the process untouched.
void CheckUnsupportedMetadataRejectsDeterministically() {
  const auto Reserved = [] {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterFunction("end", [] { return 1; }).IsSuccess(),
          "a Luau keyword is a valid Luna declaration name");
    return Luna::GenerateDeclarations(Registry.Reflection(),
                                      Luna::DeclarationOptions());
  };
  const Luna::GeneratedArtifact First = Reserved();
  const Luna::GeneratedArtifact Second = Reserved();
  Check(!First.IsComplete() &&
            First.Status() == Luna::GenerationStatus::UnsupportedDeclaration &&
            First.Bytes().empty() && First.Size() == 0,
        "a name Luau reserves rejects declaration generation with no bytes");
  Check(First.Diagnostic() != nullptr && Second.Diagnostic() != nullptr &&
            First.Status() == Second.Status() &&
            First.Diagnostic()->Message() == Second.Diagnostic()->Message() &&
            Contains(First.Diagnostic()->Message(), "unsupported-declaration"),
        "the rejection is one deterministic repeatable diagnostic");

  const auto Malformed = [](std::string_view Documentation) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(
        Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
    static_cast<void>(Studio.Documentation("Double", Documentation));
    Check(Studio.Commit().IsSuccess(),
          "documentation text is metadata, so the plan still commits");
    return Registry.Reflection();
  };
  const Luna::ReflectionSnapshot Invalid = Malformed("bad \xff byte");
  const Luna::GeneratedArtifact Text =
      Luna::GenerateDocumentation(Invalid, Luna::DocumentationOptions());
  const Luna::GeneratedArtifact Lua =
      Luna::GenerateDeclarations(Invalid, Luna::DeclarationOptions());
  Check(!Text.IsComplete() &&
            Text.Status() == Luna::GenerationStatus::InvalidEncoding &&
            Text.Bytes().empty() && !Lua.IsComplete() &&
            Lua.Status() == Luna::GenerationStatus::InvalidEncoding &&
            Lua.Bytes().empty(),
        "metadata that is not canonical UTF-8 rejects both generators with no "
        "bytes");

  // A rejection is local to its own attempt: the pinned surface still generates
  // exactly its pinned artifacts afterwards.
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  static_cast<void>(MatchesGolden(
      DocumentationGolden,
      Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions())
          .Bytes()));
  static_cast<void>(MatchesGolden(
      DeclarationGolden,
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions())
          .Bytes()));
}

// Requirement 16.6: publishing the complete surface writes exactly the pinned
// bytes, and every later refusal preserves that destination byte for byte and
// leaves no other file behind.
void CheckFailedGenerationPreservesThePriorDestination() {
  const ScratchDirectory Scratch("published");
  const std::filesystem::path DocumentationPath = Scratch.File("api.md");
  const std::filesystem::path DeclarationPath = Scratch.File("api.d.lua");

  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = CompleteSurface(Owner, 0);
  Check(Luna::PublishDocumentation(Snapshot, Luna::DocumentationOptions(),
                                   DocumentationPath.string())
            .IsPublished(),
        "the complete surface publishes its documentation");
  Check(Luna::PublishDeclarations(Snapshot, Luna::DeclarationOptions(),
                                  DeclarationPath.string())
            .IsPublished(),
        "the complete surface publishes its declarations");

  const std::optional<std::string> PublishedText = ReadBytes(DocumentationPath);
  const std::optional<std::string> PublishedLua = ReadBytes(DeclarationPath);
  Check(PublishedText.has_value() && PublishedLua.has_value(),
        "both destinations exist after publication");
  if (!PublishedText || !PublishedLua)
    return;
  static_cast<void>(MatchesGolden(DocumentationGolden, *PublishedText));
  static_cast<void>(MatchesGolden(DeclarationGolden, *PublishedLua));

  // A generation rejection refuses publication before the destination is
  // touched at all.
  Luna::State Reserved;
  Luna::BindingRegistry Registry = Reserved.Bindings();
  Check(Registry.RegisterFunction("end", [] { return 1; }).IsSuccess(),
        "the unsupported surface commits");
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.Documentation("Double", "bad \xff byte"));
  Check(Studio.Commit().IsSuccess(), "the unencodable metadata commits");
  const Luna::ReflectionSnapshot Rejected = Registry.Reflection();

  const Luna::ArtifactPublication RefusedText = Luna::PublishDocumentation(
      Rejected, Luna::DocumentationOptions(), DocumentationPath.string());
  const Luna::ArtifactPublication RefusedLua = Luna::PublishDeclarations(
      Rejected, Luna::DeclarationOptions(), DeclarationPath.string());
  Check(!RefusedText.IsPublished() &&
            RefusedText.Status() ==
                Luna::PublicationStatus::IncompleteArtifact &&
            RefusedText.Size() == 0 && !RefusedLua.IsPublished() &&
            RefusedLua.Status() == Luna::PublicationStatus::IncompleteArtifact,
        "a rejected generation refuses publication");

  // Bytes that are complete but not canonical are refused by publication
  // itself, under the same all-or-nothing rule.
  const Luna::ArtifactPublication NonCanonical = Luna::PublishArtifact(
      Luna::GeneratedArtifact::Complete("# Windows\r\nendings\r\n"),
      DocumentationPath.string());
  Check(!NonCanonical.IsPublished() &&
            NonCanonical.Status() ==
                Luna::PublicationStatus::NonCanonicalArtifact,
        "non-canonical bytes refuse publication");

  Check(ReadBytes(DocumentationPath) == PublishedText &&
            ReadBytes(DeclarationPath) == PublishedLua,
        "every refusal preserves the prior destination byte for byte");
  std::vector<std::string> Names = Scratch.Names();
  std::sort(Names.begin(), Names.end());
  Check(Names == std::vector<std::string>{"api.d.lua", "api.md"},
        "no refusal left an unpublished file behind");
}

} // namespace

int RunGeneratorArtifactGoldenTests() {
  FailureCount = 0;
  if (DumpGoldenArtifacts)
    return DumpEveryGoldenArtifact() ? 0 : 1;
  CheckGoldenArtifactsPinTheCompleteSurface();
  CheckEveryRegistrationOrderReproducesTheGolden();
  CheckDocumentationStructureFollowsTheSnapshot();
  CheckDeclarationStructureFollowsTheSnapshot();
  CheckRetainedSnapshotGeneratesTheGoldenAcrossEveryLifecycleEvent();
  CheckUnsupportedMetadataRejectsDeterministically();
  CheckFailedGenerationPreservesThePriorDestination();
  return FailureCount == 0 ? 0 : 1;
}
