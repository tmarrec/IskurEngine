// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "window/Window.h"

class Core
{
  public:
    static void OnInit();
    static void OnUpdate();
    static void OnRender();
    static void OnTerminate();
};
