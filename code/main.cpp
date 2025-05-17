// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Core.h"

#include "common/CommandLineArguments.h"
#include "common/Types.h"

i32 WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE, LPSTR, const i32 nShowCmd)
{
    ProcessCommandLineArguments(__argc, __argv);

    Window::GetInstance().Run({
        .resolution = {2560, 1440},
        .name = L"Iškur Engine",
        .title = L"Iškur Engine",
        .fullscreen = false,
        .hInstance = hInstance,
        .nShowCmd = nShowCmd,
    });

    return 0;
}