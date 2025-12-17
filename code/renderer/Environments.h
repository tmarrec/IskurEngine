// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "ImGui.h"

struct Environment
{
    ComPtr<ID3D12Resource> envCube;
    u32 envSrvIdx = UINT32_MAX;

    ComPtr<ID3D12Resource> diffuseIBL;
    u32 diffuseSrvIdx = UINT32_MAX;

    ComPtr<ID3D12Resource> specularIBL;
    u32 specularSrvIdx = UINT32_MAX;

    ComPtr<ID3D12Resource> brdfLut;
    u32 brdfSrvIdx = UINT32_MAX;

    XMFLOAT3 sunDir = {0.3f, 1.f, 0.75f};
};

class Environments
{
  public:
    void Load(ID3D12CommandQueue* cmd);

    const Environment& GetCurrentEnvironment() const;
    Environment& GetCurrentEnvironment();

  private:
    void LoadEnvironment(ID3D12CommandQueue* cmd, const WString& aName, const EnvironmentFile& envEnum);

    Array<Environment, static_cast<u32>(EnvironmentFile::Count)> m_Environments;
};