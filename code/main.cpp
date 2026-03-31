// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Core.h"

#include "common/CommandLineArguments.h"
#include "renderer/Streamline.h"

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

i32 WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE, LPSTR, const i32 nShowCmd)
{
    IE_Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    ProcessCommandLineArguments(__argc, __argv);
    Streamline::Init();

    Window::RunInfo runInfo;
    runInfo.resolution = {2560, 1440}; // 3840, 2160
    runInfo.name = "Iskur Engine";
    runInfo.title = GetWindowTitle();
    runInfo.hInstance = hInstance;
    runInfo.nShowCmd = nShowCmd;

    Core core;
    core.Run(runInfo);

    Streamline::Shutdown();

    CoUninitialize();

    return 0;
}
