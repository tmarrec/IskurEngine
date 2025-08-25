// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Types.h"

struct CommandLineArguments
{
    String sceneFile;
    bool gpuValidation = false;
};

void ProcessCommandLineArguments(i32 argc, char** argv);

const CommandLineArguments& GetCommandLineArguments();

WString GetWindowTitle();