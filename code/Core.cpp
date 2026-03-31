// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Core.h"

#include <chrono>

Core::Core() : m_Window(), m_Renderer(m_Window)
{
}

void Core::Run(const Window::RunInfo& runInfo)
{
    m_Window.Run(*this, runInfo);
}

void Core::OnInit()
{
    m_Renderer.Init();
}

void Core::OnUpdate()
{
    using namespace std::chrono;
    static time_point<steady_clock> lastTime = steady_clock::now();
    const time_point<steady_clock> currentTime = steady_clock::now();
    const f32 elapsedSeconds = duration<f32>(currentTime - lastTime).count();
    lastTime = currentTime;

    m_Renderer.GetCamera().Update(elapsedSeconds);
}

void Core::OnRender()
{
    m_Renderer.Render();
}

void Core::OnTerminate()
{
    m_Renderer.Terminate();
}

Renderer& Core::GetRenderer()
{
    return m_Renderer;
}

