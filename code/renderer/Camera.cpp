// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Camera.h"

#include "window/Window.h"

#include <DirectXMath.h>

namespace
{
POINT GetClientCenter(HWND hwnd)
{
    RECT r;
    GetClientRect(hwnd, &r);
    return {r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2};
}

void CenterCursorOnClient(HWND hwnd)
{
    POINT c = GetClientCenter(hwnd);
    ClientToScreen(hwnd, &c);
    SetCursorPos(c.x, c.y);
}

void ComputeMouseOffsetFromClient(i32 x, i32 y, HWND hwnd, XMFLOAT2& out)
{
    POINT c = GetClientCenter(hwnd);
    out.x = static_cast<f32>(x - c.x);
    out.y = static_cast<f32>(y - c.y);
}
} // namespace

void Camera::Init()
{
    ConfigurePerspective(Window::GetInstance().GetAspectRatio(), IE_PI_4, IE_PI_4, 0.01f, 0, 0);
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
    else if (sceneName == "CompareAmbientOcclusion")
    {
        m_Position = {-0.0812394544, 1.96597433, 2.14788842};
        m_Yaw = 270.798035;
        m_Pitch = -39.2999687;
        m_Front = {0.0107780313, -0.633380473, -0.773765504};
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

    // Movement
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

    //  Mouse look
    if (m_MouseOffset.x != 0.0f || m_MouseOffset.y != 0.0f)
    {
        m_Yaw += m_MouseOffset.x * m_MouseSensitivity;
        m_Pitch = IE_Clamp(m_Pitch - m_MouseOffset.y * m_MouseSensitivity, -89.9f, 89.9f);

        f32 yaw = IE_ToRadians(m_Yaw);
        f32 pitch = IE_ToRadians(m_Pitch);
        f32 cx = cosf(yaw) * cosf(pitch);
        f32 cy = sinf(pitch);
        f32 cz = sinf(yaw) * cosf(pitch);
        XMVECTOR dir = XMVector3Normalize(XMVectorSet(cx, cy, cz, 0.0f));
        XMStoreFloat3(&m_Front, dir);
        front = dir;
    }
    m_MouseOffset = {0.f, 0.f};

    // Camera frame data
    f32 f = 1.0f / tanf(m_Yfov * 0.5f);
    f32 fC = 1.0f / tanf(m_FrustumCullingYfov * 0.5f);

    XMMATRIX P_jit = XMMatrixSet(f / m_AspectRatio, 0, 0, 0, 0, f, 0, 0, 0, 0, 0, -1, m_JitterX, m_JitterY, m_Znear, 0);
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
        m_HavePrevVP = true;
    }
    else
    {
        m_FrameData.prevViewProjNoJ = m_PrevViewProjNoJ;
    }
    XMStoreFloat4x4(&m_PrevViewProjNoJ, VP_noj);

    std::memcpy(m_FrameData.frustumCullingPlanes, m_FrustumCullingPlanes, sizeof(m_FrustumCullingPlanes));
    m_FrameData.position = m_Position;
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

void Camera::OnMouseMove(i32 x, i32 y)
{
    if (!m_IsFocused)
    {
        return;
    }

    const HWND hwnd = Window::GetInstance().GetHwnd();
    ComputeMouseOffsetFromClient(x, y, hwnd, m_MouseOffset);
    CenterCursorOnClient(hwnd);
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

    const HWND hwnd = Window::GetInstance().GetHwnd();
    if (m_IsFocused)
    {
        POINT c = GetClientCenter(hwnd);
        ComputeMouseOffsetFromClient(c.x, c.y, hwnd, m_MouseOffset);
        CenterCursorOnClient(hwnd);
    }
    else
    {
        m_MouseOffset = {0.f, 0.f};
    }
}
