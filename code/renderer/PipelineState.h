// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <d3d12.h>

#include "AlphaMode.h"

struct PipelineState
{
    Array<ComPtr<ID3D12PipelineState>, AlphaMode_Count> pipelineStates;
    Array<ComPtr<ID3D12RootSignature>, AlphaMode_Count> rootSignatures;

    ComPtr<ID3D12PipelineState> depthPipelineState;
    ComPtr<ID3D12RootSignature> depthRootSignature;
};
