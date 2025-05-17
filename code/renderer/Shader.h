// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>

#include "../common/Types.h"
#include "AlphaMode.h"

enum ShaderType : u8
{
    IE_SHADER_TYPE_VERTEX,
    IE_SHADER_TYPE_PIXEL,
    IE_SHADER_TYPE_COMPUTE,
    IE_SHADER_TYPE_MESH,
    IE_SHADER_TYPE_AMPLIFICATION,
    IE_SHADER_TYPE_LIB
};

struct Shader
{
    Array<D3D12_SHADER_BYTECODE, AlphaMode_Count> bytecodes;
    WString filename;
};
