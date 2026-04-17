// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include <Windows.h>

#include "common/Types.h"

#include <sl.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

struct ID3D12Device;

namespace Streamline
{
void CheckResult(sl::Result result);

void Init();
void VerifyPCLSupport(const LUID& adapterLuid);
void VerifyReflexSupport(const LUID& adapterLuid);
void VerifyDLSSGSupport(const LUID& adapterLuid);
void InitPCLForCurrentThread(u32 threadId);
void VerifyDLSSGLoaded();
void VerifyReflexLowLatencyAvailable();
void ApplyReflexOptions();
u32 GetPCLStatsWindowMessage();
void SetPCLMarker(sl::PCLMarker marker, u32 frameIndex);
void SleepReflex(u32 frameIndex);
void SetD3DDevice(ID3D12Device* device);
void Shutdown();
} // namespace Streamline
