#pragma once

// The demonstration host for luna: one window, one VM, one frame loop.
//
// The harness owns the State and the scene objects a script mutates. Everything
// it shows is read back out of the VM -- the script console, the property
// writes, the pending coroutines, the symbol palette -- so a change in the
// library has somewhere visible to break.

#include <luna/luna.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The bound types live at namespace scope: a trait specialisation cannot be
// declared inside an anonymous namespace.
struct Shape {
  std::string Name;
  float X = 0.0f;
  float Y = 0.0f;
  float Size = 80.0f;
  float Hue = 0.55f;
  bool Visible = true;
  int Writes = 0;

  [[nodiscard]] float Area() const { return Size * Size; }
  [[nodiscard]] std::string ToText() const;
  void Move(float Dx, float Dy);
};

// The container a script reaches as `scene`. Shapes live in a deque so a pointer
// handed to Luau stays valid across a push_back.
struct Scene {
  std::deque<Shape> Shapes;

  [[nodiscard]] int Count() const { return static_cast<int>(Shapes.size()); }
  [[nodiscard]] Shape *Get(int Index) const;
  Shape *Add(std::string Name, float X, float Y, float Size, float Hue);
};

template <> struct luna::detail::RegisteredClassTrait<Shape> : std::true_type {};
template <> struct luna::detail::RegisteredClassTrait<Scene> : std::true_type {};

namespace demo {

enum class LogLevel : std::uint8_t { Host, Script, Warning, Error };

struct LogEntry {
  LogLevel Level = LogLevel::Host;
  std::string Text;
};

class Harness {
public:
  Harness();

  [[nodiscard]] bool IsReady() const noexcept { return State.IsReady(); }

  // Builds one frame of UI. Every panel is drawn every frame, which keeps the
  // demo honest: nothing updates lazily in the background.
  void Draw();

  [[nodiscard]] bool WantsQuit() const noexcept { return QuitRequested; }

  // Exercises the bindings without a window, so the demo's behaviour can be
  // checked from a terminal or CI. Returns the number of failed expectations.
  int RunSelfTest();

private:
  // --- binding -----------------------------------------------------------
  void Bind();
  void BindFunctions(luna::BindingRegistry &Registry);
  void BindShape(luna::BindingRegistry &Registry);
  void BindScene(luna::BindingRegistry &Registry);

  // --- panels ------------------------------------------------------------
  void DrawToolbar();
  void DrawEditor();
  void DrawScenePanel();
  void DrawSchedulerPanel();
  void DrawConsole();
  void DrawReflection();
  void DrawInterruptPanel();
  void DrawAboutPopup();

  // --- actions -----------------------------------------------------------
  void RunSource();
  void SelectSample(int Index);
  void ReseedScene();
  void Log(LogLevel Level, std::string Text);
  void SpawnWorkers();
  void CancelWorkers();
  void ResetWorkers();

  // --- state the VM reaches through the bindings -------------------------
  // Declared before State so it outlives the VM: a borrowed userdata is freed
  // with the State, and it points into Shapes.
  Scene SceneData;

  luna::State State;

  // --- editor ------------------------------------------------------------
  std::string Source;
  int SampleIndex = 0;
  bool ScrollConsole = true;
  bool QuitRequested = false;
  bool AboutPending = false;

  // --- last run ----------------------------------------------------------
  bool LastFailed = false;
  std::string LastStatus = "never run";
  double LastMilliseconds = 0.0;

  // --- console -----------------------------------------------------------
  std::deque<LogEntry> Entries;
  std::size_t MaxEntries = 2000;

  // --- scheduler ---------------------------------------------------------
  std::array<double, 8> Progress{};
  std::array<std::uint64_t, 8> Tasks{};
  bool AutoTick = false;
  double VirtualTime = 0.0;
  std::uint64_t TickCount = 0;
  // A ring buffer, because ImGui's plot reads a contiguous array and a deque
  // does not promise one.
  std::array<float, 120> FrameHistory{};
  int FrameCount = 0;

  // --- interrupt ---------------------------------------------------------
  char ReasonBuffer[128] = "stopped by the host";
  bool LastRunWasInterrupted = false;

  // --- reflection --------------------------------------------------------
  struct TreeNode {
    std::map<std::string, TreeNode> Children;
    bool HasLeaf = false;
    bool HasNamespace = false;
    luna::SymbolKind LeafKind = luna::SymbolKind::None;
  };

  void RebuildSymbolTree();
  void DrawNode(const TreeNode &Node, const char *Label);
  void FilterSymbols(std::string_view Query);

  TreeNode SymbolTree;
  std::vector<const luna::Symbol *> SymbolMatches;
  std::vector<luna::Symbol> SymbolCache;
  char FilterBuffer[64] = "";
  std::size_t SymbolCount = 0;

  // --- clock -------------------------------------------------------------
  std::chrono::steady_clock::time_point LastFrame =
      std::chrono::steady_clock::now();
  float FrameSeconds = 0.0f;
};

} // namespace demo
