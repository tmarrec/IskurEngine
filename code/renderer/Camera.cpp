// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Camera.h"

#include "../common/Asserts.h"
#include "../window/Window.h"

#include <DirectXMath.h>

void Camera::Init()
{
    const f32 aspectRatio = Window::GetInstance().GetAspectRatio();
    ConfigurePerspective(aspectRatio, IE_PI_4, IE_PI_4, 0.01f, 0, 0);
}

void Camera::LoadSceneConfig(const String& sceneName)
{
    if (sceneName == "Bistro")
    {
        m_Position = {-17.504068, 6.6169343, -0.6422801};
        m_Yaw = 360.59894;
        m_Pitch = -14.899944;
        m_Front = {0.96632355, -0.2571319, 0.010101734};
    }
    else if (sceneName == "Sponza")
    {
        m_Position = {-6.7842293, 2.0273955, -1.6356962};
        m_Yaw = 393.5991;
        m_Pitch = -2.6999128;
        m_Front = {0.8320053, -0.04710493, 0.55276424};
    }
    else if (sceneName == "Sponza2")
    {
        m_Position = {12.435444, 1.1098297, -0.71890974};
        m_Yaw = 532.3988;
        m_Pitch = 12.500055;
        m_Front = {-0.96771693, 0.21644057, 0.12914129};
    }
    else if (sceneName == "San-Miguel")
    {
        m_Position = {20.144629, 11.589096, 5.851092};
        m_Yaw = 208.59715;
        m_Pitch = -34.299847;
        m_Front = {-0.7253213, -0.5635238, -0.39541113};
    }
    else if (sceneName == "AlphaBlendModeTest")
    {
        m_Position = {-0.04540815, 2.3986704, 4.6940866};
        m_Yaw = 270.3981;
        m_Pitch = -11.699936;
        m_Front = {0.00680406, -0.20278622, -0.9791994};
    }
    else if (sceneName == "NormalTangentTest" || sceneName == "NormalTangentMirrorTest")
    {
        m_Position = {0.014327605, 0.088846914, 2.6952298};
        m_Yaw = 270.79776;
        m_Pitch = -2.6999424;
        m_Front = {0.013907751, -0.04710545, -0.9987931};
    }
    else if (sceneName == "MetalRoughSpheres" || sceneName == "MetalRoughSpheresNoTextures")
    {
        m_Position = {0.28876197, 0.8269017, 10.415524};
        m_Yaw = 270.1988;
        m_Pitch = -4.8999395;
        m_Front = {0.0034567926, -0.08541588, -0.9963394};
    }
    else if (sceneName == "DamagedHelmet")
    {
        m_Position = {-1.2710273, 1.1039577, 1.8417152};
        m_Yaw = 303.39874;
        m_Pitch = -26.899942;
        m_Front = {0.49090138, -0.45243382, -0.7445263};
    }
    else if (sceneName == "SSAO")
    {
        m_Position = {1.1467777, -0.1576769, 2.5923784};
        m_Yaw = 251.5986;
        m_Pitch = -29.49994;
        m_Front = {-0.27474692, -0.49242267, -0.8258535};
    }
    else if (sceneName == "ABeautifulGame")
    {
        m_Position = {-13.332466, 6.18413, -1.7803445};
        m_Yaw = 373.19904;
        m_Pitch = -27.699947;
        m_Front = {0.8620044, -0.46484122, 0.2021658};
    }
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

    f32 moveMultiplier = 1.f;

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
    if (m_KeysPressed.e)
    {
        move += {0, 1, 0};
    }
    if (m_KeysPressed.q)
    {
        move += {0, -1, 0};
    }
    if (m_KeysPressed.leftShift)
    {
        moveMultiplier *= 4.f;
    }
    if (m_KeysPressed.leftCtrl)
    {
        moveMultiplier *= 0.25f;
    }

    m_Position += move * m_MoveSpeed * moveMultiplier * elapsedSeconds;

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

const float4x4& Camera::GetProjectionNoJitter() const
{
    return m_ProjectionNoJitter;
}

const float4x4& Camera::GetFrustumCullingProjection() const
{
    return m_FrustumCullingProjection;
}

float2 Camera::GetZNearFar() const
{
    return m_ZNearFar;
}

void Camera::OnKeyDown(u64 key)
{
    switch (key)
    {
    case 'Q':
        m_KeysPressed.q = true;
        break;
    case 'W':
        m_KeysPressed.w = true;
        break;
    case 'E':
        m_KeysPressed.e = true;
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
    case 'Q':
        m_KeysPressed.q = false;
        break;
    case 'W':
        m_KeysPressed.w = false;
        break;
    case 'E':
        m_KeysPressed.e = false;
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

void Camera::ConfigurePerspective(f32 aspectRatio, f32 yfov, f32 frustumCullingYfov, f32 znear, f32 jitterX, f32 jitterY)
{
    const f32 f = 1.0f / tanf(yfov * 0.5f);
    const f32 fC = 1.0f / tanf(frustumCullingYfov * 0.5f);

    const float4 col0 = {f / aspectRatio, 0, 0, 0};
    const float4 col1 = {0, f, 0, 0};
    const float4 col2 = {0, 0, 0, -1};

    const float4 col3_noj = {0, 0, znear, 0};
    const float4 col3_jit = {jitterX, jitterY, znear, 0};

    m_ProjectionNoJitter = {col0, col1, col2, col3_noj};
    m_Projection = {col0, col1, col2, col3_jit};

    const float4 col3C_noj = {0, 0, znear, 0};
    m_FrustumCullingProjection = {{fC / aspectRatio, 0, 0, 0}, {0, fC, 0, 0}, {0, 0, 0, -1}, col3C_noj};

    m_ZNearFar = {znear, FLT_MAX};
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
