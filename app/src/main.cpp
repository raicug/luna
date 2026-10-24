// Smoke consumer: the README surface, plus the three things luna exists for.
//
//  1. A binding that suspends, so task.wait does not sleep a thread.
//  2. A table-shaped result whose entries carry instances.
//  3. A property write hook declared at the binding, not in a wrapper.

#include <luna/luna.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

class Sprite {
public:
  Sprite() = default;
  Sprite(double Width, double Height) : Width(Width), Height(Height) {}

  double Width = 1.0;
  double Height = 1.0;
  int Writes = 0;

  [[nodiscard]] double Area() const { return Width * Height; }
  [[nodiscard]] std::string ToText() const {
    return "Sprite(" + std::to_string(Width) + "x" + std::to_string(Height) +
           ")";
  }
  [[nodiscard]] Sprite Grow(double By) const {
    return Sprite(Width + By, Height + By);
  }
};

// The one line a consumer writes to make a class bindable.
template <> struct luna::detail::RegisteredClassTrait<Sprite> : std::true_type {};

enum class Channel : int { Debug = 10, Info = 20, Warning = 30 };

namespace {

double Loudness = 0.0;

int Failures = 0;

void Check(bool Condition, const char *What) {
  if (Condition) {
    std::printf("  ok    %s\n", What);
    return;
  }
  std::printf("  FAIL  %s\n", What);
  ++Failures;
}

} // namespace

int main() {
  luna::State State;
  if (!State.IsReady()) {
    std::printf("state creation failed\n");
    return 1;
  }

  luna::BindingRegistry Registry = State.Bindings();

  // --- a plain function, two arguments, one return
  if (!Registry
           .Register("Add", [](double A, double B) -> double { return A + B; })
           .IsSuccess()) {
    std::printf("Add failed to register\n");
    return 1;
  }

  // --- variadic, using the argument pack
  Registry.RegisterFunction("print", [](luna::ArgumentPack Args) {
    for (std::size_t Index = 0; Index < Args.Size(); ++Index) {
      const luna::Value Value = Args.View(Index);
      switch (Value.Category()) {
      case luna::ValueCategory::String:
        std::printf("%.*s", static_cast<int>(Value.AsString().size()),
                    Value.AsString().data());
        break;
      case luna::ValueCategory::Number:
        std::printf("%g", Value.AsNumber());
        break;
      case luna::ValueCategory::Boolean:
        std::printf(Value.AsBoolean() ? "true" : "false");
        break;
      case luna::ValueCategory::Table:
        std::printf("table");
        break;
      case luna::ValueCategory::Function:
        std::printf("function");
        break;
      case luna::ValueCategory::Userdata:
        std::printf("userdata");
        break;
      default:
        std::printf("nil");
        break;
      }
      std::printf(Index + 1 < Args.Size() ? " " : "\n");
    }
  });

  // --- a namespace, a constant, an enumeration
  luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Studio.RegisterConstant("Version", "0.1.0");

  luna::EnumBuilder<Channel> Channels =
      Studio.RegisterEnum<Channel>("Channel", luna::StableTypeKey("app.Channel"));
  Channels.Value("Debug", Channel::Debug)
      .Value("Info", Channel::Info)
      .Value("Warning", Channel::Warning)
      .Alias("Default", "Info");
  if (!Channels.Commit().IsSuccess())
    std::printf("Channel failed to register\n");

  // --- a class: factories, method, property with a native write hook, operators
  luna::ClassBuilder<Sprite> Sprites =
      Studio.RegisterClass<Sprite>("Sprite", luna::StableTypeKey("app.Sprite"));

  Sprites.Factory("new", [](double Width, double Height) {
           return Sprite(Width, Height);
         })
      .Method("Area", &Sprite::Area)
      .Method("Grow", &Sprite::Grow)
      .Property("Width", [](const Sprite &Self) { return Self.Width; },
                [](Sprite &Self, double Value) { Self.Width = Value; },
                luna::PropertyPolicy::Notify(
                    [](Sprite &Self, std::string_view) { ++Self.Writes; }))
      .Property("Height", [](const Sprite &Self) { return Self.Height; })
      .Operator(luna::ClassOperator::ToText, &Sprite::ToText)
      .Operator(luna::ClassOperator::Add,
                [](const Sprite &A, const Sprite &B) {
                  return Sprite(A.Width + B.Width, A.Height + B.Height);
                });

  const luna::RegistrationResult Committed = Sprites.Commit();
  if (!Committed.IsSuccess()) {
    std::printf("Sprite failed: %s\n",
                Committed.Diagnostic() ? Committed.Diagnostic()->Message().c_str()
                                       : "?");
    return 1;
  }

  // --- a binding that suspends instead of sleeping
  Registry.RegisterFunction("wait", [](double Seconds) {
    Loudness += Seconds;
    luna::Suspend(Seconds);
  });

  // --- a table result carrying instances
  Registry.RegisterFunction("makeSprites", [](double Count) {
    luna::OwnedValue List = luna::OwnedValue::Table();
    for (int Index = 0; Index < static_cast<int>(Count); ++Index) {
      List.Append(luna::OwnedValue::Instance<Sprite>(
          Sprite(static_cast<double>(Index), 1.0)));
    }
    return List;
  });

  // -------------------------------------------------------------------------
  // Behaviour checks
  // -------------------------------------------------------------------------

  std::printf("luna smoke\n");

  luna::ExecutionResult Result = State.Execute(R"(
    print("add", Add(2, 3))
    print("channel", Studio.Channel.Info, Studio.Channel.Default)
    print("version", Studio.Version)

    local S = Studio.Sprite.new(3, 4)
    print("area", S.Area, S.Width, S.Height)
    print("text", tostring(S))
    print("typeof", typeof(S))

    S.Width = 7
    print("hooked", S.Width)

    local T = S + Studio.Sprite.new(1, 2)
    print("sum", tostring(T))

    local list = makeSprites(3)
    print("list", #list, list[2].Width, typeof(list[2]))

    wait(0.5)
    print("resumed")
  )");

  if (!Result.IsSuccess()) {
    std::printf("execution failed: %s\n",
                Result.Diagnostic() ? Result.Diagnostic()->Message().c_str()
                                    : "?");
    ++Failures;
  }

  // --- argument diagnostics name the position and the symbol
  const luna::ExecutionResult Bad = State.Execute("Add('nope', 2)");
  std::printf("  error: %s\n",
              Bad.Diagnostic() ? Bad.Diagnostic()->Message().c_str() : "(none)");

  Check(!Bad.IsSuccess(), "a mistyped argument is refused");
  Check(Result.IsSuccess(), "the script runs");
  Check(Loudness >= 0.5, "wait() suspended and resumed with its delay");

  // --- the symbol palette the host reads for autocomplete
  const luna::SymbolRange Symbols = State.Symbols();
  std::printf("  symbols: %zu\n", Symbols.Size());

  bool SawSprite = false;
  bool SawArea = false;
  for (const luna::Symbol &Entry : Symbols) {
    if (Entry.Name() == "Sprite" && Entry.Kind() == luna::SymbolKind::Class)
      SawSprite = true;
    if (Entry.Name() == "Area" && Entry.Kind() == luna::SymbolKind::Method)
      SawArea = true;
  }
  Check(SawSprite, "the palette carries the class");
  Check(SawArea, "the palette carries the method");

  // --- a snapshot survives the state
  luna::ReflectionSnapshot Snapshot = State.Reflection();
  Check(Snapshot.IsValid(), "a reflection snapshot is produced");

  std::printf("\n%s (%d failure%s)\n", Failures == 0 ? "PASS" : "FAIL", Failures,
              Failures == 1 ? "" : "s");
  return Failures == 0 ? 0 : 1;
}
