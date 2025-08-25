// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#define ROOT_SIG \
  "RootFlags( CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED ), " \
  "RootConstants(num32BitConstants=28,b0)"
  
[RootSignature(ROOT_SIG)]
struct RT_RootSig { };