// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Core.h"

#include <chrono>

#include "renderer/Camera.h"
#include "renderer/Renderer.h"
#include "window/Window.h"

void Core::OnInit()
{
    Camera::GetInstance().Init();
    Renderer::GetInstance().Init();
}

void Core::OnUpdate()
{
    using namespace std::chrono;
    static time_point<steady_clock> lastTime = high_resolution_clock::now();
    const time_point<steady_clock> currentTime = high_resolution_clock::now();
    const f32 elapsedSeconds = duration<f32>(currentTime - lastTime).count();
    lastTime = currentTime;

    Camera::GetInstance().Update(elapsedSeconds);
}

void Core::OnRender()
{
    Renderer::GetInstance().Render();
}

void Core::OnTerminate()
{
    Renderer::GetInstance().Terminate();
    Renderer::DestroyInstance();
}
