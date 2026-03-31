// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

class Window;

class Camera
{
  public:
    explicit Camera(Window& window);

    struct FrameData
    {
        XMFLOAT3 position = {};
        f32 yaw = 0.0f;
        f32 pitch = 0.0f;
        XMFLOAT3 front = {};
        XMFLOAT2 znearfar = {};

        XMFLOAT4X4 view = {};
        XMFLOAT4X4 projection = {};
        XMFLOAT4X4 projectionNoJitter = {};
        XMFLOAT4X4 frustumCullingProjection = {};

        XMFLOAT4X4 invView = {};
        XMFLOAT4X4 invProjJ = {};
        XMFLOAT4X4 invViewProj = {};

        XMFLOAT4X4 viewProj = {};
        XMFLOAT4X4 viewProjNoJ = {};
        XMFLOAT4X4 prevViewProjNoJ = {};
        XMFLOAT4X4 prevView = {};
        XMFLOAT4X4 prevProjectionNoJitter = {};

        XMFLOAT4 frustumCullingPlanes[6] = {};
    };

    void Init();

    void LoadSceneConfig(const String& sceneName);
    void ResetHistory();

    void Update(f32 elapsedSeconds);
    void BuildFrameData();

    const FrameData& GetFrameData() const;

    void OnKeyDown(u64 key);
    void OnKeyUp(u64 key);
    void OnRawMouseDelta(i32 dx, i32 dy);
    void OnLostFocus();
    void OnGainedFocus();

    void HandleShowCursor() const;

    void ConfigurePerspective(f32 aspectRatio, f32 yfov, f32 frustumCullingYfov, f32 znear, f32 jitterX, f32 jitterY);

  private:
    Window& m_Window;

    void SetFocus(bool value);

    struct KeysPressed
    {
        bool q = false;
        bool w = false;
        bool e = false;
        bool a = false;
        bool s = false;
        bool d = false;
        bool leftShift = false;
        bool leftCtrl = false;
        bool space = false;
        bool escape = false;
    };

    KeysPressed m_KeysPressed;
    bool m_IsFocused = false;

    XMFLOAT3 m_Position = {};
    f32 m_Yaw = 0.0f;
    f32 m_Pitch = 0.0f;
    XMFLOAT3 m_Front = {0.0f, 0.0f, -1.0f};
    f32 m_MoveSpeed = 10.f;

    XMFLOAT3 m_Up = {0.f, 1.f, 0.f};
    f32 m_MouseSensitivity = 0.15f;

    XMFLOAT2 m_MouseOffset = {0.f, 0.f};

    XMFLOAT4 m_FrustumCullingPlanes[6] = {};

    f32 m_AspectRatio = 0.0f;
    f32 m_Yfov = 0.0f;
    f32 m_FrustumCullingYfov = 0.0f;
    f32 m_Znear = 0.0f;
    f32 m_JitterX = 0.0f;
    f32 m_JitterY = 0.0f;

    XMFLOAT4X4 m_PrevViewProjNoJ = {};
    XMFLOAT4X4 m_PrevView = {};
    XMFLOAT4X4 m_PrevProjectionNoJitter = {};
    bool m_HavePrevVP = false; // Set to false when camera teleports

    FrameData m_FrameData = {};
};
