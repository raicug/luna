// A playground for Luna's reflection-driven binding surface.
//
// The host registers one representative surface through the public API only:
// root functions, an overload set, optional, defaulted and variadic parameters,
// two kinds of multiple return, nested namespaces with constants, a scoped
// enumeration with an alias, a bitflag enumeration, a class hierarchy with a
// constructor, a factory, methods, a static method, fields, a lazy property and
// operators, and a versioned module graph with a resolved dependency. Every
// declaration records the `RegistrationResult` it produced, so a refusal is
// visible instead of silent.
//
// The window then lets you run Luau against that surface, browse the captured
// reflection, generate the documentation and `.d.lua` artifacts, freeze the
// State, and read the exact C++ snippet each feature was bound with.

// clang-format off
#include <luna/luna.hpp>

#include <TextEditor.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// clang-format on

namespace {

// ---------------------------------------------------------------------------
// The native model the host exposes
// ---------------------------------------------------------------------------

// One scoped enumeration. Scoped behaviour is Luna's default, so nothing has to
// be opted into here.
enum class Channel : int { Debug = 10, Info = 20, Warning = 30, Error = 40 };

// One scoped enumeration that also declares bitflag behaviour, so a combined
// mask converts back into the host as one whole value.
enum class Access : unsigned int { Read = 1, Write = 2, Execute = 4 };

// Compact text for one number, so the demo's own output stays readable.
[[nodiscard]] std::string NumberText(double Value) {
  std::string Formatted = std::to_string(Value);
  while (Formatted.size() > 1 && Formatted.back() == '0')
    Formatted.pop_back();
  if (!Formatted.empty() && Formatted.back() == '.')
    Formatted.pop_back();
  return Formatted;
}

// The base of the exposed hierarchy.
class Entity {
public:
  Entity() = default;
  explicit Entity(std::string Name) : NameValue(std::move(Name)) {}

  Entity(const Entity &) = default;
  Entity &operator=(const Entity &) = default;
  Entity(Entity &&) = default;
  Entity &operator=(Entity &&) = default;
  virtual ~Entity() = default;

  // One read-write property, declared as a getter plus a setter.
  [[nodiscard]] std::string Name() const { return NameValue; }
  void Rename(std::string Renamed) { NameValue = std::move(Renamed); }

  // One instance method.
  [[nodiscard]] std::string Label() const {
    return "entity '" + NameValue + "' #" + std::to_string(Tag);
  }

  // One static method: no receiver at all.
  [[nodiscard]] static std::string Category() { return "entity"; }

  // One field, copied across the member boundary in both directions.
  int Tag = 0;

private:
  std::string NameValue;
};

// The derived class. It declares its base edge explicitly, which is the only
// thing that lets a sprite be received as an entity.
class Sprite final : public Entity {
public:
  Sprite() = default;

  Sprite(std::string Name, double Width, double Height)
      : Entity(std::move(Name)), Width(Width), Height(Height) {}

  double Width = 1.0;
  double Height = 1.0;

  // The lazy property: computed once, then reused.
  [[nodiscard]] double Area() const { return Width * Height; }

  // One method with ordered multiple returns.
  [[nodiscard]] std::pair<double, double> Bounds() const {
    return {Width, Height};
  }

  // One mutating method: a const receiver refuses it before native code runs.
  double Grow(double Factor) {
    Width *= Factor;
    Height *= Factor;
    return Area();
  }

  // The operand of an operator is an ordinary parameter, so it is one of the
  // supported value types rather than another class.
  [[nodiscard]] double Padded(double Padding) const {
    return (Width + Padding) * (Height + Padding);
  }

  [[nodiscard]] int Pixels() const {
    return static_cast<int>(std::lround(Area()));
  }

  [[nodiscard]] std::string ToText() const {
    return "Sprite(" + Name() + ", " + NumberText(Width) + "x" +
           NumberText(Height) + ")";
  }

  [[nodiscard]] static std::string Category() { return "sprite"; }

  // One factory: returning the class by value states Lua ownership.
  [[nodiscard]] static Sprite Square(std::string Name, double Side) {
    return Sprite(std::move(Name), Side, Side);
  }
};

// -- root callables ---------------------------------------------------------

// Two candidates of one overload set, selected by declared signature.
[[nodiscard]] int Measure(std::string Text) {
  return static_cast<int>(Text.size());
}

[[nodiscard]] int Measure(int Width, int Height) { return Width * Height; }

// One optional parameter: omission and an explicit nil are both empty.
[[nodiscard]] std::string Greet(std::string Name,
                                std::optional<std::string> Title) {
  if (Title && !Title->empty())
    return "Hello, " + *Title + " " + Name + ".";
  return "Hello, " + Name + ".";
}

// One defaulted parameter, declared with `WithDefaults` at registration.
[[nodiscard]] std::string Shorten(std::string Text, int Limit) {
  if (Limit <= 0 || Text.size() <= static_cast<std::size_t>(Limit))
    return Text;
  return Text.substr(0, static_cast<std::size_t>(Limit)) + "...";
}

// One variadic parameter, in its callback-lifetime form. Every element reports
// the one-based call position it came from, and every accessor answers from the
// argument frame Luna opened for this invocation only.
[[nodiscard]] std::string Join(Luna::ArgumentView Arguments) {
  std::string Joined;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (!Joined.empty())
      Joined += ", ";
    Joined += std::to_string(Arguments.Position(Index));
    Joined += ':';
    Joined += Luna::ValueCategoryText(Arguments.Kind(Index));
    Joined += '=';
    if (const std::optional<std::string> Text = Arguments.ToText(Index))
      Joined += *Text;
    else if (const std::optional<double> Number = Arguments.ToNumber(Index))
      Joined += NumberText(*Number);
    else if (const std::optional<bool> Flag = Arguments.ToBoolean(Index))
      Joined += *Flag ? "true" : "false";
    else
      Joined += "(not convertible)";
  }
  return Joined.empty() ? std::string("no variadic arguments") : Joined;
}

// Ordered multiple returns whose count is fixed by the signature.
[[nodiscard]] std::tuple<int, int, std::string> Analyze(std::string Text) {
  int Words = Text.empty() ? 0 : 1;
  std::string Upper;
  Upper.reserve(Text.size());
  for (const char Character : Text) {
    if (Character == ' ')
      ++Words;
    Upper.push_back(Character >= 'a' && Character <= 'z'
                        ? static_cast<char>(Character - ('a' - 'A'))
                        : Character);
  }
  return {static_cast<int>(Text.size()), Words, std::move(Upper)};
}

// Ordered multiple returns whose count is decided by the invocation.
[[nodiscard]] Luna::ReturnPack Tally(Luna::ArgumentView Arguments) {
  int Numbers = 0;
  double Sum = 0.0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (const std::optional<double> Number = Arguments.ToNumber(Index)) {
      ++Numbers;
      Sum += *Number;
    }
  }

  Luna::ReturnPack Pack;
  Pack.AppendInteger(static_cast<int>(Arguments.Size()))
      .AppendInteger(Numbers)
      .AppendNumber(Sum);
  return Pack;
}

// One host reader of a declared bitflag mask.
[[nodiscard]] std::string DescribeAccess(int Mask) {
  std::string Described;
  const auto Append = [&Described](std::string_view Name) {
    if (!Described.empty())
      Described += '|';
    Described.append(Name);
  };
  if ((Mask & static_cast<int>(Access::Read)) != 0)
    Append("read");
  if ((Mask & static_cast<int>(Access::Write)) != 0)
    Append("write");
  if ((Mask & static_cast<int>(Access::Execute)) != 0)
    Append("execute");
  return Described.empty() ? std::string("none") : Described;
}

// -- module callables -------------------------------------------------------

[[nodiscard]] double ToPixels(double Metres) { return Metres * 64.0; }

[[nodiscard]] std::string DescribePasses(int Passes) {
  return "render graph with " + std::to_string(Passes) + " pass(es)";
}

// ---------------------------------------------------------------------------
// Stable identities and module manifests
// ---------------------------------------------------------------------------

// A user-defined leaf is accepted only with an explicit validated stable key,
// so the identity of an enumeration or a class never derives from an RTTI name
// or an address.
[[nodiscard]] Luna::StableTypeKey ChannelKey() {
  return Luna::StableTypeKey("demo.studio.Channel");
}

[[nodiscard]] Luna::StableTypeKey AccessKey() {
  return Luna::StableTypeKey("demo.studio.Access");
}

[[nodiscard]] Luna::StableTypeKey EntityKey() {
  return Luna::StableTypeKey("demo.studio.Entity");
}

[[nodiscard]] Luna::StableTypeKey SpriteKey() {
  return Luna::StableTypeKey("demo.studio.Sprite");
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const std::optional<Luna::SemanticVersion> Parsed =
      Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency Dependency(std::string Identity,
                                                std::string_view Constraint) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  if (const std::optional<Luna::VersionConstraint> Parsed =
          Luna::VersionConstraint::TryParse(Constraint))
    Declared.Constraints.push_back(*Parsed);
  return Declared;
}

[[nodiscard]] Luna::ModuleExport
Exported(Luna::SymbolKind Kind, std::string Name, std::string Documentation) {
  Luna::ModuleExport Declared;
  Declared.Kind = Kind;
  Declared.Name = std::move(Name);
  Declared.Documentation = std::move(Documentation);
  return Declared;
}

// The dependency of the graph. Two versions are made available, so resolution
// has an actual choice to make.
[[nodiscard]] Luna::ModuleManifest UnitsManifest(std::string_view VersionText) {
  std::vector<Luna::ModuleExport> Exports;
  Exports.push_back(Exported(Luna::SymbolKind::Namespace, "Units",
                             "Unit constants and conversions."));
  std::optional<Luna::ModuleManifest> Created =
      Luna::ModuleManifest::TryCreate("studio.units", Version(VersionText), {},
                                      "The unit module.", std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

// The requested module. Its declared constraint is what makes the resolver
// select a version rather than a definition.
[[nodiscard]] Luna::ModuleManifest RenderManifest() {
  std::vector<Luna::ModuleDependency> Dependencies;
  Dependencies.push_back(Dependency("studio.units", ">=1.0.0"));

  std::vector<Luna::ModuleExport> Exports;
  Exports.push_back(
      Exported(Luna::SymbolKind::Namespace, "Render", "The render surface."));
  Exports.push_back(Exported(Luna::SymbolKind::Constant, "Render.Backend",
                             "The configured backend name."));

  std::optional<Luna::ModuleManifest> Created = Luna::ModuleManifest::TryCreate(
      "studio.render", Version("2.1.0"), std::move(Dependencies),
      "The render module, which depends on the unit module.",
      std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

// One module registration callback. It receives a transaction-attached builder,
// so everything it stages joins the load's one outermost transaction and
// nothing here commits on its own.
void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  Units.RegisterConstant("Metre", 1)
      .RegisterConstant("Pixel", 64)
      .RegisterFunction("ToPixels", &ToPixels)
      .Documentation("Unit constants and conversions.")
      .Documentation("ToPixels", "Converts metres into device pixels.");
}

void ConfigureRender(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Render = Builder.RegisterNamespace("Render");
  Render.RegisterConstant("Backend", "opengl3")
      .RegisterConstant("Passes", 2)
      .RegisterFunction("Describe", &DescribePasses)
      .Documentation("The render surface published by studio.render.")
      .Attribute("owner", "demo")
      .Example("HostLog(Render.Describe(Render.Passes))");
}

// ---------------------------------------------------------------------------
// The bound-with snippets shown in the UI
// ---------------------------------------------------------------------------

struct BoundFeature final {
  std::string_view Name;
  std::string_view Snippet;
};

constexpr std::array<BoundFeature, 14> BoundFeatures{{
    {"Root function (Register)",
     R"(Registry.Register("HostLog", [this](std::string Message) {
  OutputLines.push_back(std::move(Message));
});)"},
    {"Root function (RegisterFunction)",
     R"(Registry.RegisterFunction("Join", &Join);
Registry.RegisterConstant("HostName", "Luna playground");)"},
    {"Overload set",
     R"(// One name, two candidates. `Overload<Signature>` selects the C++
// target by its declared signature, with no macro involved.
Registry.RegisterFunction("Measure",
                          Luna::Overload<int(std::string)>(&Measure));
Registry.RegisterFunction("Measure",
                          Luna::Overload<int(int, int)>(&Measure));)"},
    {"Optional parameter",
     R"(// std::string Greet(std::string Name, std::optional<std::string> Title)
// Omission and an explicit nil both arrive as the empty value.
Registry.RegisterFunction("Greet", &Greet);)"},
    {"Defaulted parameter",
     R"(// std::string Shorten(std::string Text, int Limit)
// The default is validated at registration and materialized only when the
// parameter is omitted.
Text.RegisterFunction("Shorten", Luna::WithDefaults(&Shorten, 8));)"},
    {"Variadic parameter",
     R"(// std::string Join(Luna::ArgumentView Arguments)
// The view is callback-lifetime only; `ToOwned()` is the way to keep it.
Registry.RegisterFunction("Join", &Join);)"},
    {"Multiple returns",
     R"(// std::tuple<int, int, std::string> Analyze(std::string Text)
Registry.RegisterFunction("Analyze", &Analyze);

// Luna::ReturnPack Tally(Luna::ArgumentView Arguments)
// The dynamic form: the element count is decided by the invocation.
Registry.RegisterFunction("Tally", &Tally);)"},
    {"Nested namespaces and constants",
     R"(Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
Studio.RegisterConstant("Version", "0.1.0")
    .RegisterConstant("MaxSprites", 512)
    .RegisterConstant("DefaultChannel", Channel::Info, ChannelKey())
    .Documentation("The demo surface.")
    .Attribute("stability", "experimental");

Luna::NamespaceBuilder Text = Studio.RegisterNamespace("Text");
Text.RegisterConstant("Ellipsis", "...");

// One plan, one transaction: the whole namespace publishes or none of it.
return Studio.Commit();)"},
    {"Scoped enumeration with an "
     "alias",
     R"CPP(Luna::EnumBuilder<Channel> Channels =
    Studio.RegisterEnum<Channel>("Channel", ChannelKey());
Channels.Value("Debug", Channel::Debug)
    .Value("Info", Channel::Info)
    .Value("Warning", Channel::Warning)
    .Value("Error", Channel::Error)
    .Alias("Default", "Info")
    .Documentation("Host log channels.")
    .Documentation("Default", "A second name for one canonical enumerator.")
    .Example("HostLog(Studio.Channel.Default)");)CPP"},
    {"Bitflag enumeration",
     R"(Luna::EnumBuilder<Access> Flags =
    Studio.RegisterEnum<Access>("Access", AccessKey());
Flags.Value("Read", Access::Read)
    .Value("Write", Access::Write)
    .Value("Execute", Access::Execute)
    .Bitflags()
    .Documentation("Declared bitflags: the supported mask is checked.");)"},
    {"Base class",
     R"CPP(Luna::ClassBuilder<Entity> Entities =
    Studio.RegisterClass<Entity>("Entity", EntityKey());
Entities.Constructor<std::string>()
    .Field("Tag", &Entity::Tag)
    .Property("Name", &Entity::Name, &Entity::Rename)
    .Method("Label", &Entity::Label)
    .StaticMethod("Category", &Entity::Category)
    .Documentation("One named host object.")
    .Documentation("New", "Constructs one entity from its name.")
    .Example("local E = Studio.Entity.New('crate')");)CPP"},
    {"Derived class, factory, lazy property, operators",
     R"(Luna::ClassBuilder<Sprite> Sprites =
    Studio.RegisterClass<Sprite>("Sprite", SpriteKey());
Sprites.Base<Entity>(EntityKey())
    .Constructor<std::string, double, double>()
    .Factory("Square", &Sprite::Square)
    .Field("Width", &Sprite::Width)
    .Field("Height", &Sprite::Height)
    .Property("Area", Luna::PropertyPolicy::Lazy(), &Sprite::Area)
    .Property("Name", [](const Sprite &Value) { return Value.Name(); },
              [](Sprite &Value, std::string Renamed) {
                Value.Rename(std::move(Renamed));
              })
    .Method("Bounds", &Sprite::Bounds)
    .Method("Grow", &Sprite::Grow)
    .StaticMethod("Category", &Sprite::Category)
    .Operator(Luna::ClassOperator::Add, &Sprite::Padded)
    .Operator(Luna::ClassOperator::Length, &Sprite::Pixels)
    .Operator(Luna::ClassOperator::ToText, &Sprite::ToText)
    .Documentation("One drawable entity.")
    .Documentation("Area", "Computed once, then reused.")
    .Attribute("Area", "evaluation", "lazy")
    .Documentation(Luna::ClassOperator::Add,
                   "Sprite + padding is the padded area.");)"},
    {"Versioned module graph",
     R"(// Two versions of the dependency become available without loading
// anything, so resolution has a choice to make.
Registry.ProvideModule(UnitsManifest("1.0.0"), &ConfigureUnits);
Registry.ProvideModule(UnitsManifest("1.2.0"), &ConfigureUnits);

// The load resolves `studio.units >=1.0.0` to the highest available
// version, then runs every callback dependency-first in one transaction.
Registry.RegisterModule(RenderManifest(), &ConfigureRender);)"},
    {"Freeze, reflection, and generation",
     R"(const Luna::RegistrationResult Frozen = Registry.Freeze();

// One captured generation. It stays readable after later registrations, a
// freeze, a State move, and destruction of the State.
const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();

const Luna::GeneratedArtifact Documentation =
    Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions());
const Luna::GeneratedArtifact Declarations =
    Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());)"},
}};

// ---------------------------------------------------------------------------
// The example scripts the editor can load
// ---------------------------------------------------------------------------

struct ExampleScript final {
  std::string_view Name;
  std::string_view Source;
};

constexpr std::array<ExampleScript, 6> ExampleScripts{{
    {"Root functions, parameters and returns",
     R"LUA(-- Root scope: Register, RegisterFunction, RegisterConstant.
HostLog("HostName = " .. HostName)

-- One overload set, two candidates, selected by declared shape.
HostLog("Measure('playground') = " .. Measure("playground"))
HostLog("Measure(3, 4) = " .. Measure(3, 4))

-- std::optional<std::string>: omitted, then supplied.
HostLog(Greet("Ada"))
HostLog(Greet("Ada", "Doctor"))

-- Luna::WithDefaults: the limit defaults to 8.
HostLog(Studio.Text.Shorten("reflection-driven"))
HostLog(Studio.Text.Shorten("reflection-driven", 4))

-- Luna::ArgumentView: every trailing argument, with its call position.
HostLog(Join(1, "two", true, 4.5))

-- std::tuple: the return count is fixed by the signature.
local Length, Words, Upper = Analyze("luna binds cpp")
HostLog(("Analyze -> %d chars, %d words, %s"):format(Length, Words, Upper))

-- Luna::ReturnPack: the return count is decided by the invocation.
local Count, Numbers, Sum = Tally(1, 2, "three", 4)
HostLog(("Tally -> %d values, %d numbers, sum %.1f"):format(
    Count, Numbers, Sum))
)LUA"},
    {"Namespaces and constants",
     R"LUA(-- One namespace plan published as one transaction.
HostLog("Studio.Version = " .. Studio.Version)
HostLog("Studio.MaxSprites = " .. Studio.MaxSprites)

-- A constant whose canonical type is the registered enumeration.
HostLog("Studio.DefaultChannel = " .. Studio.DefaultChannel)

-- A nested namespace, staged by the same plan.
HostLog("Studio.Text.Ellipsis = " .. Studio.Text.Ellipsis)
HostLog(Studio.Text.Shorten("nested namespaces stay one plan", 12))
)LUA"},
    {"Enumerations and bitflags",
     R"LUA(-- A scoped enumeration. Every enumerator is validated against the
-- declared underlying type at registration.
HostLog("Studio.Channel.Debug = " .. Studio.Channel.Debug)
HostLog("Studio.Channel.Warning = " .. Studio.Channel.Warning)

-- An alias is a second name for one canonical enumerator, which is the only
-- way a duplicate value is accepted.
HostLog("Studio.Channel.Default = " .. Studio.Channel.Default)
HostLog("alias is Info: " ..
    tostring(Studio.Channel.Default == Studio.Channel.Info))

-- Declared bitflags: the host reads one combined mask back.
local Mask = bit32.bor(Studio.Access.Read, Studio.Access.Write)
HostLog("mask " .. Mask .. " -> " .. Studio.DescribeAccess(Mask))
HostLog("execute -> " .. Studio.DescribeAccess(Studio.Access.Execute))
HostLog("nothing -> " .. Studio.DescribeAccess(0))
)LUA"},
    {"Class hierarchy, members and operators",
     R"LUA(-- Constructor, fields, lazy property, methods, operators, inheritance.
local Hero = Studio.Sprite.New("hero", 3, 4)
HostLog("tostring(Hero) = " .. tostring(Hero))
HostLog("Hero.Width = " .. Hero.Width)
HostLog("Hero.Area (lazy property) = " .. Hero.Area)
HostLog("#Hero (Length operator) = " .. #Hero)
HostLog("Hero + 2 (Add operator) = " .. (Hero + 2))

-- One method with ordered multiple returns.
local Width, Height = Hero:Bounds()
HostLog(("Hero:Bounds() = %.1f x %.1f"):format(Width, Height))

-- The receiver is rank position zero, so all three spellings are one call.
HostLog("Hero:Grow(2) = " .. Hero:Grow(2))
HostLog("Studio.Sprite.Grow(Hero, 1) = " .. Studio.Sprite.Grow(Hero, 1))
HostLog("Hero.Width after growing = " .. Hero.Width)

-- The property is declared lazy, so the first computed value is reused.
HostLog("Hero.Area is still " .. Hero.Area)

-- The sprite declares its own read-write name property.
Hero.Name = "champion"
HostLog("Hero.Name = " .. Hero.Name)

-- One registered base edge is what lets a sprite be received as an entity, so
-- the base method reaches it through an adjusted receiver.
HostLog("Studio.Entity.Label(Hero) = " .. Studio.Entity.Label(Hero))

-- Each class declares the members it exposes: the tag field belongs to the
-- base, so it is reached through a value of the base.
local Crate = Studio.Entity.New("crate")
Crate.Tag = 7
HostLog("Crate.Tag = " .. Crate.Tag .. ", Crate:Label() = " .. Crate:Label())

-- A static method declares no receiver, and a factory states Lua ownership.
HostLog("Studio.Sprite.Category() = " .. Studio.Sprite.Category())
HostLog("Studio.Entity.Category() = " .. Studio.Entity.Category())
HostLog("Square factory = " .. tostring(Studio.Sprite.Square("tile", 2)))
)LUA"},
    {"Module symbols",
     R"LUA(-- Both modules of the resolved graph published in one transaction. The
-- Reflection tab names the module and version every symbol came from.
HostLog("Units.Metre = " .. Units.Metre)
HostLog("Units.Pixel = " .. Units.Pixel)
HostLog("Units.ToPixels(2.5) = " .. Units.ToPixels(2.5))
HostLog("Render.Backend = " .. Render.Backend)
HostLog("Render.Passes = " .. Render.Passes)
HostLog("Render.Describe(Render.Passes) = " ..
    Render.Describe(Render.Passes))
)LUA"},
    {"Deterministic diagnostics",
     R"LUA(-- Every refusal names what failed and reports the same message each time.
HostLog("this line runs first")

-- No candidate of the overload set accepts a table.
local Fine, Message = pcall(function() return Measure({}) end)
HostLog("Measure({}) -> " .. tostring(Message))

-- An operator operand is prevalidated before native code runs.
local Hero = Studio.Sprite.New("hero", 2, 3)
local Second, OperandMessage = pcall(function() return Hero + "wide" end)
HostLog("Hero + 'wide' -> " .. tostring(OperandMessage))

-- A defaulted parameter still checks the value you do supply.
local Third, DefaultMessage = pcall(function()
  return Studio.Text.Shorten("text", "four")
end)
HostLog("Shorten('text', 'four') -> " .. tostring(DefaultMessage))
)LUA"},
}};

void ReportGlfwError(int Code, const char *Message) {
  std::cerr << "GLFW error " << Code << ": "
            << (Message ? Message : "unknown error") << '\n';
}

[[nodiscard]] std::string Describe(const Luna::ErrorDiagnostic *Diagnostic) {
  if (Diagnostic == nullptr)
    return "Luna reported a failure without a diagnostic.";
  return Diagnostic->Message();
}

void TextView(std::string_view Text) {
  if (Text.empty()) {
    ImGui::TextUnformatted("");
    return;
  }
  ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
}

// ---------------------------------------------------------------------------
// The playground window
// ---------------------------------------------------------------------------

class Playground final {
public:
  Playground() {
    Editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    Editor.SetText(std::string(ExampleScripts[0].Source));
    Build();
  }

  Playground(const Playground &) = delete;
  Playground &operator=(const Playground &) = delete;

  void Draw() {
    const ImGuiViewport *Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->WorkPos);
    ImGui::SetNextWindowSize(Viewport->WorkSize);

    constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoDecoration |
                                             ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Luna playground", nullptr, WindowFlags);

    DrawToolbar();
    DrawStatus();
    ImGui::Separator();

    if (ImGui::BeginTabBar("Panels", ImGuiTabBarFlags_None)) {
      DrawScriptTab();
      DrawOutputTab();
      DrawReflectionTab();
      DrawArtifactTab();
      DrawSnippetTab();
      DrawDiagnosticTab();
      ImGui::EndTabBar();
    }

    ImGui::End();

    if (ShowImGuiDemo)
      ImGui::ShowDemoWindow(&ShowImGuiDemo);
  }

private:
  struct LogEntry final {
    std::string Origin;
    std::string Detail;
    bool Succeeded = false;
  };

  // -- registration ---------------------------------------------------------

  void Build() {
    Log.clear();
    OutputLines.clear();
    DocumentationText.clear();
    DeclarationText.clear();
    Frozen = false;
    ProbeCount = 0;

    if (!State.IsReady()) {
      SetStatus(false, "Luna could not create a Luau state.");
      return;
    }

    RegisterRootSurface();
    RegisterStudioSurface();
    RegisterModuleGraph();
    RefreshSnapshot();

    const std::size_t Failures = FailureCount();
    if (Failures == 0) {
      SetStatus(true, "Registered " + std::to_string(Log.size()) +
                          " declarations. Choose an example and press Run.");
      return;
    }
    SetStatus(false, std::to_string(Failures) +
                         " registration(s) were refused; the Diagnostics tab "
                         "names each one.");
  }

  // Root scope: the two spellings of function registration, one overload set,
  // an optional parameter, a variadic parameter, and both return packs.
  void RegisterRootSurface() {
    Luna::BindingRegistry Registry = State.Bindings();

    Record("Register(\"HostLog\")",
           Registry.Register("HostLog", [this](std::string Message) {
             OutputLines.push_back(std::move(Message));
           }));
    Record("RegisterConstant(\"HostName\")",
           Registry.RegisterConstant("HostName", "Luna playground"));
    Record("RegisterFunction(\"Measure\", Overload<int(std::string)>)",
           Registry.RegisterFunction(
               "Measure", Luna::Overload<int(std::string)>(&Measure)));
    Record("RegisterFunction(\"Measure\", Overload<int(int, int)>)",
           Registry.RegisterFunction("Measure",
                                     Luna::Overload<int(int, int)>(&Measure)));
    Record("RegisterFunction(\"Greet\")",
           Registry.RegisterFunction("Greet", &Greet));
    Record("RegisterFunction(\"Join\")",
           Registry.RegisterFunction("Join", &Join));
    Record("RegisterFunction(\"Analyze\")",
           Registry.RegisterFunction("Analyze", &Analyze));
    Record("RegisterFunction(\"Tally\")",
           Registry.RegisterFunction("Tally", &Tally));
  }

  // One namespace plan: nested namespaces, constants, two enumerations, and a
  // class hierarchy. Nothing here reaches the virtual machine before `Commit`,
  // so a single refusal publishes none of it.
  void RegisterStudioSurface() {
    Luna::BindingRegistry Registry = State.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

    Studio.RegisterConstant("Version", "0.1.0")
        .RegisterConstant("MaxSprites", 512)
        .RegisterConstant("DefaultChannel", Channel::Info, ChannelKey())
        .RegisterFunction("DescribeAccess", &DescribeAccess)
        .Documentation("The demo surface.")
        .Documentation("MaxSprites", "How many sprites the demo would draw.")
        .Attribute("stability", "experimental")
        .Example("HostLog(Studio.Version)");

    Luna::NamespaceBuilder Text = Studio.RegisterNamespace("Text");
    Text.RegisterConstant("Ellipsis", "...")
        .RegisterFunction("Shorten", Luna::WithDefaults(&Shorten, 8))
        .Documentation("Text helpers.")
        .Documentation("Shorten", "The limit defaults to eight characters.")
        .Example("HostLog(Studio.Text.Shorten('reflection-driven'))");

    Luna::EnumBuilder<Channel> Channels =
        Studio.RegisterEnum<Channel>("Channel", ChannelKey());
    Channels.Value("Debug", Channel::Debug)
        .Value("Info", Channel::Info)
        .Value("Warning", Channel::Warning)
        .Value("Error", Channel::Error)
        .Alias("Default", "Info")
        .Documentation("Host log channels.")
        .Documentation("Default", "A second name for one canonical enumerator.")
        .Attribute("owner", "demo")
        .Example("HostLog(Studio.Channel.Default)");

    Luna::EnumBuilder<Access> Flags =
        Studio.RegisterEnum<Access>("Access", AccessKey());
    Flags.Value("Read", Access::Read)
        .Value("Write", Access::Write)
        .Value("Execute", Access::Execute)
        .Bitflags()
        .Documentation("Declared bitflags, so a combined mask converts whole.")
        .Example("Studio.DescribeAccess(bit32.bor(Studio.Access.Read, 2))");

    Luna::ClassBuilder<Entity> Entities =
        Studio.RegisterClass<Entity>("Entity", EntityKey());
    Entities.Constructor<std::string>()
        .Field("Tag", &Entity::Tag)
        .Property("Name", &Entity::Name, &Entity::Rename)
        .Method("Label", &Entity::Label)
        .StaticMethod("Category", &Entity::Category)
        .Documentation("One named host object.")
        .Documentation("New", "Constructs one entity from its name.")
        .Documentation("Name", "The entity name. Reading and writing are both "
                               "declared, so both are permitted.")
        .Attribute("stability", "experimental")
        .Example("local E = Studio.Entity.New('crate')");

    Luna::ClassBuilder<Sprite> Sprites =
        Studio.RegisterClass<Sprite>("Sprite", SpriteKey());
    Sprites.Base<Entity>(EntityKey())
        .Constructor<std::string, double, double>()
        .Factory("Square", &Sprite::Square)
        .Field("Width", &Sprite::Width)
        .Field("Height", &Sprite::Height)
        .Property("Area", Luna::PropertyPolicy::Lazy(), &Sprite::Area)
        .Property(
            "Name", [](const Sprite &Value) { return Value.Name(); },
            [](Sprite &Value, std::string Renamed) {
              Value.Rename(std::move(Renamed));
            })
        .Method("Bounds", &Sprite::Bounds)
        .Method("Grow", &Sprite::Grow)
        .StaticMethod("Category", &Sprite::Category)
        .Operator(Luna::ClassOperator::Add, &Sprite::Padded)
        .Operator(Luna::ClassOperator::Length, &Sprite::Pixels)
        .Operator(Luna::ClassOperator::ToText, &Sprite::ToText)
        .Documentation("One drawable entity.")
        .Documentation("Square", "Produces one square sprite by value.")
        .Documentation("Area", "Computed once, then reused for that object.")
        .Attribute("Area", "evaluation", "lazy")
        .Documentation(Luna::ClassOperator::Add,
                       "Sprite + padding is the padded area.")
        .Example("local S = Studio.Sprite.Square('tile', 2)");

    Record("NamespaceBuilder(\"Studio\")::Commit", Studio.Commit());
  }

  // One versioned module graph. Two dependency versions become available
  // without loading anything, so the load actually resolves a constraint.
  void RegisterModuleGraph() {
    Luna::BindingRegistry Registry = State.Bindings();

    Record("ProvideModule(studio.units@1.0.0)",
           Registry.ProvideModule(UnitsManifest("1.0.0"), &ConfigureUnits));
    Record("ProvideModule(studio.units@1.2.0)",
           Registry.ProvideModule(UnitsManifest("1.2.0"), &ConfigureUnits));
    Record("RegisterModule(studio.render@2.1.0)",
           Registry.RegisterModule(RenderManifest(), &ConfigureRender));
  }

  // -- bookkeeping ----------------------------------------------------------

  void Record(std::string Origin, const Luna::RegistrationResult &Result) {
    Record(std::move(Origin), Result.IsSuccess(),
           Result.IsSuccess() ? std::string("accepted")
                              : Describe(Result.Diagnostic()));
  }

  void Record(std::string Origin, const Luna::ExecutionResult &Result) {
    Record(std::move(Origin), Result.IsSuccess(),
           Result.IsSuccess() ? std::string("accepted")
                              : Describe(Result.Diagnostic()));
  }

  void Record(std::string Origin, bool Succeeded, std::string Detail) {
    LogEntry Entry;
    Entry.Origin = std::move(Origin);
    Entry.Detail = std::move(Detail);
    Entry.Succeeded = Succeeded;
    Log.push_back(std::move(Entry));
  }

  [[nodiscard]] std::size_t FailureCount() const {
    std::size_t Count = 0;
    for (const LogEntry &Entry : Log) {
      if (!Entry.Succeeded)
        ++Count;
    }
    return Count;
  }

  void SetStatus(bool Succeeded, std::string Message) {
    StatusSucceeded = Succeeded;
    Status = std::move(Message);
  }

  // -- actions --------------------------------------------------------------

  void RunScript() {
    OutputLines.clear();
    if (!State.IsReady()) {
      SetStatus(false, "The Luau state is not ready.");
      return;
    }

    const Luna::ExecutionResult Result = State.Execute(Editor.GetText());
    Record("Execute(editor source)", Result);
    SetStatus(Result.IsSuccess(), Result.IsSuccess()
                                      ? std::string("Script finished "
                                                    "successfully.")
                                      : Describe(Result.Diagnostic()));
  }

  void LoadExample(std::size_t Index) {
    if (Index >= ExampleScripts.size())
      return;
    SelectedExample = Index;
    Editor.SetText(std::string(ExampleScripts[Index].Source));
    OutputLines.clear();
    SetStatus(true, "Loaded the '" + std::string(ExampleScripts[Index].Name) +
                        "' example.");
  }

  // Freeze validates the whole committed model and publishes every runtime
  // lookup cache. Invocation and reflection keep working afterwards;
  // registration does not.
  void FreezeSurface() {
    const Luna::RegistrationResult Result = State.Bindings().Freeze();
    Record("BindingRegistry::Freeze", Result);
    if (Result.IsSuccess())
      Frozen = true;
    SetStatus(Result.IsSuccess(),
              Result.IsSuccess()
                  ? std::string("Frozen. Every lookup cache is published and "
                                "further registration is refused.")
                  : Describe(Result.Diagnostic()));
    RefreshSnapshot();
  }

  // One extra registration attempt, so the difference between the open and the
  // frozen lifecycle is something you can watch rather than take on trust.
  void ProbeRegistration() {
    ++ProbeCount;
    const std::string Name = "Probe" + std::to_string(ProbeCount);
    const Luna::RegistrationResult Result = State.Bindings().RegisterFunction(
        Name, [](int Value) { return Value + 1; });
    Record("RegisterFunction(\"" + Name + "\")", Result);
    SetStatus(Result.IsSuccess(),
              Result.IsSuccess()
                  ? "Registered " + Name +
                        "; the surface is still open to registration."
                  : Describe(Result.Diagnostic()));
    RefreshSnapshot();
  }

  void RebuildState() {
    State = Luna::State();
    Build();
  }

  void RefreshSnapshot() { Snapshot = State.Bindings().Reflection(); }

  [[nodiscard]] Luna::DocumentationOptions DocumentationChoices() const {
    return Luna::DocumentationOptions()
        .WithIdentities(WithIdentities)
        .WithAttributes(WithAttributes)
        .WithExamples(WithExamples)
        .WithTitle("Luna playground reference");
  }

  [[nodiscard]] Luna::DeclarationOptions DeclarationChoices() const {
    return Luna::DeclarationOptions()
        .WithStrictMode(WithStrictMode)
        .WithProvenance(WithProvenance)
        .WithDocumentation(WithDeclarationDocumentation);
  }

  // Both generators read one captured snapshot and nothing else, so neither one
  // touches the State they came from.
  void GenerateArtifacts() {
    const Luna::GeneratedArtifact Documented =
        Luna::GenerateDocumentation(Snapshot, DocumentationChoices());
    DocumentationComplete = Documented.IsComplete();
    DocumentationStatus =
        std::string(Luna::GenerationStatusText(Documented.Status()));
    DocumentationText = Documented.IsComplete()
                            ? Documented.Bytes()
                            : Describe(Documented.Diagnostic());
    Record("GenerateDocumentation", Documented.IsComplete(),
           Documented.IsComplete()
               ? std::to_string(Documented.Size()) + " bytes"
               : DocumentationStatus + " - " + DocumentationText);

    const Luna::GeneratedArtifact Declared =
        Luna::GenerateDeclarations(Snapshot, DeclarationChoices());
    DeclarationComplete = Declared.IsComplete();
    DeclarationStatus =
        std::string(Luna::GenerationStatusText(Declared.Status()));
    DeclarationText = Declared.IsComplete() ? Declared.Bytes()
                                            : Describe(Declared.Diagnostic());
    Record("GenerateDeclarations", Declared.IsComplete(),
           Declared.IsComplete() ? std::to_string(Declared.Size()) + " bytes"
                                 : DeclarationStatus + " - " + DeclarationText);

    const bool Both = DocumentationComplete && DeclarationComplete;
    SetStatus(Both, Both ? std::string("Generated both artifacts from "
                                       "generation " +
                                       std::to_string(Snapshot.Generation()))
                         : std::string("One generator refused; the Artifacts "
                                       "tab shows the diagnostic."));
  }

  // -- panels ---------------------------------------------------------------

  void DrawToolbar() {
    if (ImGui::Button("Run", ImVec2(80.0f, 0.0f)))
      RunScript();
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
      LoadExample(SelectedExample);
    ImGui::SameLine();
    if (ImGui::Button("Rebuild State"))
      RebuildState();
    ImGui::SameLine();
    if (ImGui::Button(Frozen ? "Frozen" : "Freeze"))
      FreezeSurface();
    ImGui::SameLine();
    if (ImGui::Button("Register one more function"))
      ProbeRegistration();
    ImGui::SameLine();
    if (ImGui::Button("Recapture reflection")) {
      RefreshSnapshot();
      SetStatus(true, "Captured generation " +
                          std::to_string(Snapshot.Generation()) + " with " +
                          std::to_string(Snapshot.Size()) + " symbols.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("ImGui demo", &ShowImGuiDemo);
  }

  void DrawStatus() {
    const ImVec4 Color = StatusSucceeded ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f)
                                         : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
    ImGui::TextColored(Color, "%s", Status.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | generation %llu | %zu symbols",
                        Frozen ? "frozen" : "open to registration",
                        static_cast<unsigned long long>(Snapshot.Generation()),
                        Snapshot.Size());
  }

  void DrawScriptTab() {
    if (!ImGui::BeginTabItem("Script"))
      return;

    ImGui::TextUnformatted("Example:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.0f);
    const std::string Current(ExampleScripts[SelectedExample].Name);
    if (ImGui::BeginCombo("##Examples", Current.c_str())) {
      for (std::size_t Index = 0; Index < ExampleScripts.size(); ++Index) {
        const std::string Name(ExampleScripts[Index].Name);
        if (ImGui::Selectable(Name.c_str(), Index == SelectedExample))
          LoadExample(Index);
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d line(s) | Run executes this buffer in the State",
                        Editor.GetTotalLines());

    Editor.Render("Luau source", ImVec2(0.0f, 0.0f), true);
    ImGui::EndTabItem();
  }

  void DrawOutputTab() {
    if (!ImGui::BeginTabItem("Host output"))
      return;

    if (ImGui::Button("Clear"))
      OutputLines.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu line(s) from the HostLog binding",
                        OutputLines.size());

    ImGui::BeginChild("OutputLines", ImVec2(0.0f, 0.0f), true);
    if (OutputLines.empty())
      ImGui::TextDisabled("Run a script; every HostLog call lands here.");
    for (const std::string &Line : OutputLines)
      ImGui::TextWrapped("%s", Line.c_str());
    ImGui::EndChild();
    ImGui::EndTabItem();
  }

  // One captured generation, walked in the canonical order it publishes.
  void DrawReflectionTab() {
    if (!ImGui::BeginTabItem("Reflection"))
      return;

    ImGui::Text("Generation %llu: %zu symbols, %zu canonical types, %zu "
                "loaded modules",
                static_cast<unsigned long long>(Snapshot.Generation()),
                Snapshot.Size(), Snapshot.Types().Size(),
                Snapshot.Modules().Size());

    SymbolFilter.Draw("Filter (name or kind)", 320.0f);
    ImGui::SameLine();
    if (ImGui::Button("Clear filter"))
      SymbolFilter.Clear();

    constexpr ImGuiTableFlags TableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("Symbols", 5, TableFlags, ImVec2(0.0f, 0.0f))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("qualified name");
      ImGui::TableSetupColumn("kind");
      ImGui::TableSetupColumn("signature");
      ImGui::TableSetupColumn("documentation");
      ImGui::TableSetupColumn("module");
      ImGui::TableHeadersRow();

      const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
      for (std::size_t Index = 0; Index < Symbols.Size(); ++Index) {
        const Luna::ReflectionRecord Record = Symbols.At(Index);
        const std::string Qualified(Record.QualifiedName());
        const std::string Kind(Luna::SymbolKindText(Record.Kind()));
        if (!SymbolFilter.PassFilter(Qualified.c_str()) &&
            !SymbolFilter.PassFilter(Kind.c_str()))
          continue;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        TextView(Qualified);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Kind.c_str());
        ImGui::TableNextColumn();
        TextView(Record.Signature());
        ImGui::TableNextColumn();
        TextView(Record.Documentation());
        ImGui::TableNextColumn();
        if (!Record.HasModule()) {
          ImGui::TextDisabled("-");
          continue;
        }
        const Luna::ModuleRecord Module = Record.Module();
        ImGui::Text("%s@%s", std::string(Module.Identity()).c_str(),
                    std::string(Module.Version()).c_str());
      }
      ImGui::EndTable();
    }
    ImGui::EndTabItem();
  }

  void DrawArtifactTab() {
    if (!ImGui::BeginTabItem("Generated artifacts"))
      return;

    if (ImGui::Button("Generate both"))
      GenerateArtifacts();
    ImGui::SameLine();
    ImGui::TextDisabled("Both generators read the captured snapshot only.");

    ImGui::Checkbox("identities", &WithIdentities);
    ImGui::SameLine();
    ImGui::Checkbox("attributes", &WithAttributes);
    ImGui::SameLine();
    ImGui::Checkbox("examples", &WithExamples);
    ImGui::SameLine();
    ImGui::TextDisabled("| documentation options");

    ImGui::Checkbox("strict mode", &WithStrictMode);
    ImGui::SameLine();
    ImGui::Checkbox("provenance", &WithProvenance);
    ImGui::SameLine();
    ImGui::Checkbox("doc comments", &WithDeclarationDocumentation);
    ImGui::SameLine();
    ImGui::TextDisabled("| declaration options");

    if (ImGui::BeginTabBar("Artifacts")) {
      if (ImGui::BeginTabItem("documentation.md")) {
        DrawArtifactBody("DocumentationBytes", DocumentationText,
                         DocumentationComplete, DocumentationStatus);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("declarations.d.lua")) {
        DrawArtifactBody("DeclarationBytes", DeclarationText,
                         DeclarationComplete, DeclarationStatus);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::EndTabItem();
  }

  static void DrawArtifactBody(const char *Identifier, const std::string &Text,
                               bool Complete, const std::string &Status) {
    if (Text.empty()) {
      ImGui::TextDisabled("Press 'Generate both'.");
      return;
    }
    if (!Complete) {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f),
                         "Generation refused (%s)", Status.c_str());
    }
    ImGui::BeginChild(Identifier, ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(Text.c_str());
    ImGui::EndChild();
  }

  // The point of the whole window: the exact C++ each feature was bound with.
  void DrawSnippetTab() {
    if (!ImGui::BeginTabItem("How this was bound"))
      return;

    ImGui::TextDisabled("Every snippet below is the public API, and nothing "
                        "else: the demo links Luna::Luna alone.");
    ImGui::BeginChild("Snippets", ImVec2(0.0f, 0.0f), true);
    for (const BoundFeature &Feature : BoundFeatures) {
      const std::string Name(Feature.Name);
      if (!ImGui::CollapsingHeader(Name.c_str()))
        continue;
      ImGui::Indent();
      TextView(Feature.Snippet);
      ImGui::Unindent();
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
  }

  void DrawDiagnosticTab() {
    if (!ImGui::BeginTabItem("Diagnostics"))
      return;

    const std::size_t Failures = FailureCount();
    ImGui::Text("%zu recorded result(s), %zu refused", Log.size(), Failures);

    constexpr ImGuiTableFlags TableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("Results", 3, TableFlags, ImVec2(0.0f, 0.0f))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("outcome", ImGuiTableColumnFlags_WidthFixed,
                              90.0f);
      ImGui::TableSetupColumn("call");
      ImGui::TableSetupColumn("detail");
      ImGui::TableHeadersRow();

      for (const LogEntry &Entry : Log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (Entry.Succeeded)
          ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "accepted");
        else
          ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "refused");
        ImGui::TableNextColumn();
        TextView(Entry.Origin);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", Entry.Detail.c_str());
      }
      ImGui::EndTable();
    }
    ImGui::EndTabItem();
  }

  Luna::State State;
  TextEditor Editor;
  Luna::ReflectionSnapshot Snapshot;
  ImGuiTextFilter SymbolFilter;
  std::vector<LogEntry> Log;
  std::vector<std::string> OutputLines;
  std::string Status;
  std::string DocumentationText;
  std::string DocumentationStatus;
  std::string DeclarationText;
  std::string DeclarationStatus;
  std::size_t SelectedExample = 0;
  std::size_t ProbeCount = 0;
  bool StatusSucceeded = false;
  bool DocumentationComplete = false;
  bool DeclarationComplete = false;
  bool WithIdentities = false;
  bool WithAttributes = true;
  bool WithExamples = true;
  bool WithStrictMode = true;
  bool WithProvenance = true;
  bool WithDeclarationDocumentation = true;
  bool Frozen = false;
  bool ShowImGuiDemo = false;
};

} // namespace

int main() {
  glfwSetErrorCallback(ReportGlfwError);
  if (!glfwInit())
    return EXIT_FAILURE;

#if defined(__APPLE__)
  const char *GlslVersion = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
  const char *GlslVersion = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow *Window =
      glfwCreateWindow(1440, 860, "Luna binding playground", nullptr, nullptr);
  if (!Window) {
    glfwTerminate();
    return EXIT_FAILURE;
  }

  glfwMakeContextCurrent(Window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(Window, true);
  ImGui_ImplOpenGL3_Init(GlslVersion);

  Playground Demo;
  while (!glfwWindowShouldClose(Window)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    Demo.Draw();

    ImGui::Render();
    int Width = 0;
    int Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);
    glViewport(0, 0, Width, Height);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(Window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(Window);
  glfwTerminate();
  return EXIT_SUCCESS;
}
