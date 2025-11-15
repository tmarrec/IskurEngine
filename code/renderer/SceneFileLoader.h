// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/IskurPackFormat.h"

#include <filesystem>

struct SceneFileData
{
    // Geometry blobs (raw bytes of each chunk)
    Vector<u8> vertBlob;
    Vector<u8> idxBlob;
    Vector<u8> mshlBlob;
    Vector<u8> mlvtBlob;
    Vector<u8> mltrBlob;
    Vector<u8> mlbdBlob;

    // Primitive table from the pack file
    Vector<IEPack::PrimRecord> prims;

    // Texture table and raw texture blob (TXTB)
    Vector<IEPack::TextureRecord> texTable;
    Vector<u8> texBlob;

    // Other data tables
    Vector<D3D12_SAMPLER_DESC> samplers;
    Vector<IEPack::MaterialRecord> materials;
    Vector<IEPack::InstanceRecord> instances;
};

SceneFileData LoadSceneFile(const std::filesystem::path& packFile);
