// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CommandLineArguments.h"

#include "StringUtils.h"
#include "UtfConversion.h"

void ProcessCommandLineArguments(i32 argc, char** argv)
{
    CommandLineArguments& args = const_cast<CommandLineArguments&>(GetCommandLineArguments());
    if (argc > 1)
    {
        for (i32 i = 1; i < argc; ++i)
        {
            const String option = ToLowerAscii(argv[i]);
            if (option == "--scene" && i + 1 < argc)
            {
                args.sceneFile = argv[++i]; // consume the next argument
            }
            else if (option == "--gpu-validation")
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

String GetWindowTitle()
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

    String title = "Iskur Engine";
    if (*argsStart)
    {
        title += " ";
        title += WideToUtf8(argsStart);
    }

    return title;
}

