#include "harness.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

std::string Shape::ToText() const {
  char Buffer[96];
  std::snprintf(Buffer, sizeof Buffer, "Shape(%s %.0fx%.0f at %.0f,%.0f)",
                Name.c_str(), Size, Size, X, Y);
  return Buffer;
}

void Shape::Move(float Dx, float Dy) {
  X += Dx;
  Y += Dy;
}

Shape *Scene::Get(int Index) const {
  if (Index < 1 || Index > static_cast<int>(Shapes.size()))
    return nullptr;
  return const_cast<Shape *>(&Shapes[static_cast<std::size_t>(Index - 1)]);
}

Shape *Scene::Add(std::string Name, float X, float Y, float Size, float Hue) {
  Shapes.push_back(Shape{std::move(Name), X, Y, Size, Hue});
  return &Shapes.back();
}

namespace demo {
namespace {
struct Sample {
  const char *Label;
  const char *Source;
  bool Runaway;
};

const Sample Samples[] = {
    {"Read the scene",
     R"(
-- Every value below comes out of a bound property getter.
print("shapes", scene.Count)

for Index, Shape in scene do
  -- Area is a method, so it is called; Size is a property, so it is not.
  print(Index, Shape.Name, Shape.Size, Shape:Area())
end

print("tostring", tostring(scene[1]))
print("typeof", typeof(scene[1]), typeof(scene))
)"},

    {"Move and resize",
     R"(
-- A write goes through the bound setter, which runs the host hook.
for Index, Shape in scene do
  Shape:Move(12, 6)
  Shape.Size = Shape.Size * 1.08
  Shape.Hue = (Shape.Hue + 0.11) % 1
end

-- Reading a property back sees the write, so the hook cannot lie.
print("scene[1].Size is now", scene[1].Size)
print("scene[1] moved to", scene[1].X, scene[1].Y)
print("the hook counted", scene[1].Writes, "writes")
)"},

    {"Wait and resume",
     R"(
-- task.wait asks the host to suspend, then returns nothing. The chunk stops
-- here and the Scheduler tab shows it as pending until the host ticks.
print("waiting for 0.3s of virtual time")
task.wait(0.3)
print("resumed")
)"},

    {"Fail on purpose",
     R"(
-- Passing a string where an index belongs is refused by the argument reader.
local Ok, Error = pcall(function()
  return scene:Get("not a number")
end)

print("refused:", Error)

-- A callable that throws becomes a Lua error, so pcall catches it too.
local Caught = pcall(function() fail("raised from C++") end)
print("caught:", Caught)

-- The state is still usable afterwards.
scene:Add("extra", 20, 40, 50, 0.9)
print("still running, shapes:", scene.Count)
)"},

    {"Runaway loop",
     R"(
-- This never yields, and an empty while-true loop carries no interrupt check at
-- all, so only single-stepping can stop it. The state runs with an instruction
-- budget, which is what halts this line.
print("entering the loop")
while true do end
)",
     true},
};

constexpr int SampleCount = static_cast<int>(std::size(Samples));

const char *KindName(luna::SymbolKind Kind) {
  switch (Kind) {
  case luna::SymbolKind::Function:
    return "fn";
  case luna::SymbolKind::Method:
    return "method";
  case luna::SymbolKind::StaticMethod:
    return "static";
  case luna::SymbolKind::Factory:
    return "factory";
  case luna::SymbolKind::Constructor:
    return "ctor";
  case luna::SymbolKind::Property:
    return "prop";
  case luna::SymbolKind::Field:
    return "field";
  case luna::SymbolKind::Class:
    return "class";
  case luna::SymbolKind::Enumeration:
    return "enum";
  case luna::SymbolKind::Enumerator:
  case luna::SymbolKind::EnumeratorAlias:
    return "item";
  case luna::SymbolKind::Constant:
    return "const";
  case luna::SymbolKind::Namespace:
    return "ns";
  case luna::SymbolKind::OverloadSet:
    return "overload";
  case luna::SymbolKind::None:
    break;
  }
  return "?";
}

template <class Result> std::string Describe(const Result &Value) {
  if (Value.Diagnostic() && !Value.Diagnostic()->Message().empty())
    return Value.Diagnostic()->Message();
  return Value.IsSuccess() ? std::string() : std::string("(no message)");
}

std::string FormatNumber(double Value) {
  char Buffer[40];
  if (std::isfinite(Value) && Value == std::floor(Value) &&
      std::fabs(Value) < 1e15) {
    std::snprintf(Buffer, sizeof Buffer, "%.0f", Value);
    return Buffer;
  }

  std::snprintf(Buffer, sizeof Buffer, "%.14g", Value);
  return Buffer;
}

ImVec4 LevelColor(LogLevel Level) {
  switch (Level) {
  case LogLevel::Script:
    return ImVec4(0.86f, 0.89f, 0.94f, 1.0f);
  case LogLevel::Warning:
    return ImVec4(1.00f, 0.80f, 0.36f, 1.0f);
  case LogLevel::Error:
    return ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
  case LogLevel::Host:
    break;
  }

  return ImVec4(0.52f, 0.72f, 0.95f, 1.0f);
}

void WriteNumber(luna::State &State, int Index, const char *Member,
                 double Value, std::string &Error) {
  char Buffer[192];
  std::snprintf(Buffer, sizeof Buffer,
                "local Target = scene[%d]\n"
                "if Target then Target.%s = %g end",
                Index, Member, Value);

  const luna::ExecutionResult Result = State.Execute(Buffer);
  if (!Result.IsSuccess())
    Error = Describe(Result);
}

void WriteBoolean(luna::State &State, int Index, const char *Member, bool Value,
                  std::string &Error) {
  char Buffer[192];
  std::snprintf(Buffer, sizeof Buffer,
                "local Target = scene[%d]\n"
                "if Target then Target.%s = %s end",
                Index, Member, Value ? "true" : "false");

  const luna::ExecutionResult Result = State.Execute(Buffer);
  if (!Result.IsSuccess())
    Error = Describe(Result);
}
} // namespace

Harness::Harness() {
  SelectSample(0);
  ReseedScene();
  if (!State.IsReady()) {
    Log(LogLevel::Error, "the VM failed to start");
    return;
  }

  Bind();
  RebuildSymbolTree();
  Log(LogLevel::Host,
      "luna demo ready: " + std::to_string(SymbolCount) + " symbols bound");
  Log(LogLevel::Host,
      "Run a sample, then drag a shape in the Scene tab: the UI writes through "
      "the same setter a script uses.");
}

void Harness::Bind() {
  luna::BindingRegistry Registry = State.Bindings();
  BindFunctions(Registry);
  BindShape(Registry);
  BindScene(Registry);
  const luna::RegistrationResult Published = Registry.RegisterValue(
      "scene", [this]() -> Scene * { return &SceneData; },
      luna::OwnershipPolicy::Borrowed(luna::LifetimeHandle()));

  if (!Published.IsSuccess())
    Log(LogLevel::Error, "scene failed to publish: " + Describe(Published));
}

void Harness::BindFunctions(luna::BindingRegistry &Registry) {
  Registry.RegisterFunction("print", [this](luna::ArgumentPack Args) {
    std::string Line;
    for (std::size_t Index = 0; Index < Args.Size(); ++Index) {
      if (Index > 0)
        Line += ' ';

      const luna::Value Value = Args.View(Index);
      switch (Value.Category()) {
      case luna::ValueCategory::Boolean:
        Line += Value.AsBoolean() ? "true" : "false";
        break;
      case luna::ValueCategory::Number:
        Line += FormatNumber(Value.AsNumber());
        break;
      case luna::ValueCategory::String:
        Line += Value.AsString();
        break;
      case luna::ValueCategory::Table:
        Line += "table";
        break;
      case luna::ValueCategory::Userdata:
        Line += "userdata";
        break;
      case luna::ValueCategory::Function:
        Line += "function";
        break;
      case luna::ValueCategory::Thread:
        Line += "thread";
        break;
      case luna::ValueCategory::Nil:
        Line += "nil";
        break;
      case luna::ValueCategory::None:
        break;
      }
    }
    Log(LogLevel::Script, std::move(Line));
  });
  Registry.RegisterFunction("warn", [this](std::string Text) {
    Log(LogLevel::Warning, std::move(Text));
  });
  Registry.RegisterFunction(
      "fail", [](std::string Text) { throw std::runtime_error(Text); });
  luna::NamespaceBuilder Task = Registry.RegisterNamespace("task");
  Task.RegisterFunction("wait", [](double Seconds) { luna::Suspend(Seconds); });
  Task.RegisterFunction("pending", [this]() { return State.PendingTasks(); });
  Task.Commit();
  luna::NamespaceBuilder Job = Registry.RegisterNamespace("job");
  Job.RegisterFunction("progress", [this](double Slot, double Value) {
    const auto Index = static_cast<std::size_t>(Slot);
    if (Index >= 1 && Index <= Progress.size())
      Progress[Index - 1] = std::clamp(Value, 0.0, 1.0);
  });
  Job.RegisterFunction("done", [this](double Slot) {
    const auto Index = static_cast<std::size_t>(Slot);
    if (Index >= 1 && Index <= Progress.size())
      Progress[Index - 1] = 1.0;
  });
  Job.Commit();
  luna::NamespaceBuilder App = Registry.RegisterNamespace("app");
  App.RegisterConstant("Version", "0.1.0");
  App.RegisterFunction("symbols",
                       [this]() { return static_cast<double>(SymbolCount); });
  App.Commit();
}

void Harness::BindShape(luna::BindingRegistry &Registry) {
  luna::ClassBuilder<Shape> Shapes =
      Registry.RegisterClass<Shape>("Shape", luna::StableTypeKey("demo.Shape"));

  Shapes
      .Factory("new",
               [](std::string Name, float X, float Y, float Size) {
                 return Shape{std::move(Name), X, Y, Size};
               })
      .Method("Area", &Shape::Area)
      .Method("Move", &Shape::Move)

      .Property(
          "Name",
          [](const Shape &Self) -> const std::string & { return Self.Name; })

      .Property(
          "X", [](const Shape &Self) { return Self.X; },
          [](Shape &Self, float Value) { Self.X = Value; },
          luna::PropertyPolicy::Notify(
              [](Shape &Self, std::string_view) { ++Self.Writes; }))

      .Property(
          "Y", [](const Shape &Self) { return Self.Y; },
          [](Shape &Self, float Value) { Self.Y = Value; },
          luna::PropertyPolicy::Notify(
              [](Shape &Self, std::string_view) { ++Self.Writes; }))

      .Property(
          "Size", [](const Shape &Self) { return Self.Size; },
          [](Shape &Self, float Value) {
            Self.Size = std::clamp(Value, 4.0f, 240.0f);
          },
          luna::PropertyPolicy::Notify(
              [](Shape &Self, std::string_view) { ++Self.Writes; }))

      .Property(
          "Hue", [](const Shape &Self) { return Self.Hue; },
          [](Shape &Self, float Value) {
            Self.Hue = Value - std::floor(Value);
          },
          luna::PropertyPolicy::Notify(
              [](Shape &Self, std::string_view) { ++Self.Writes; }))

      .Property(
          "Visible", [](const Shape &Self) { return Self.Visible; },
          [](Shape &Self, bool Value) { Self.Visible = Value; },
          luna::PropertyPolicy::Notify(
              [](Shape &Self, std::string_view) { ++Self.Writes; }))

      .Property("Writes", [](const Shape &Self) { return Self.Writes; })

      .Operator(luna::ClassOperator::ToText, &Shape::ToText);

  const luna::RegistrationResult Committed = Shapes.Commit();
  if (!Committed.IsSuccess())
    Log(LogLevel::Error, "Shape failed: " + Describe(Committed));
}

void Harness::BindScene(luna::BindingRegistry &Registry) {
  luna::ClassBuilder<Scene> Scenes =
      Registry.RegisterClass<Scene>("Scene", luna::StableTypeKey("demo.Scene"));

  Scenes.Property("Count", [](const Scene &Self) { return Self.Count(); })
      .Method("Get", &Scene::Get)
      .Method("Add", &Scene::Add)

      .Operator(luna::ClassOperator::Index, &Scene::Get)

      .Operator(
          luna::ClassOperator::Iterate,
          [](const Scene &Self, int Control) { return Self.Get(Control + 1); });

  const luna::RegistrationResult Committed = Scenes.Commit();
  if (!Committed.IsSuccess())
    Log(LogLevel::Error, "Scene failed: " + Describe(Committed));
}

void Harness::Log(LogLevel Level, std::string Text) {
  Entries.push_back(LogEntry{Level, std::move(Text)});
  while (Entries.size() > MaxEntries)
    Entries.pop_front();
}

void Harness::SelectSample(int Index) {
  SampleIndex = std::clamp(Index, 0, SampleCount - 1);
  Source = Samples[SampleIndex].Source;
}

void Harness::ReseedScene() {
  SceneData.Shapes.clear();
  SceneData.Add("panel", 16.0f, 14.0f, 120.0f, 0.55f);
  SceneData.Add("marker", 150.0f, 46.0f, 64.0f, 0.08f);
}

void Harness::RunSource() {
  if (!State.IsReady())
    return;

  const bool Runaway = Samples[SampleIndex].Runaway;
  if (Runaway)
    Log(LogLevel::Warning,
        "this sample never yields; the state's instruction budget halts it");

  Log(LogLevel::Host, "run: " + std::string(Samples[SampleIndex].Label));
  const auto Start = std::chrono::steady_clock::now();
  const luna::ExecutionResult Result = State.Execute(Source);
  const auto Stop = std::chrono::steady_clock::now();
  LastMilliseconds =
      std::chrono::duration<double, std::milli>(Stop - Start).count();
  LastFailed = !Result.IsSuccess();
  LastRunWasInterrupted = Result.IsInterrupted();
  if (Result.IsSuccess()) {
    LastStatus = "ok";
  } else if (Result.IsInterrupted()) {
    LastStatus = "interrupted";
    Log(LogLevel::Warning, "stopped: " + Describe(Result));
  } else {
    LastStatus = "error";
    Log(LogLevel::Error, Describe(Result));
  }

  if (State.IsInterruptPending())
    State.ClearInterrupt();
}

void Harness::SpawnWorkers() {
  ResetWorkers();
  if (!State.IsReady())
    return;

  for (int Slot = 1; Slot <= static_cast<int>(Tasks.size()); ++Slot) {
    char Buffer[256];
    std::snprintf(Buffer, sizeof Buffer,
                  "for Step = 1, 10 do\n"
                  "  task.wait(0.1)\n"
                  "  job.progress(%d, Step / 10)\n"
                  "end\n"
                  "job.done(%d)\n",
                  Slot, Slot);

    const std::uint64_t Handle = State.Spawn(Buffer);
    Tasks[static_cast<std::size_t>(Slot - 1)] = Handle;
  }

  Log(LogLevel::Host, "spawned " + std::to_string(Tasks.size()) +
                          " coroutines on one thread, " +
                          std::to_string(State.PendingTasks()) + " pending");
}

void Harness::ResetWorkers() {
  for (std::uint64_t &Handle : Tasks) {
    if (Handle != 0)
      State.Cancel(Handle);
    Handle = 0;
  }
  Progress.fill(0.0);
}

void Harness::CancelWorkers() {
  ResetWorkers();
  Log(LogLevel::Host,
      "cancelled; " + std::to_string(State.PendingTasks()) + " pending");
}

int Harness::RunSelfTest() {
  int Failures = 0;
  auto Expect = [&Failures](bool Condition, const char *What) {
    std::printf("  %s  %s\n", Condition ? "ok  " : "FAIL", What);
    if (!Condition)
      ++Failures;
  };
  if (!State.IsReady()) {
    std::printf("  FAIL  the VM did not start\n");
    return 1;
  }

  std::printf("demo self test\n");
  Expect(SymbolCount > 10, "the palette carries the binding");
  Expect(SceneData.Count() == 2, "the scene is seeded");
  std::fflush(stdout);
  for (int Index = 0; Index < SampleCount - 1; ++Index) {
    SelectSample(Index);
    RunSource();
    Expect(!LastFailed, Samples[Index].Label);
    if (LastFailed) {
      for (auto Entry = Entries.rbegin(); Entry != Entries.rend(); ++Entry) {
        if (Entry->Level == LogLevel::Error) {
          std::printf("       %s\n", Entry->Text.c_str());
          break;
        }
      }
    }
    std::fflush(stdout);
  }

  Expect(State.PendingTasks() == 0, "Execute drains its own suspensions");
  Expect(SceneData.Shapes[0].Writes > 0, "the write hook ran");
  Expect(SceneData.Count() == 3, "a shape was added from script");
  ReseedScene();
  SpawnWorkers();
  Expect(State.PendingTasks() == 8, "eight coroutines on one thread");
  for (int Step = 0; Step < 14; ++Step)
    State.Tick(0.2);

  Expect(State.PendingTasks() == 0, "every coroutine finished");
  Expect(Progress[0] >= 0.99, "a script reported its progress");
  SpawnWorkers();
  Expect(State.PendingTasks() == 8, "re-spawned");
  CancelWorkers();
  Expect(State.PendingTasks() == 0, "cancel drops every task");
  SelectSample(SampleCount - 1);
  RunSource();
  Expect(LastRunWasInterrupted, "a non-yielding loop was interrupted");
  SelectSample(0);
  RunSource();
  Expect(!LastFailed, "the state survived the interrupt");
  std::printf("\n%s (%d failure%s)\n", Failures == 0 ? "PASS" : "FAIL",
              Failures, Failures == 1 ? "" : "s");
  return Failures;
}

void Harness::Draw() {
  const auto Now = std::chrono::steady_clock::now();
  FrameSeconds = std::chrono::duration<float>(Now - LastFrame).count();
  LastFrame = Now;
  if (ImGui::IsKeyPressed(ImGuiKey_F5, false) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
    RunSource();
  }

  FrameHistory[static_cast<std::size_t>(FrameCount) % FrameHistory.size()] =
      FrameSeconds * 1000.0f;
  ++FrameCount;
  DrawToolbar();
  const ImGuiViewport *Viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(Viewport->WorkPos);
  ImGui::SetNextWindowSize(Viewport->WorkSize);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  const ImGuiStyle &Style = ImGui::GetStyle();
  const float ConsoleHeight = 200.0f;
  const ImVec2 Available = ImGui::GetContentRegionAvail();
  const float BodyHeight =
      std::max(200.0f, Available.y - ConsoleHeight - Style.ItemSpacing.y);

  ImGui::BeginChild("##left", ImVec2(Available.x * 0.46f, BodyHeight),
                    ImGuiChildFlags_Borders);
  DrawEditor();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##right", ImVec2(0, BodyHeight), ImGuiChildFlags_Borders);
  if (ImGui::BeginTabBar("##tabs")) {
    if (ImGui::BeginTabItem("Scene")) {
      DrawScenePanel();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Scheduler")) {
      DrawSchedulerPanel();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Halting")) {
      DrawInterruptPanel();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Symbols")) {
      DrawReflection();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
  ImGui::BeginChild("##console", ImVec2(0, 0), ImGuiChildFlags_Borders);
  DrawConsole();
  ImGui::EndChild();
  ImGui::End();
  if (AboutPending) {
    ImGui::OpenPopup("About luna");
    AboutPending = false;
  }
  DrawAboutPopup();
}

void Harness::DrawToolbar() {
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Quit", "Alt+F4"))
      QuitRequested = true;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Samples")) {
    for (int Index = 0; Index < SampleCount; ++Index) {
      if (ImGui::MenuItem(Samples[Index].Label, nullptr, Index == SampleIndex))
        SelectSample(Index);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Console auto-scroll", nullptr, &ScrollConsole);
    ImGui::MenuItem("Scheduler auto-tick", nullptr, &AutoTick);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("About"))
      AboutPending = true;
    ImGui::EndMenu();
  }

  char Status[192];
  std::snprintf(Status, sizeof Status,
                "%.2f ms/frame  |  %s  |  %.3f ms last run",
                FrameSeconds * 1000.0f, LastStatus.c_str(), LastMilliseconds);
  const float Width = ImGui::CalcTextSize(Status).x;
  ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - Width - 16.0f));
  ImGui::TextDisabled("%s", Status);
  ImGui::EndMainMenuBar();
}

void Harness::DrawEditor() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
  if (ImGui::Button("Run", ImVec2(80, 0)))
    RunSource();

  ImGui::SameLine();
  if (ImGui::Button("Halt", ImVec2(80, 0))) {
    State.RequestInterrupt(ReasonBuffer);
    Log(LogLevel::Warning, std::string("interrupt requested: ") + ReasonBuffer);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Arms an interrupt. It takes effect at the next call boundary, so a\n"
        "chunk that never yields is only stopped by the instruction budget.");

  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    Entries.clear();
    Log(LogLevel::Host, "console cleared");
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##sample", Samples[SampleIndex].Label)) {
    for (int Index = 0; Index < SampleCount; ++Index) {
      if (ImGui::Selectable(Samples[Index].Label, Index == SampleIndex))
        SelectSample(Index);
    }
    ImGui::EndCombo();
  }

  ImGui::PopStyleVar();
  ImGui::Separator();
  ImGui::InputTextMultiline("##source", &Source, ImVec2(-1, -1),
                            ImGuiInputTextFlags_AllowTabInput);

  if (LastFailed) {
    ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(LogLevel::Error));
    ImGui::TextWrapped("Last run failed (%s); see the console.",
                       LastStatus.c_str());
    ImGui::PopStyleColor();
  }
}

void Harness::DrawScenePanel() {
  ImGui::Separator();
  ImGui::TextDisabled("%d shapes, owned by the host and borrowed by the VM",
                      SceneData.Count());
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset scene"))
    ReseedScene();

  const ImVec2 Origin = ImGui::GetCursorScreenPos();
  const float Width = ImGui::GetContentRegionAvail().x;
  const float Height = 150.0f;
  ImGui::InvisibleButton("##canvas", ImVec2(Width, Height));
  ImDrawList *Draw = ImGui::GetWindowDrawList();
  Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + Height),
                      IM_COL32(18, 20, 26, 255), 4.0f);
  Draw->AddRect(Origin, ImVec2(Origin.x + Width, Origin.y + Height),
                IM_COL32(60, 66, 80, 255), 4.0f);

  for (const Shape &Item : SceneData.Shapes) {
    if (!Item.Visible)
      continue;

    const float Scale = 1.2f;
    const ImVec2 Min(Origin.x + Item.X * Scale + 10.0f,
                     Origin.y + Item.Y * Scale + 10.0f);
    const float Side = Item.Size * Scale * 0.5f;
    const ImVec2 Max(Min.x + Side, Min.y + Side);
    float Red = 0.0f;
    float Green = 0.0f;
    float Blue = 0.0f;
    ImGui::ColorConvertHSVtoRGB(Item.Hue, 0.62f, 0.95f, Red, Green, Blue);
    Draw->AddRectFilled(Min, Max,
                        IM_COL32(static_cast<int>(Red * 255.0f),
                                 static_cast<int>(Green * 255.0f),
                                 static_cast<int>(Blue * 255.0f), 210),
                        3.0f);
    Draw->AddRect(Min, Max, IM_COL32(0, 0, 0, 120), 3.0f);
    Draw->AddText(ImVec2(Min.x + 4.0f, Min.y + 3.0f), IM_COL32(10, 10, 12, 220),
                  Item.Name.c_str());
  }

  if (SceneData.Count() == 0) {
    Draw->AddText(ImVec2(Origin.x + 12.0f, Origin.y + 12.0f),
                  IM_COL32(120, 128, 144, 255),
                  "scene is empty - run a sample that adds a shape");
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Members");
  for (int Index = 0; Index < SceneData.Count(); ++Index) {
    Shape &Item = SceneData.Shapes[static_cast<std::size_t>(Index)];
    ImGui::PushID(Index);
    char Header[96];
    std::snprintf(Header, sizeof Header, "%s  ##%d", Item.Name.c_str(), Index);
    if (ImGui::CollapsingHeader(Header)) {
      ImGui::TextDisabled("%s", Item.ToText().c_str());
      std::string WriteError;
      // scene[i] is one-based, while the panel walks the deque from zero.
      const int Slot = Index + 1;
      float Value = Item.X;
      if (ImGui::SliderFloat("X", &Value, 0.0f, 260.0f, "%.0f"))
        WriteNumber(State, Slot, "X", Value, WriteError);

      Value = Item.Y;
      if (ImGui::SliderFloat("Y", &Value, 0.0f, 140.0f, "%.0f"))
        WriteNumber(State, Slot, "Y", Value, WriteError);

      Value = Item.Size;
      if (ImGui::SliderFloat("Size", &Value, 4.0f, 240.0f, "%.0f"))
        WriteNumber(State, Slot, "Size", Value, WriteError);

      Value = Item.Hue;
      if (ImGui::SliderFloat("Hue", &Value, 0.0f, 1.0f, "%.2f"))
        WriteNumber(State, Slot, "Hue", Value, WriteError);

      bool Shown = Item.Visible;
      if (ImGui::Checkbox("Visible", &Shown))
        WriteBoolean(State, Slot, "Visible", Shown, WriteError);

      ImGui::SameLine();
      ImGui::TextDisabled("writes %d   area %.0f", Item.Writes, Item.Area());
      if (!WriteError.empty())
        Log(LogLevel::Error, WriteError);
    }

    ImGui::PopID();
  }
}

void Harness::DrawSchedulerPanel() {
  ImGui::TextDisabled(
      "One thread, %u coroutines. A binding that calls Suspend yields instead "
      "of\nblocking, so the host advances them with Tick().",
      State.PendingTasks());

  ImGui::Separator();
  if (ImGui::Button("Spawn 8"))
    SpawnWorkers();
  ImGui::SameLine();
  if (ImGui::Button("Cancel all"))
    CancelWorkers();
  ImGui::SameLine();
  ImGui::Checkbox("Tick 0.1s per frame", &AutoTick);
  if (AutoTick) {
    State.Tick(0.1);
    VirtualTime += 0.1;
    ++TickCount;
  }

  ImGui::SeparatorText("Host side");
  ImGui::Text("virtual time  %.1f s", VirtualTime);
  ImGui::Text("tick calls    %llu", static_cast<unsigned long long>(TickCount));
  ImGui::Text("pending       %u", State.PendingTasks());
  const int Plotted =
      std::min(FrameCount, static_cast<int>(FrameHistory.size()));
  const int Offset = FrameCount % static_cast<int>(FrameHistory.size());
  ImGui::PlotLines("##frame", FrameHistory.data(), Plotted, Offset, "ms/frame",
                   0.0f, 33.0f, ImVec2(-1, 56));

  ImGui::SeparatorText("Per-script progress");
  for (std::size_t Index = 0; Index < Progress.size(); ++Index) {
    ImGui::PushID(static_cast<int>(Index));
    char Label[32];
    std::snprintf(Label, sizeof Label, "script %zu", Index + 1);
    ImGui::ProgressBar(static_cast<float>(Progress[Index]), ImVec2(-1, 0),
                       Label);
    ImGui::PopID();
  }

  if (ImGui::Button("Reset progress"))
    Progress.fill(0.0);
}

void Harness::DrawConsole() {
  ImGui::Checkbox("Auto-scroll", &ScrollConsole);
  ImGui::SameLine();
  ImGui::TextDisabled("%zu entries", Entries.size());
  ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);

  for (const LogEntry &Entry : Entries) {
    ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(Entry.Level));
    ImGui::TextUnformatted(Entry.Text.c_str());
    ImGui::PopStyleColor();
  }

  if (ScrollConsole)
    ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();
}

void Harness::DrawInterruptPanel() {
  ImGui::TextWrapped(
      "RequestInterrupt is safe from any thread and stops the running chunk at "
      "its next safepoint, a loop iteration or a call. This demo runs the "
      "script "
      "on the UI thread, so an interrupt has to be armed before the run; the "
      "state's instruction budget is what halts a chunk on its own.");

  ImGui::SeparatorText("Reason");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##reason", ReasonBuffer, IM_ARRAYSIZE(ReasonBuffer));
  if (ImGui::Button("Arm interrupt"))
    State.RequestInterrupt(ReasonBuffer);
  ImGui::SameLine();
  if (ImGui::Button("Clear"))
    State.ClearInterrupt();
  ImGui::SameLine();
  if (ImGui::Button("Arm and run the runaway")) {
    SelectSample(SampleCount - 1);
    State.RequestInterrupt(ReasonBuffer);
    RunSource();
  }

  ImGui::Separator();
  if (State.IsInterruptPending())
    ImGui::TextColored(LevelColor(LogLevel::Warning),
                       "pending: the next safepoint stops the chunk");
  else
    ImGui::TextDisabled("no interrupt pending");

  if (LastRunWasInterrupted)
    ImGui::TextColored(LevelColor(LogLevel::Warning),
                       "the last run was stopped by an interrupt");
  else
    ImGui::TextDisabled("the last run finished on its own");
}

void Harness::DrawReflection() {
  ImGui::TextDisabled(
      "%zu symbols. The palette is read from State::Symbols(), the same list a "
      "host\nfeeds its autocomplete.",
      SymbolCount);

  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##filter", "filter...", FilterBuffer,
                               IM_ARRAYSIZE(FilterBuffer)))
    FilterSymbols(FilterBuffer);

  ImGui::BeginChild("##symbols", ImVec2(0, 0));
  if (FilterBuffer[0] == '\0') {
    for (const auto &Entry : SymbolTree.Children)
      DrawNode(Entry.second, Entry.first.c_str());
  } else if (SymbolMatches.empty()) {
    ImGui::TextDisabled("nothing matches");
  } else {
    for (const luna::Symbol *Match : SymbolMatches) {
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::TextUnformatted(Match->QualifiedName().c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%s", KindName(Match->Kind()));
    }
  }

  ImGui::EndChild();
}

void Harness::DrawNode(const TreeNode &Node, const char *Label) {
  ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth;
  if (Node.Children.empty())
    Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

  const bool Open = ImGui::TreeNodeEx(Label, Flags);
  if (Node.HasLeaf) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", KindName(Node.LeafKind));
  }

  if (Open && !Node.Children.empty()) {
    for (const auto &Entry : Node.Children)
      DrawNode(Entry.second, Entry.first.c_str());
    ImGui::TreePop();
  }
}

void Harness::RebuildSymbolTree() {
  SymbolTree = TreeNode();
  SymbolCache.clear();
  SymbolMatches.clear();
  const luna::SymbolRange Symbols = State.Symbols();
  SymbolCache.reserve(Symbols.Size());
  for (const luna::Symbol &Entry : Symbols)
    SymbolCache.push_back(Entry);

  SymbolCount = SymbolCache.size();
  for (const luna::Symbol &Entry : SymbolCache) {
    TreeNode *Node = &SymbolTree;
    const std::string &Path = Entry.QualifiedName();
    std::size_t Start = 0;
    while (true) {
      const std::size_t Dot = Path.find('.', Start);
      const std::string Part = Path.substr(
          Start, Dot == std::string::npos ? std::string::npos : Dot - Start);

      if (Dot == std::string::npos) {
        Node->HasLeaf = true;
        Node->LeafKind = Entry.Kind();
        break;
      }

      Node = &Node->Children[Part];
      Node->HasNamespace = true;
      Start = Dot + 1;
    }
  }

  FilterSymbols(FilterBuffer);
}

void Harness::FilterSymbols(std::string_view Query) {
  SymbolMatches.clear();
  if (Query.empty())
    return;

  for (const luna::Symbol &Entry : SymbolCache) {
    if (Entry.QualifiedName().find(Query) != std::string::npos)
      SymbolMatches.push_back(&Entry);
  }
}

void Harness::DrawAboutPopup() {
  const ImVec2 Center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(Center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("About luna", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ImGui::Text("luna demonstration host");
  ImGui::Separator();
  ImGui::TextWrapped(
      "One Luau State and one ImGui frame loop. Nothing here reaches into the "
      "library's internals: the scene, the console, the scheduler and the "
      "palette all go through the public surface, so a regression in the "
      "binding shows up as a broken panel rather than a failed assert.");
  ImGui::Spacing();
  ImGui::Text("Dear ImGui %s", ImGui::GetVersion());
  ImGui::Spacing();
  if (ImGui::Button("Close", ImVec2(120, 0)))
    ImGui::CloseCurrentPopup();

  ImGui::EndPopup();
}
} // namespace demo
