// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "../common/Singleton.h"

class Camera : public Singleton<Camera>
{
  public:
    void Init();

    void LoadSceneConfig(const String& sceneName);

    void Update(f32 elapsedSeconds);

    float4x4 GetViewMatrix() const;
    float3 GetPosition() const;
    const float4x4& GetProjection() const;
    const float4x4& GetProjectionNoJitter() const;
    const float4x4& GetFrustumCullingProjection() const;
    float2 GetZNearFar() const;

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

    float3 m_Position = {-25.53637f, 3.5737517f, -3.990844f};
    f32 m_Yaw = 374.79907f;
    f32 m_Pitch = -4.89994f;
    float3 m_Front = {0.9632942f, -0.085415885f, 0.25449646f};
    f32 m_MoveSpeed = 10.f;

    float3 m_Up = {0.f, 1.f, 0.f};
    f32 m_MouseSensitivity = 0.2f;

    float2 m_MouseOffset = {0.f, 0.f};

    float4x4 m_Projection = float4x4::Identity();
    float4x4 m_ProjectionNoJitter = float4x4::Identity();
    float2 m_ZNearFar = {0, 0};

    float4x4 m_FrustumCullingProjection = float4x4::Identity();

    KeysPressed m_KeysPressed;
    bool m_IsFocused = false;
};