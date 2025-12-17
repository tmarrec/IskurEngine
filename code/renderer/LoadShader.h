// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Shader.h"

enum ShaderType : u8
{
    IE_SHADER_TYPE_VERTEX,
    IE_SHADER_TYPE_PIXEL,
    IE_SHADER_TYPE_COMPUTE,
    IE_SHADER_TYPE_MESH,
    IE_SHADER_TYPE_AMPLIFICATION,
    IE_SHADER_TYPE_LIB
};

SharedPtr<Shader> IE_LoadShader(ShaderType type, const WString& filename, const Vector<WString>& defines, const SharedPtr<Shader>& oldShader);