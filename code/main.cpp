// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Core.h"
#include "common/Asserts.h"

#include "common/CommandLineArguments.h"
#include "common/Types.h"

i32 WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE, LPSTR, const i32 nShowCmd)
{
    IE_Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    ProcessCommandLineArguments(__argc, __argv);

    const WString windowTitle = GetWindowTitle();

    Window::GetInstance().Run({
        .resolution = {2560, 1440},
        .name = L"Iškur Engine",
        .title = windowTitle,
        .fullscreen = false,
        .hInstance = hInstance,
        .nShowCmd = nShowCmd,
    });

    CoUninitialize();

    return 0;
}