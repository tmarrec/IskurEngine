// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

struct Shader
{
    D3D12_SHADER_BYTECODE bytecode = {};
    WString filename;
    Vector<WString> defines;
};
