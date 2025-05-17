// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Window.h"

#include <bit>
#include <chrono>
#include <windowsx.h>

#include "../Core.h"
#include "../common/Asserts.h"
#include "../common/Log.h"
#include "../renderer/Camera.h"

namespace
{
using namespace std::chrono;

time_point<steady_clock> g_LastTime = high_resolution_clock::now();
i32 g_FrameCount = 0;
f32 g_Fps = 0.0f;

LRESULT CALLBACK loc_WndProc(const HWND hWnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    const Window* window = std::bit_cast<const Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (!window)
    {
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    Camera& camera = Camera::GetInstance();

    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_KEYDOWN:
        camera.OnKeyDown(wParam);
        break;

    case WM_KEYUP:
        camera.OnKeyUp(wParam);
        break;

    case WM_MOUSEMOVE:
        camera.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
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
        Core::OnUpdate();
        Core::OnRender();
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    const time_point<steady_clock> currentTime = high_resolution_clock::now();
    const duration<f32> elapsed = currentTime - g_LastTime;
    g_FrameCount++;
    if (elapsed.count() >= 1.0f)
    {
        g_Fps = static_cast<f32>(g_FrameCount) / elapsed.count();
        g_FrameCount = 0;
        g_LastTime = currentTime;
        IE_Log("FPS : {}", g_Fps);
    }

    return 0;
}
} // namespace

void Window::Run(const RunInfo& runInfo)
{
    m_Fullscreen = runInfo.fullscreen;
    m_Name = runInfo.name.Data();
    m_Title = runInfo.title.Data();
    m_Resolution = runInfo.resolution;
    m_Hinstance = runInfo.hInstance;

    if (m_Fullscreen)
    {
        const HMONITOR hmonitor = MonitorFromWindow(m_Hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = {.cbSize = sizeof(monitorInfo), .rcMonitor = {0, 0, 0, 0}, .rcWork = {0, 0, 0, 0}, .dwFlags = 0};
        GetMonitorInfo(hmonitor, &monitorInfo);

        m_Resolution.x = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
        m_Resolution.y = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    }

    m_AspectRatio = static_cast<f32>(m_Resolution.x) / static_cast<f32>(m_Resolution.y);

    const WNDCLASSEX windowClass = {
        .cbSize = sizeof(WNDCLASSEX),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = loc_WndProc,
        .cbClsExtra = NULL,
        .cbWndExtra = NULL,
        .hInstance = m_Hinstance,
        .hIcon = LoadIcon(nullptr, IDI_APPLICATION),
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .hbrBackground = nullptr,
        .lpszMenuName = nullptr,
        .lpszClassName = m_Name,
        .hIconSm = LoadIcon(nullptr, IDI_APPLICATION),
    };
    IE_Assert(RegisterClassExW(&windowClass));

    SetProcessDPIAware();

    m_Hwnd = CreateWindowEx(NULL, m_Name, m_Title, WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<i32>(m_Resolution.x), static_cast<i32>(m_Resolution.y),
                            nullptr, nullptr, m_Hinstance, nullptr);
    IE_Assert(m_Hwnd);

    SetWindowLongPtr(m_Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (m_Fullscreen)
    {
        SetWindowLong(m_Hwnd, GWL_STYLE, 0);
    }

    Core::OnInit();

    ShowWindow(m_Hwnd, runInfo.nShowCmd);

    const Camera& camera = Camera::GetInstance();
    static MSG msg;
    ZeroMemory(&msg, sizeof(MSG));
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            camera.HandleShowCursor();
        }
    }
}

void Window::Terminate() const
{
    CloseWindow(m_Hwnd);
}

const HINSTANCE& Window::GetHinstance() const
{
    return m_Hinstance;
}

const HWND& Window::GetHwnd() const
{
    return m_Hwnd;
}

const uint2& Window::GetResolution() const
{
    return m_Resolution;
}

f32 Window::GetAspectRatio() const
{
    return m_AspectRatio;
}

bool Window::IsFullscreen() const
{
    return m_Fullscreen;
}
