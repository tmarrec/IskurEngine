// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>
#include <dxcapi.h>

#include "common/Types.h"

enum ShaderType : u8
{
    IE_SHADER_TYPE_VERTEX,
    IE_SHADER_TYPE_PIXEL,
    IE_SHADER_TYPE_COMPUTE,
    IE_SHADER_TYPE_MESH,
    IE_SHADER_TYPE_AMPLIFICATION,
    IE_SHADER_TYPE_LIB
};

class Shader
{
  public:
    struct ReloadStats
    {
        u32 total = 0;
        u32 reloaded = 0;
        u32 skipped = 0;
        u32 failed = 0;
    };

    Shader(ShaderType type, String filename, Vector<String> defines);
    bool Reload();
    static bool ReloadOrCreate(SharedPtr<Shader>& shader, ShaderType type, const String& filename, const Vector<String>& defines);
    static void ResetReloadStats();
    static ReloadStats GetReloadStats();

    D3D12_SHADER_BYTECODE GetBytecode() const;

    const ComPtr<ID3D12RootSignature>& GetOrCreateRootSignature(const ComPtr<ID3D12Device14>& device);

    bool IsValid() const;
    const String& GetErrorLog() const;

  private:
    bool Reload(bool& outDidReload);

    ShaderType m_Type = IE_SHADER_TYPE_VERTEX;
    ComPtr<IDxcBlob> m_Blob;
    String m_Filename;
    Vector<String> m_Defines;
    String m_ErrorLog;
    bool m_Valid = false;
    u64 m_SourceHash = 0;
    bool m_SourceHashValid = false;

    ComPtr<ID3D12RootSignature> m_CachedRootSig;
    ID3D12Device14* m_CachedRootSigDevice = nullptr;
};

