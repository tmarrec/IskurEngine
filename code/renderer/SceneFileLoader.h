// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/IskurPackFormat.h"

#include <filesystem>

struct SceneFileData
{
    // Owns the full pack file bytes.
    Vector<u8> fileBytes;

    // Geometry blob ranges in fileBytes.
    u64 vertBlobOffset = 0;
    u64 vertBlobSize = 0;
    u64 idxBlobOffset = 0;
    u64 idxBlobSize = 0;
    u64 mshlBlobOffset = 0;
    u64 mshlBlobSize = 0;
    u64 mlvtBlobOffset = 0;
    u64 mlvtBlobSize = 0;
    u64 mltrBlobOffset = 0;
    u64 mltrBlobSize = 0;
    u64 mlbdBlobOffset = 0;
    u64 mlbdBlobSize = 0;
    Vector<i32> ommIndices;
    Vector<IEPack::OpacityMicromapDescRecord> ommDescs;
    u64 ommDataBlobOffset = 0;
    u64 ommDataBlobSize = 0;

    // Primitive table from the pack file
    Vector<IEPack::PrimRecord> prims;

    // Texture table, subresource layout, and texture blob range in fileBytes.
    Vector<IEPack::TextureRecord> texTable;
    Vector<IEPack::TextureSubresourceRecord> texSubresources;
    u64 texBlobOffset = 0;
    u64 texBlobSize = 0;

    // Other data tables
    Vector<D3D12_SAMPLER_DESC> samplers;
    Vector<IEPack::MaterialRecord> materials;
    Vector<IEPack::InstanceRecord> instances;

    const u8* VertBlob() const { return vertBlobSize ? fileBytes.data() + vertBlobOffset : nullptr; }
    const u8* IdxBlob() const { return idxBlobSize ? fileBytes.data() + idxBlobOffset : nullptr; }
    const u8* MshlBlob() const { return mshlBlobSize ? fileBytes.data() + mshlBlobOffset : nullptr; }
    const u8* MlvtBlob() const { return mlvtBlobSize ? fileBytes.data() + mlvtBlobOffset : nullptr; }
    const u8* MltrBlob() const { return mltrBlobSize ? fileBytes.data() + mltrBlobOffset : nullptr; }
    const u8* MlbdBlob() const { return mlbdBlobSize ? fileBytes.data() + mlbdBlobOffset : nullptr; }
    const u8* OmmDataBlob() const { return ommDataBlobSize ? fileBytes.data() + ommDataBlobOffset : nullptr; }
    const u8* TexBlob() const { return texBlobSize ? fileBytes.data() + texBlobOffset : nullptr; }
};

SceneFileData LoadSceneFile(const std::filesystem::path& packFile);

