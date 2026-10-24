// Win32 + Direct3D 11 shell for the luna demo.
//
// This file is deliberately boring: a window, a swap chain, and a frame loop.
// Everything the demo demonstrates lives in harness.cpp, behind the library's
// public API.

#include "harness.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <iterator>

// The backend header leaves this declaration inside an `#if 0` so that it does
// not force <windows.h> on every consumer. Copying it here is the documented
// way to wire up the message handler.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND Window,
                                                             UINT Message,
                                                             WPARAM WParam,
                                                             LPARAM LParam);

namespace {

ID3D11Device *Device = nullptr;
ID3D11DeviceContext *Context = nullptr;
IDXGISwapChain *SwapChain = nullptr;
ID3D11RenderTargetView *RenderTarget = nullptr;

bool SwapChainOccluded = false;
UINT ResizeWidth = 0;
UINT ResizeHeight = 0;

void CreateRenderTarget() {
  ID3D11Texture2D *BackBuffer = nullptr;
  SwapChain->GetBuffer(0, IID_PPV_ARGS(&BackBuffer));
  if (!BackBuffer)
    return;
  Device->CreateRenderTargetView(BackBuffer, nullptr, &RenderTarget);
  BackBuffer->Release();
}

void ReleaseRenderTarget() {
  if (RenderTarget) {
    RenderTarget->Release();
    RenderTarget = nullptr;
  }
}

bool CreateDevice(HWND Window) {
  DXGI_SWAP_CHAIN_DESC Description = {};
  Description.BufferCount = 2;
  Description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  Description.BufferDesc.RefreshRate.Numerator = 60;
  Description.BufferDesc.RefreshRate.Denominator = 1;
  Description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  Description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  Description.OutputWindow = Window;
  Description.SampleDesc.Count = 1;
  Description.Windowed = TRUE;
  Description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL Requested[] = {D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL Obtained = D3D_FEATURE_LEVEL_11_0;

  HRESULT Result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, Requested,
      static_cast<UINT>(std::size(Requested)), D3D11_SDK_VERSION, &Description,
      &SwapChain, &Device, &Obtained, &Context);

  // A machine with no usable GPU driver still runs the demo, just slowly.
  if (FAILED(Result)) {
    Result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, Requested,
        static_cast<UINT>(std::size(Requested)), D3D11_SDK_VERSION,
        &Description, &SwapChain, &Device, &Obtained, &Context);
  }

  if (FAILED(Result))
    return false;

  CreateRenderTarget();
  return RenderTarget != nullptr;
}

void DestroyDevice() {
  ReleaseRenderTarget();
  if (SwapChain) {
    SwapChain->Release();
    SwapChain = nullptr;
  }
  if (Context) {
    Context->Release();
    Context = nullptr;
  }
  if (Device) {
    Device->Release();
    Device = nullptr;
  }
}

LRESULT WINAPI WindowProc(HWND Window, UINT Message, WPARAM WParam,
                          LPARAM LParam) {
  // The backend needs first look, or it loses track of mouse capture and keys.
  if (ImGui_ImplWin32_WndProcHandler(Window, Message, WParam, LParam))
    return 1;

  switch (Message) {
  case WM_SIZE:
    if (WParam == SIZE_MINIMIZED)
      return 0;
    SwapChainOccluded = false;
    // Deferred: the swap chain cannot be resized while a frame is in flight.
    ResizeWidth = static_cast<UINT>(LOWORD(LParam));
    ResizeHeight = static_cast<UINT>(HIWORD(LParam));
    return 0;

  case WM_SYSCOMMAND:
    if ((WParam & 0xfff0) == SC_KEYMENU)
      return 0;
    break;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(Window, Message, WParam, LParam);
}

void ApplyStyle() {
  ImGui::StyleColorsDark();

  ImGuiStyle &Style = ImGui::GetStyle();
  Style.WindowRounding = 5.0f;
  Style.ChildRounding = 4.0f;
  Style.FrameRounding = 3.0f;
  Style.GrabRounding = 3.0f;
  Style.TabRounding = 3.0f;
  Style.ScrollbarRounding = 6.0f;
  Style.WindowBorderSize = 0.0f;
  Style.FramePadding = ImVec2(7.0f, 4.0f);
  Style.ItemSpacing = ImVec2(7.0f, 6.0f);

  ImVec4 *Colors = Style.Colors;
  Colors[ImGuiCol_WindowBg] = ImVec4(0.082f, 0.090f, 0.110f, 1.00f);
  Colors[ImGuiCol_ChildBg] = ImVec4(0.067f, 0.074f, 0.090f, 1.00f);
  Colors[ImGuiCol_PopupBg] = ImVec4(0.098f, 0.106f, 0.129f, 1.00f);
  Colors[ImGuiCol_Border] = ImVec4(0.157f, 0.173f, 0.208f, 1.00f);
  Colors[ImGuiCol_FrameBg] = ImVec4(0.137f, 0.149f, 0.180f, 1.00f);
  Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.176f, 0.192f, 0.231f, 1.00f);
  Colors[ImGuiCol_FrameBgActive] = ImVec4(0.208f, 0.227f, 0.275f, 1.00f);
  Colors[ImGuiCol_TitleBg] = ImVec4(0.082f, 0.090f, 0.110f, 1.00f);
  Colors[ImGuiCol_TitleBgActive] = ImVec4(0.110f, 0.120f, 0.149f, 1.00f);
  Colors[ImGuiCol_MenuBarBg] = ImVec4(0.098f, 0.106f, 0.129f, 1.00f);
  Colors[ImGuiCol_Button] = ImVec4(0.173f, 0.204f, 0.271f, 1.00f);
  Colors[ImGuiCol_ButtonHovered] = ImVec4(0.239f, 0.302f, 0.404f, 1.00f);
  Colors[ImGuiCol_ButtonActive] = ImVec4(0.290f, 0.376f, 0.502f, 1.00f);
  Colors[ImGuiCol_Header] = ImVec4(0.157f, 0.184f, 0.243f, 1.00f);
  Colors[ImGuiCol_HeaderHovered] = ImVec4(0.208f, 0.251f, 0.333f, 1.00f);
  Colors[ImGuiCol_HeaderActive] = ImVec4(0.247f, 0.302f, 0.400f, 1.00f);
  Colors[ImGuiCol_Tab] = ImVec4(0.110f, 0.120f, 0.149f, 1.00f);
  Colors[ImGuiCol_TabHovered] = ImVec4(0.208f, 0.251f, 0.333f, 1.00f);
  Colors[ImGuiCol_TabSelected] = ImVec4(0.173f, 0.204f, 0.271f, 1.00f);
  Colors[ImGuiCol_Separator] = ImVec4(0.157f, 0.173f, 0.208f, 1.00f);
  Colors[ImGuiCol_CheckMark] = ImVec4(0.400f, 0.760f, 0.647f, 1.00f);
  Colors[ImGuiCol_SliderGrab] = ImVec4(0.353f, 0.616f, 0.851f, 1.00f);
  Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.451f, 0.702f, 0.925f, 1.00f);
  Colors[ImGuiCol_Text] = ImVec4(0.878f, 0.894f, 0.937f, 1.00f);
  Colors[ImGuiCol_TextDisabled] = ImVec4(0.478f, 0.510f, 0.580f, 1.00f);
}

} // namespace

int WINAPI wWinMain(HINSTANCE Instance, HINSTANCE, PWSTR, int) {
  ImGui_ImplWin32_EnableDpiAwareness();

  WNDCLASSEXW Class = {};
  Class.cbSize = sizeof(Class);
  Class.style = CS_CLASSDC;
  Class.lpfnWndProc = WindowProc;
  Class.hInstance = Instance;
  Class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  Class.lpszClassName = L"LunaDemoWindow";
  RegisterClassExW(&Class);

  HWND Window = CreateWindowW(Class.lpszClassName, L"luna demo",
                              WS_OVERLAPPEDWINDOW, 120, 80, 1500, 940, nullptr,
                              nullptr, Instance, nullptr);
  if (!Window) {
    std::fprintf(stderr, "CreateWindow failed\n");
    return 1;
  }

  if (!CreateDevice(Window)) {
    DestroyDevice();
    DestroyWindow(Window);
    UnregisterClassW(Class.lpszClassName, Instance);
    std::fprintf(stderr, "Direct3D 11 is unavailable\n");
    return 1;
  }

  ShowWindow(Window, SW_SHOWDEFAULT);
  UpdateWindow(Window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO &Input = ImGui::GetIO();
  Input.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // No ini file: the layout is fixed, and a stray imgui.ini next to the exe
  // would only confuse whoever copies it around.
  Input.IniFilename = nullptr;

  ApplyStyle();
  ImGui::GetStyle().FontScaleDpi = ImGui_ImplWin32_GetDpiScaleForHwnd(Window);

  ImGui_ImplWin32_Init(Window);
  ImGui_ImplDX11_Init(Device, Context);

  int ExitCode = 0;
  {
    demo::Harness Harness;

    bool Running = true;
    while (Running) {
      MSG Message;
      while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
        if (Message.message == WM_QUIT)
          Running = false;
      }
      if (!Running)
        break;

      // While minimised there is nothing to draw, so the loop idles instead of
      // spinning the GPU.
      if (SwapChainOccluded &&
          SwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
        Sleep(10);
        continue;
      }
      SwapChainOccluded = false;

      if (ResizeWidth != 0 && ResizeHeight != 0) {
        ReleaseRenderTarget();
        SwapChain->ResizeBuffers(0, ResizeWidth, ResizeHeight,
                                 DXGI_FORMAT_UNKNOWN, 0);
        ResizeWidth = 0;
        ResizeHeight = 0;
        CreateRenderTarget();
      }

      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();

      Harness.Draw();

      ImGui::Render();
      const ImVec4 Clear = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
      const float ClearColor[4] = {Clear.x, Clear.y, Clear.z, 1.0f};
      Context->OMSetRenderTargets(1, &RenderTarget, nullptr);
      Context->ClearRenderTargetView(RenderTarget, ClearColor);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

      if (SwapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED)
        SwapChainOccluded = true;

      if (Harness.WantsQuit())
        Running = false;
    }

    ExitCode = Harness.IsReady() ? 0 : 1;
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  DestroyDevice();
  DestroyWindow(Window);
  UnregisterClassW(Class.lpszClassName, Instance);
  return ExitCode;
}
