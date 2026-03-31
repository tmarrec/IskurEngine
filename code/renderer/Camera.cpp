// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Camera.h"

#include "common/StringUtils.h"
#include "window/Window.h"

#include <DirectXMath.h>
#include <sstream>

namespace
{
struct CameraPreset
{
    String sceneName;
    XMFLOAT3 position{};
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
};

XMFLOAT3 ComputeFrontFromYawPitch(const f32 yawDeg, const f32 pitchDeg)
{
    const f32 yaw = IE_ToRadians(yawDeg);
    const f32 pitch = IE_ToRadians(pitchDeg);
    const f32 cx = IE_Cosf(yaw) * IE_Cosf(pitch);
    const f32 cy = IE_Sinf(pitch);
    const f32 cz = IE_Sinf(yaw) * IE_Cosf(pitch);

    XMVECTOR dir = XMVector3Normalize(XMVectorSet(cx, cy, cz, 0.0f));
    XMFLOAT3 front{};
    XMStoreFloat3(&front, dir);
    return front;
}

bool TryParsePresetLine(const String& line, CameraPreset& outPreset)
{
    if (line.empty() || line[0] == '#')
    {
        return false;
    }

    std::istringstream stream(line);
    if (!(stream >> outPreset.sceneName >> outPreset.position.x >> outPreset.position.y >> outPreset.position.z >> outPreset.yaw >> outPreset.pitch))
    {
        return false;
    }

    outPreset.sceneName = ToLowerAscii(outPreset.sceneName);
    return true;
}

bool TryLoadPreset(const String& sceneName, CameraPreset& outPreset)
{
    std::ifstream file("data/camera_presets.txt");
    if (!file.is_open())
    {
        return false;
    }

    const String sceneNameLower = ToLowerAscii(sceneName);
    String line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        CameraPreset preset{};
        if (!TryParsePresetLine(line, preset))
        {
            continue;
        }

        if (preset.sceneName == sceneNameLower)
        {
            outPreset = preset;
            return true;
        }
    }

    return false;
}
} // namespace

Camera::Camera(Window& window) : m_Window(window)
{
}

void Camera::Init()
{
    ConfigurePerspective(m_Window.GetAspectRatio(), IE_PI_4, IE_PI_4, 0.01f, 0, 0);
}

void Camera::LoadSceneConfig(const String& sceneName)
{
    CameraPreset preset{};
    if (TryLoadPreset(sceneName, preset))
    {
        m_Position = preset.position;
        m_Yaw = preset.yaw;
        m_Pitch = preset.pitch;
        m_Front = ComputeFrontFromYawPitch(m_Yaw, m_Pitch);
    }
}

void Camera::ResetHistory()
{
    m_HavePrevVP = false;
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

    XMVECTOR front = XMLoadFloat3(&m_Front);
    XMVECTOR up = XMLoadFloat3(&m_Up);
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));

    XMVECTOR move = XMVectorZero();
    if (m_KeysPressed.w)
    {
        move = XMVectorAdd(move, front);
    }
    if (m_KeysPressed.s)
    {
        move = XMVectorSubtract(move, front);
    }
    if (m_KeysPressed.a)
    {
        move = XMVectorSubtract(move, right);
    }
    if (m_KeysPressed.d)
    {
        move = XMVectorAdd(move, right);
    }
    if (m_KeysPressed.e)
    {
        move = XMVectorAdd(move, XMVectorSet(0.f, 1.f, 0.f, 0.f));
    }
    if (m_KeysPressed.q)
    {
        move = XMVectorAdd(move, XMVectorSet(0.f, -1.f, 0.f, 0.f));
    }

    f32 speedMul = 1.f;
    if (m_KeysPressed.leftShift)
    {
        speedMul *= 4.f;
    }
    if (m_KeysPressed.leftCtrl)
    {
        speedMul *= 0.25f;
    }

    f32 scale = m_MoveSpeed * speedMul * elapsedSeconds;
    XMVECTOR pos = XMLoadFloat3(&m_Position);
    pos = XMVectorAdd(pos, XMVectorScale(move, scale));
    XMStoreFloat3(&m_Position, pos);

    if (m_MouseOffset.x != 0.0f || m_MouseOffset.y != 0.0f)
    {
        m_Yaw += m_MouseOffset.x * m_MouseSensitivity;
        m_Pitch = IE_Clamp(m_Pitch - m_MouseOffset.y * m_MouseSensitivity, -89.9f, 89.9f);

        f32 yaw = IE_ToRadians(m_Yaw);
        f32 pitch = IE_ToRadians(m_Pitch);
        f32 cx = IE_Cosf(yaw) * IE_Cosf(pitch);
        f32 cy = IE_Sinf(pitch);
        f32 cz = IE_Sinf(yaw) * IE_Cosf(pitch);
        XMVECTOR dir = XMVector3Normalize(XMVectorSet(cx, cy, cz, 0.0f));
        XMStoreFloat3(&m_Front, dir);
        front = dir;
    }
    m_MouseOffset = {0.f, 0.f};
}

void Camera::BuildFrameData()
{
    XMVECTOR pos = XMLoadFloat3(&m_Position);
    XMVECTOR front = XMLoadFloat3(&m_Front);
    XMVECTOR up = XMLoadFloat3(&m_Up);

    f32 f = 1.0f / tanf(m_Yfov * 0.5f);
    f32 fC = 1.0f / tanf(m_FrustumCullingYfov * 0.5f);

    XMMATRIX P_jit = XMMatrixSet(f / m_AspectRatio, 0, 0, 0, 0, f, 0, 0, -m_JitterX, -m_JitterY, 0, -1, 0, 0, m_Znear, 0);
    XMMATRIX P_noj = XMMatrixSet(f / m_AspectRatio, 0, 0, 0, 0, f, 0, 0, 0, 0, 0, -1, 0, 0, m_Znear, 0);
    XMMATRIX P_cull = XMMatrixSet(fC / m_AspectRatio, 0, 0, 0, 0, fC, 0, 0, 0, 0, 0, -1, 0, 0, m_Znear, 0);

    XMFLOAT2 znearfar(m_Znear, FLT_MAX);

    XMMATRIX V = XMMatrixLookToRH(pos, front, up);
    XMMATRIX VP_jit = XMMatrixMultiply(V, P_jit);

    XMMATRIX VPc = XMMatrixMultiply(V, P_cull);
    XMMATRIX VPc_T = XMMatrixTranspose(VPc);
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, VPc_T);

    XMVECTOR r0 = XMVectorSet(m._11, m._12, m._13, m._14);
    XMVECTOR r1 = XMVectorSet(m._21, m._22, m._23, m._24);
    XMVECTOR r2 = XMVectorSet(m._31, m._32, m._33, m._34);
    XMVECTOR r3 = XMVectorSet(m._41, m._42, m._43, m._44);
    XMStoreFloat4(&m_FrustumCullingPlanes[0], XMPlaneNormalize(XMVectorAdd(r3, r0)));      // left
    XMStoreFloat4(&m_FrustumCullingPlanes[1], XMPlaneNormalize(XMVectorSubtract(r3, r0))); // right
    XMStoreFloat4(&m_FrustumCullingPlanes[2], XMPlaneNormalize(XMVectorAdd(r3, r1)));      // bottom
    XMStoreFloat4(&m_FrustumCullingPlanes[3], XMPlaneNormalize(XMVectorSubtract(r3, r1))); // top
    XMStoreFloat4(&m_FrustumCullingPlanes[4], XMPlaneNormalize(r2));                       // near
    XMStoreFloat4(&m_FrustumCullingPlanes[5], XMPlaneNormalize(XMVectorSubtract(r3, r2))); // far

    XMStoreFloat4x4(&m_FrameData.view, V);
    XMStoreFloat4x4(&m_FrameData.projection, P_jit);
    XMStoreFloat4x4(&m_FrameData.projectionNoJitter, P_noj);
    XMStoreFloat4x4(&m_FrameData.frustumCullingProjection, P_cull);

    XMMATRIX invV = XMMatrixInverse(nullptr, V);
    XMMATRIX invP_J = XMMatrixInverse(nullptr, P_jit);
    XMMATRIX invVPJ = XMMatrixInverse(nullptr, VP_jit);
    XMStoreFloat4x4(&m_FrameData.invView, invV);
    XMStoreFloat4x4(&m_FrameData.invProjJ, invP_J);
    XMStoreFloat4x4(&m_FrameData.invViewProj, invVPJ);

    XMMATRIX VP_noj = XMMatrixMultiply(V, P_noj);
    XMStoreFloat4x4(&m_FrameData.viewProj, VP_jit);
    XMStoreFloat4x4(&m_FrameData.viewProjNoJ, VP_noj);

    if (!m_HavePrevVP)
    {
        m_FrameData.prevViewProjNoJ = m_FrameData.viewProjNoJ;
        m_FrameData.prevView = m_FrameData.view;
        m_FrameData.prevProjectionNoJitter = m_FrameData.projectionNoJitter;
        m_HavePrevVP = true;
    }
    else
    {
        m_FrameData.prevViewProjNoJ = m_PrevViewProjNoJ;
        m_FrameData.prevView = m_PrevView;
        m_FrameData.prevProjectionNoJitter = m_PrevProjectionNoJitter;
    }
    XMStoreFloat4x4(&m_PrevView, V);
    XMStoreFloat4x4(&m_PrevProjectionNoJitter, P_noj);
    XMStoreFloat4x4(&m_PrevViewProjNoJ, VP_noj);

    std::memcpy(m_FrameData.frustumCullingPlanes, m_FrustumCullingPlanes, sizeof(m_FrustumCullingPlanes));
    m_FrameData.position = m_Position;
    m_FrameData.yaw = m_Yaw;
    m_FrameData.pitch = m_Pitch;
    m_FrameData.front = m_Front;
    m_FrameData.znearfar = znearfar;
}

const Camera::FrameData& Camera::GetFrameData() const
{
    return m_FrameData;
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

void Camera::OnRawMouseDelta(i32 dx, i32 dy)
{
    if (!m_IsFocused)
    {
        return;
    }

    m_MouseOffset.x += static_cast<f32>(dx);
    m_MouseOffset.y += static_cast<f32>(dy);
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
    m_AspectRatio = aspectRatio;
    m_Yfov = yfov;
    m_FrustumCullingYfov = frustumCullingYfov;
    m_Znear = znear;
    m_JitterX = jitterX;
    m_JitterY = jitterY;
}

void Camera::SetFocus(bool value)
{
    m_IsFocused = value;
    m_MouseOffset = {0.f, 0.f};

    HWND hwnd = m_Window.GetHwnd();
    if (!hwnd)
    {
        return;
    }

    if (m_IsFocused)
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);

        POINT topLeft{clientRect.left, clientRect.top};
        POINT bottomRight{clientRect.right, clientRect.bottom};
        ClientToScreen(hwnd, &topLeft);
        ClientToScreen(hwnd, &bottomRight);

        RECT clipRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ClipCursor(&clipRect);
        SetCapture(hwnd);

        POINT center{(topLeft.x + bottomRight.x) / 2, (topLeft.y + bottomRight.y) / 2};
        SetCursorPos(center.x, center.y);
    }
    else
    {
        ClipCursor(nullptr);
        ReleaseCapture();
    }
}

