// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>
#include <chrono>

#include "common/Types.h"

class Core;

class Window
{
  public:
    struct RunInfo
    {
        XMUINT2 resolution;
        String name;
        String title;
        HINSTANCE hInstance;
        i32 nShowCmd;
    };

    void Run(Core& core, const RunInfo& runInfo);
    void Terminate() const;

    const HWND& GetHwnd() const;
    const XMUINT2& GetResolution() const;
    f32 GetAspectRatio() const;
    Core* GetCore() const;

    f32 GetFPS() const;
    f32 GetFrameTimeMs() const;

  private:
    Core* m_Core = nullptr;
    HINSTANCE m_Hinstance = nullptr;
    HWND m_Hwnd = nullptr;

    String m_Name = "undefined";
    String m_Title = "undefined";
    XMUINT2 m_Resolution = {1, 1};
    f32 m_AspectRatio = 1;
    std::chrono::steady_clock::time_point m_LastFrameTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_LastFpsTime = m_LastFrameTime;
    u32 m_FrameCount = 0;
    f32 m_FrameTimeMs = 0.0f;
    f32 m_Fps = 0.0f;
};

