// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>

#include <directx/d3d12.h>
#include <directx/d3dx12.h>

#include <D3D12MemAlloc.h>
#include <array>
#include <dxcapi.h>
#include <dxgiformat.h>
#include <vector>

#include "renderer/Constants.h"

#include "common/Asserts.h"
#include "common/Log.h"
#include "common/Singleton.h"
#include "common/Types.h"
#include "common/math/MathUtils.h"
