// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Singleton.h"

class Camera : public Singleton<Camera>
{
  public:
    void Init();

    void LoadSceneConfig(const String& sceneName);

    void Update(f32 elapsedSeconds);

    XMFLOAT4X4 GetViewMatrix() const;
    XMFLOAT3 GetPosition() const;
    const XMFLOAT4X4& GetProjection() const;
    const XMFLOAT4X4& GetProjectionNoJitter() const;
    const XMFLOAT4X4& GetFrustumCullingProjection() const;
    XMFLOAT2 GetZNearFar() const;

    void OnKeyDown(u64 key);
    void OnKeyUp(u64 key);
    void OnMouseMove(i32 x, i32 y);
    void OnLostFocus();
    void OnGainedFocus();

    void HandleShowCursor() const;

    void ConfigurePerspective(f32 aspectRatio, f32 yfov, f32 frustumCullingYfov, f32 znear, f32 jitterX, f32 jitterY);

  private:
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

    XMFLOAT3 m_Position = {-25.53637f, 3.5737517f, -3.990844f};
    f32 m_Yaw = 374.79907f;
    f32 m_Pitch = -4.89994f;
    XMFLOAT3 m_Front = {0.9632942f, -0.085415885f, 0.25449646f};
    f32 m_MoveSpeed = 10.f;

    XMFLOAT3 m_Up = {0.f, 1.f, 0.f};
    f32 m_MouseSensitivity = 0.2f;

    XMFLOAT2 m_MouseOffset = {0.f, 0.f};

    XMFLOAT4X4 m_Projection = {};
    XMFLOAT4X4 m_ProjectionNoJitter = {};
    XMFLOAT2 m_ZNearFar = {0, 0};

    XMFLOAT4X4 m_FrustumCullingProjection = {};

    KeysPressed m_KeysPressed;
    bool m_IsFocused = false;
};