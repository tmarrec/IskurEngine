// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "common/Types.h"

#include <sl.h>

struct ID3D12Device;

namespace Streamline
{
void CheckResult(sl::Result result);

void Init();
void SetD3DDevice(ID3D12Device* device);
void Shutdown();
} // namespace Streamline
