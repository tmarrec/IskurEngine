// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "renderer/Renderer.h"
#include "window/Window.h"

class Core
{
  public:
    Core();

    void Run(const Window::RunInfo& runInfo);

    void OnInit();
    void OnUpdate();
    void OnRender();
    void OnTerminate();

    Renderer& GetRenderer();

  private:
    Window m_Window;
    Renderer m_Renderer;
};

