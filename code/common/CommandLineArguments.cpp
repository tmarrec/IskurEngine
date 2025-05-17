// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CommandLineArguments.h"

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
        }
    }
}

const CommandLineArguments& GetCommandLineArguments()
{
    static CommandLineArguments args;
    return args;
}
