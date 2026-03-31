// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Window.h"

#include <bit>
#include <chrono>
#include <imgui_impl_win32.h>

#include "Core.h"
#include "common/UtfConversion.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
using namespace std::chrono;

void RegisterRawMouseInput(HWND hwnd)
{
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Generic Desktop Controls
    rid.usUsage = 0x02;     // Mouse
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    IE_Assert(RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE);
}

void HandleRawMouseInput(const LPARAM lParam, Camera& camera)
{
    RAWINPUT raw{};
    UINT rawSize = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER)) == UINT(-1))
    {
        return;
    }

    if (raw.header.dwType != RIM_TYPEMOUSE)
    {
        return;
    }

    const LONG dx = raw.data.mouse.lLastX;
    const LONG dy = raw.data.mouse.lLastY;
    if (dx != 0 || dy != 0)
    {
        camera.OnRawMouseDelta(static_cast<i32>(dx), static_cast<i32>(dy));
    }
}

LRESULT CALLBACK loc_WndProc(const HWND hWnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    const Window* window = std::bit_cast<const Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (window && msg == WM_INPUT)
    {
        Camera& camera = window->GetCore()->GetRenderer().GetCamera();
        HandleRawMouseInput(lParam, camera);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    {
        return true;
    }

    window = std::bit_cast<const Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (!window)
    {
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    Camera& camera = window->GetCore()->GetRenderer().GetCamera();

    switch (msg)
    {
    case WM_CLOSE:
        window->Terminate();
        break;

    case WM_DESTROY:
        window->GetCore()->OnTerminate();
        PostQuitMessage(0);
        break;

    case WM_KEYDOWN:
        camera.OnKeyDown(wParam);
        break;

    case WM_KEYUP:
        camera.OnKeyUp(wParam);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            camera.OnLostFocus();
        }
        else
        {
            camera.OnGainedFocus();
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        break;
    }

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    return 0;
}
} // namespace

void Window::Run(Core& core, const RunInfo& runInfo)
{
    m_Core = &core;
    m_Name = runInfo.name;
    m_Title = runInfo.title;
    m_Resolution = runInfo.resolution;
    m_Hinstance = runInfo.hInstance;
    const std::wstring wideName = Utf8ToWide(m_Name);
    const std::wstring wideTitle = Utf8ToWide(m_Title);

    const WNDCLASSEXW windowClass = {
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = loc_WndProc,
        .cbClsExtra = NULL,
        .cbWndExtra = NULL,
        .hInstance = m_Hinstance,
        .hIcon = LoadIcon(nullptr, IDI_APPLICATION),
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .hbrBackground = nullptr,
        .lpszMenuName = nullptr,
        .lpszClassName = wideName.c_str(),
        .hIconSm = LoadIcon(nullptr, IDI_APPLICATION),
    };
    IE_Assert(RegisterClassExW(&windowClass));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    constexpr DWORD windowStyle = WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU;
    RECT windowRect = {0, 0, static_cast<LONG>(m_Resolution.x), static_cast<LONG>(m_Resolution.y)};
    IE_Assert(AdjustWindowRectExForDpi(&windowRect, windowStyle, FALSE, 0, GetDpiForSystem()) == TRUE);

    m_Hwnd = CreateWindowExW(0, wideName.c_str(), wideTitle.c_str(), windowStyle, CW_USEDEFAULT, CW_USEDEFAULT, windowRect.right - windowRect.left,
                             windowRect.bottom - windowRect.top, nullptr, nullptr, m_Hinstance, nullptr);
    IE_Assert(m_Hwnd);

    SetWindowLongPtr(m_Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    RegisterRawMouseInput(m_Hwnd);

    m_AspectRatio = static_cast<f32>(m_Resolution.x) / static_cast<f32>(m_Resolution.y);

    ShowWindow(m_Hwnd, runInfo.nShowCmd);
    UpdateWindow(m_Hwnd);

    m_Core->OnInit();
    m_LastFrameTime = steady_clock::now();
    m_LastFpsTime = m_LastFrameTime;
    m_FrameCount = 0;
    m_FrameTimeMs = 0.0f;
    m_Fps = 0.0f;

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            m_Core->GetRenderer().GetCamera().HandleShowCursor();
        }
        else
        {
            if (IsIconic(m_Hwnd))
            {
                m_LastFrameTime = steady_clock::now();
                continue;
            }

            m_Core->OnUpdate();
            m_Core->OnRender();

            const steady_clock::time_point now = steady_clock::now();
            m_FrameTimeMs = std::chrono::duration<f32, std::milli>(now - m_LastFrameTime).count();
            m_LastFrameTime = now;

            m_FrameCount++;
            const f32 elapsedSec = std::chrono::duration<f32>(now - m_LastFpsTime).count();
            if (elapsedSec >= 1.0f)
            {
                m_Fps = static_cast<f32>(m_FrameCount) / elapsedSec;
                m_FrameCount = 0;
                m_LastFpsTime = now;
            }
        }
    }
}

void Window::Terminate() const
{
    if (m_Hwnd)
    {
        DestroyWindow(m_Hwnd);
    }
}

const HWND& Window::GetHwnd() const
{
    return m_Hwnd;
}

const XMUINT2& Window::GetResolution() const
{
    return m_Resolution;
}

f32 Window::GetAspectRatio() const
{
    return m_AspectRatio;
}

Core* Window::GetCore() const
{
    return m_Core;
}

f32 Window::GetFPS() const
{
    return m_Fps;
}

f32 Window::GetFrameTimeMs() const
{
    return m_FrameTimeMs;
}

