// clang-format off
#include <luna/luna.hpp>

#include <TextEditor.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

constexpr std::string_view DefaultSource = R"(local answer = HostAdd(19, 23)
HostLog("HostAdd returned " .. tostring(answer))
assert(answer == 42)
)";

void ReportGlfwError(int Code, const char *Message) {
  std::cerr << "GLFW error " << Code << ": "
            << (Message ? Message : "unknown error") << '\n';
}

class Playground final {
public:
  Playground() {
    Editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    Editor.SetText(std::string(DefaultSource));

    if (!State.IsReady()) {
      SetStatus(false, "Luna could not create a Luau state.");
      return;
    }

    const auto LogRegistration =
        State.Bindings().Register("HostLog", [this](std::string Message) {
          if (!Output.empty())
            Output += '\n';
          Output += Message;
        });
    if (!LogRegistration.IsSuccess()) {
      SetStatus(false, DiagnosticMessage(LogRegistration.Diagnostic()));
      return;
    }

    const auto AddRegistration = State.Bindings().Register(
        "HostAdd", [](int Left, int Right) { return Left + Right; });
    if (!AddRegistration.IsSuccess()) {
      SetStatus(false, DiagnosticMessage(AddRegistration.Diagnostic()));
      return;
    }

    SetStatus(true, "Ready. Edit the Luau source and press Run.");
  }

  void Draw() {
    const ImGuiViewport *Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->WorkPos);
    ImGui::SetNextWindowSize(Viewport->WorkSize);

    constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoDecoration |
                                             ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Luna playground", nullptr, WindowFlags);

    if (ImGui::Button("Run", ImVec2(90.0f, 0.0f)))
      RunSource();
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      Editor.SetText(std::string(DefaultSource));
      Output.clear();
      SetStatus(true, "Editor reset to the example script.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show ImGui demo", &ShowImGuiDemo);

    ImGui::Separator();
    const ImVec4 StatusColor = LastRunSucceeded
                                   ? ImVec4(0.45f, 0.85f, 0.55f, 1.0f)
                                   : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
    ImGui::TextColored(StatusColor, "%s", Status.c_str());

    const float OutputHeight = 110.0f;
    const float EditorHeight = ImGui::GetContentRegionAvail().y - OutputHeight;
    Editor.Render("Luau source", ImVec2(-1.0f, EditorHeight), true);
    ImGui::SeparatorText("Host output");
    ImGui::BeginChild("Host output", ImVec2(0.0f, 0.0f), true);
    if (Output.empty())
      ImGui::TextDisabled("HostLog output appears here.");
    else
      ImGui::TextWrapped("%s", Output.c_str());
    ImGui::EndChild();
    ImGui::End();

    if (ShowImGuiDemo)
      ImGui::ShowDemoWindow(&ShowImGuiDemo);
  }

private:
  static std::string
  DiagnosticMessage(const Luna::ErrorDiagnostic *Diagnostic) {
    return Diagnostic ? Diagnostic->Message()
                      : "Luna returned a failure without a diagnostic.";
  }

  void RunSource() {
    Output.clear();
    if (!State.IsReady()) {
      SetStatus(false, "The Luau state is not ready.");
      return;
    }

    const auto Result = State.Execute(Editor.GetText());
    if (!Result.IsSuccess()) {
      SetStatus(false, DiagnosticMessage(Result.Diagnostic()));
      return;
    }

    SetStatus(true, "Script finished successfully.");
  }

  void SetStatus(bool Succeeded, std::string Message) {
    LastRunSucceeded = Succeeded;
    Status = std::move(Message);
  }

  Luna::State State;
  TextEditor Editor;
  std::string Status;
  std::string Output;
  bool LastRunSucceeded = false;
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
      glfwCreateWindow(1280, 720, "Luna Luau playground", nullptr, nullptr);
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
