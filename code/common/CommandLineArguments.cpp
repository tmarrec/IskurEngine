// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CommandLineArguments.h"

#include <Windows.h>
#include <string.h>

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

    // Skip over the program name (handles quoted or unquoted exe path)
    LPWSTR argsStart = cmdLine;
    if (*argsStart == L'"')
    {
        // Skip opening quote
        argsStart++;

        // Find closing quote
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

    // Skip any whitespace before the real args
    while (*argsStart == L' ' || *argsStart == L'\t')
    {
        argsStart++;
    }

    // Now argsStart points to the arguments (or an empty string)
    WString title = L"Iškur Engine";
    if (*argsStart)
    {
        title += L" ";
        title += argsStart; // Append everything after the exe name
    }

    return title;
}
