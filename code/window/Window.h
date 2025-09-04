// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#define NOMINMAX
#include <Windows.h>

#include "common/Singleton.h"
#include "common/Types.h"

class Core;

class Window : public Singleton<Window>
{
  public:
    struct RunInfo
    {
        XMUINT2 resolution;
        WString name;
        WString title;
        bool fullscreen;
        HINSTANCE hInstance;
        i32 nShowCmd;
    };

    void Run(const RunInfo& runInfo);
    void Terminate() const;

    const HINSTANCE& GetHinstance() const;
    const HWND& GetHwnd() const;
    const XMUINT2& GetResolution() const;
    f32 GetAspectRatio() const;

    bool IsFullscreen() const;

    static f32 GetFPS();
    static f32 GetFrameTimeMs();

  private:
    HINSTANCE m_Hinstance = nullptr;
    HWND m_Hwnd = nullptr;

    LPCWSTR m_Name = L"undefined";
    LPCWSTR m_Title = L"undefined";
    XMUINT2 m_Resolution = {1, 1};
    f32 m_AspectRatio = 1;

    bool m_Fullscreen = false;
};
