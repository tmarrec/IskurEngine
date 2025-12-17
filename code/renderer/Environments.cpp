// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Environments.h"

#include "Renderer.h"

#include <DDSTextureLoader.h>

#include <ResourceUploadBatch.h>

void Environments::Load(ID3D12CommandQueue* cmd)
{
    LoadEnvironment(cmd, L"autumn_field_puresky_4k", EnvironmentFile::AutumnField);
    LoadEnvironment(cmd, L"belfast_sunset_puresky_4k", EnvironmentFile::BelfastSunset);
    LoadEnvironment(cmd, L"kloofendal_48d_partly_cloudy_puresky", EnvironmentFile::PartlyCloudy);
    LoadEnvironment(cmd, L"overcast_soil_puresky_4k", EnvironmentFile::OvercastSoil);
}

const Environment& Environments::GetCurrentEnvironment() const
{
    return m_Environments[static_cast<u32>(g_EnvironmentFile_Type)];
}

Environment& Environments::GetCurrentEnvironment()
{
    return m_Environments[static_cast<u32>(g_EnvironmentFile_Type)];
}

void Environments::LoadEnvironment(ID3D12CommandQueue* cmd, const WString& aName, const EnvironmentFile& envEnum)
{
    Renderer& renderer = Renderer::GetInstance();
    Environment& env = m_Environments[static_cast<u32>(envEnum)];

    WString basePath = L"data/textures/" + aName;

    ID3D12Device14* device = renderer.GetDevice().Get();

    ResourceUploadBatch batch(device);
    batch.Begin();

    IE_Check(CreateDDSTextureFromFile(device, batch, (basePath + L"/envMap.dds").c_str(), &env.envCube, false, 0, nullptr, nullptr));
    IE_Check(env.envCube->SetName(L"EnvCubeMap"));

    IE_Check(CreateDDSTextureFromFile(device, batch, (basePath + L"/diffuseIBL.dds").c_str(), &env.diffuseIBL, false, 0, nullptr, nullptr));
    IE_Check(env.diffuseIBL->SetName(L"DiffuseIBL"));

    IE_Check(CreateDDSTextureFromFile(device, batch, (basePath + L"/specularIBL.dds").c_str(), &env.specularIBL, false, 0, nullptr, nullptr));
    IE_Check(env.specularIBL->SetName(L"SpecularIBL"));

    IE_Check(CreateDDSTextureFromFile(device, batch, L"data/textures/BRDF_LUT.dds", &env.brdfLut, false, 0, nullptr, nullptr));
    IE_Check(env.brdfLut->SetName(L"BrdfLut"));

    batch.End(cmd).wait();

    BindlessHeaps& bindlessHeaps = renderer.GetBindlessHeaps();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    srv.Format = DXGI_FORMAT_BC6H_UF16;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = UINT32_MAX;
    env.envSrvIdx = bindlessHeaps.CreateSRV(env.envCube, srv);

    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    env.diffuseSrvIdx = bindlessHeaps.CreateSRV(env.diffuseIBL, srv);

    srv.Format = DXGI_FORMAT_BC6H_UF16;
    env.specularSrvIdx = bindlessHeaps.CreateSRV(env.specularIBL, srv);

    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = UINT32_MAX;
    env.brdfSrvIdx = bindlessHeaps.CreateSRV(env.brdfLut, srv);
}
