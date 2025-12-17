// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CommandLineArguments.h"

#include <Windows.h>

void ProcessCommandLineArguments(i32 argc, char** argv)
{
    CommandLineArguments& args = const_cast<CommandLineArguments&>(GetCommandLineArguments());
    if (argc > 1)
    {
        for (i32 i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
            {
                args.sceneFile = argv[++i]; // consume the next argument
            }
            else if (strcmp(argv[i], "--gpu-validation") == 0)
            {
                args.gpuValidation = true;
            }
        }
    }
}

const CommandLineArguments& GetCommandLineArguments()
{
    static CommandLineArguments args;
    return args;
}

WString GetWindowTitle()
{
    const LPWSTR cmdLine = GetCommandLineW();

    // Skip the executable name (quoted or not)
    LPWSTR argsStart = cmdLine;
    if (*argsStart == L'"')
    {
        argsStart++;

        while (*argsStart && *argsStart != L'"')
        {
            argsStart++;
        }
        if (*argsStart == L'"')
        {
            argsStart++;
        }
    }
    else
    {
        // Skip until whitespace
        while (*argsStart && *argsStart != L' ' && *argsStart != L'\t')
        {
            argsStart++;
        }
    }

    // Trim leading whitespace before the actual arguments
    while (*argsStart == L' ' || *argsStart == L'\t')
    {
        argsStart++;
    }

    WString title = L"Iškur Engine";
    if (*argsStart)
    {
        title += L" ";
        title += argsStart; // Append the raw arguments
    }

    return title;
}
