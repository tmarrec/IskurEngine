// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>

struct ID3D12Device;

void IE_Assert(bool condition);

void IE_SetDeviceRemovedReasonDevice(ID3D12Device* device);

bool IE_Try(HRESULT hr);

void IE_Check(HRESULT hr);

