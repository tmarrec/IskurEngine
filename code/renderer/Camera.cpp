// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Camera.h"

#include "../window/Window.h"

void Camera::Init()
{
    const f32 aspectRatio = Window::GetInstance().GetAspectRatio();
    ConfigurePerspective(aspectRatio, IE_PI_4, 0.01f, 1000.f);
}

void Camera::Update(f32 elapsedSeconds)
{
    if (m_KeysPressed.escape)
    {
        SetFocus(false);
    }
    if (m_KeysPressed.space)
    {
        SetFocus(!m_IsFocused);
        m_KeysPressed.space = false;
    }

    float3 move;
    if (m_KeysPressed.w)
    {
        move += m_Front;
    }
    if (m_KeysPressed.s)
    {
        move -= m_Front;
    }
    if (m_KeysPressed.a)
    {
        move -= float3::Cross(m_Front, m_Up).Normalized();
    }
    if (m_KeysPressed.d)
    {
        move += float3::Cross(m_Front, m_Up).Normalized();
    }
    if (m_KeysPressed.leftShift)
    {
        move += {0, 1, 0};
    }
    if (m_KeysPressed.leftCtrl)
    {
        move += {0, -1, 0};
    }

    m_Position += move * m_MoveSpeed * elapsedSeconds;

    if (m_MouseOffset.x != 0.0f || m_MouseOffset.y != 0.0f)
    {
        m_Yaw += m_MouseOffset.x * m_MouseSensitivity;
        m_Pitch = IE_Clamp(m_Pitch - m_MouseOffset.y * m_MouseSensitivity, -89.9f, 89.9f);

        const float3 dir = float3(cosf(IE_ToRadians(m_Yaw)) * cosf(IE_ToRadians(m_Pitch)), sinf(IE_ToRadians(m_Pitch)), sinf(IE_ToRadians(m_Yaw)) * cosf(IE_ToRadians(m_Pitch))).Normalized();
        m_Front = dir;
    }

    m_MouseOffset.x = 0.0f;
    m_MouseOffset.y = 0.0f;
}

float4x4 Camera::GetViewMatrix() const
{
    return float4x4::LookAtRH(m_Position, m_Position + m_Front, m_Up);
}

float3 Camera::GetPosition() const
{
    /*
    IE_Log("position: {} {} {}", m_Position.x, m_Position.y, m_Position.z);
    IE_Log("yaw: {} pitch: {}", m_Yaw, m_Pitch);
    IE_Log("front: {} {} {}", m_Front.x, m_Front.y, m_Front.z);
    IE_Log("");
    */
    return m_Position;
}

const float4x4& Camera::GetProjection() const
{
    return m_Projection;
}

float2 Camera::GetZNearFar() const
{
    return {m_Near, m_Far};
}

void Camera::OnKeyDown(u64 key)
{
    switch (key)
    {
    case 'W':
        m_KeysPressed.w = true;
        break;
    case 'A':
        m_KeysPressed.a = true;
        break;
    case 'S':
        m_KeysPressed.s = true;
        break;
    case 'D':
        m_KeysPressed.d = true;
        break;
    case VK_SHIFT:
        m_KeysPressed.leftShift = true;
        break;
    case VK_CONTROL:
        m_KeysPressed.leftCtrl = true;
        break;
    case VK_SPACE:
        m_KeysPressed.space = true;
        break;
    case VK_ESCAPE:
        m_KeysPressed.escape = true;
        break;
    default:
        break;
    }
}

void Camera::OnKeyUp(u64 key)
{
    switch (key)
    {
    case 'W':
        m_KeysPressed.w = false;
        break;
    case 'A':
        m_KeysPressed.a = false;
        break;
    case 'S':
        m_KeysPressed.s = false;
        break;
    case 'D':
        m_KeysPressed.d = false;
        break;
    case VK_SHIFT:
        m_KeysPressed.leftShift = false;
        break;
    case VK_CONTROL:
        m_KeysPressed.leftCtrl = false;
        break;
    case VK_SPACE:
        m_KeysPressed.space = false;
        break;
    case VK_ESCAPE:
        m_KeysPressed.escape = false;
        break;
    default:
        break;
    }
}

void Camera::OnMouseMove(i32 x, i32 y)
{
    if (m_IsFocused)
    {
        const HWND& hwnd = Window::GetInstance().GetHwnd();

        RECT rect;
        GetClientRect(hwnd, &rect);
        const i32 centerX = rect.left + (rect.right - rect.left) / 2;
        const i32 centerY = rect.top + (rect.bottom - rect.top) / 2;
        POINT centerPos = {centerX, centerY};
        ClientToScreen(hwnd, &centerPos);
        SetCursorPos(centerPos.x, centerPos.y);

        m_MouseOffset.x = static_cast<f32>(x - centerX);
        m_MouseOffset.y = static_cast<f32>(y - centerY);
    }
}

void Camera::OnLostFocus()
{
    SetFocus(false);
}

void Camera::OnGainedFocus()
{
    SetFocus(m_IsFocused);
}

void Camera::HandleShowCursor() const
{
    if (m_IsFocused)
    {
        while (ShowCursor(false) > 0)
        {
        }
    }
    else
    {
        while (ShowCursor(true) < 1)
        {
        }
    }
}

void Camera::ConfigurePerspective(f32 aspectRatio, f32 yfov, f32 znear, f32 zfar)
{
    m_Projection = float4x4::PerspectiveFovRH(yfov, aspectRatio, znear, zfar);
    m_Near = znear;
    m_Far = zfar;
}

void Camera::ConfigureOrthographic(f32 xmag, f32 ymag, f32 znear, f32 zfar)
{
    m_Projection = float4x4::OrthographicRH(xmag, ymag, znear, zfar);
    m_Near = znear;
    m_Far = zfar;
}

void Camera::SetFocus(bool value)
{
    if (value)
    {
        const HWND& hwnd = Window::GetInstance().GetHwnd();
        RECT rect;
        GetClientRect(hwnd, &rect);
        const i32 centerX = rect.left + (rect.right - rect.left) / 2;
        const i32 centerY = rect.top + (rect.bottom - rect.top) / 2;
        POINT centerPos = {centerX, centerY};
        ClientToScreen(hwnd, &centerPos);
        SetCursorPos(centerPos.x, centerPos.y);
    }

    m_IsFocused = value;
}
