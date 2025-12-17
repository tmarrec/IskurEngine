// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "ImGui.h"

#include "Renderer.h"
#include "window/Window.h"

#include <cassert>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <unordered_map>
#include <vector>

f32 g_Timing_AverageWindowMs = 2000.0f;

f32 g_ToneMapping_WhitePoint = 1.0f;
f32 g_ToneMapping_Contrast = 1.0f;
f32 g_ToneMapping_Saturation = 1.4f;

f32 g_Camera_FOV = 60.f;
f32 g_Camera_FrustumCullingFOV = 60.f;

f32 g_Sun_Azimuth = 210.0f * (3.1415926535f / 180.f);
f32 g_Sun_Elevation = -48.0f * (3.1415926535f / 180.f);
f32 g_Sun_Intensity = 1.0f;

f32 g_IBL_SpecularIntensity = 1.0f;
f32 g_IBL_SkyIntensity = 1.0f;

f32 g_AutoExposure_TargetPct = 0.82f;
f32 g_AutoExposure_LowReject = 0.02f;
f32 g_AutoExposure_HighReject = 0.95f;
f32 g_AutoExposure_Key = 0.22f;
f32 g_AutoExposure_MinLogLum = -3.5f;
f32 g_AutoExposure_MaxLogLum = 3.5f;
f32 g_AutoExposure_ClampMin = 1.0f / 32.0f;
f32 g_AutoExposure_ClampMax = 32.0f;
f32 g_AutoExposure_TauBright = 0.20f;
f32 g_AutoExposure_TauDark = 1.50f;

bool g_RTShadows_Enabled = true;
RayTracingResolution g_RTShadows_Type = RayTracingResolution::Full;

EnvironmentFile g_EnvironmentFile_Type = EnvironmentFile::OvercastSoil;

bool g_ShadersCompilationSuccess = true;

u32 g_PathTrace_SppCached = 1;    // SPP when the cache already has enough samples
u32 g_PathTrace_SppNotCached = 1; // SPP while the cache is still filling
u32 g_PathTrace_BounceCount = 2;  // Diffuse bounce count

bool g_RadianceCache_Trilinear = true;               // Enable trilinear lookup across neighboring cells
u32 g_RadianceCache_TrilinearMinCornerSamples = 128; // Minimum samples at a cache corner before blending
u32 g_RadianceCache_TrilinearMinHits = 2;            // Required neighboring corners for trilinear lookup
u32 g_RadianceCache_TrilinearPresentMinSamples = 64; // Samples needed for a corner to count as present

u32 g_RadianceCache_NormalBinRes = 16;      // Normal bin resolution for cache keying
u32 g_RadianceCache_MinExtraSppCount = 16u; // Samples required before trusting cache results
u32 g_RadianceCache_MaxAge = 256u;          // Frames before a cache entry is treated as stale
u32 g_RadianceCache_MaxProbes = 16;         // Max probe attempts in the cache hash table
u32 g_RadianceCache_MaxSamples = 8192u;     // Per-entry sample cap
f32 g_RadianceCache_CellSize = 0.3f;        // Cache cell size in meters

namespace
{
struct ImGuiAllocCtx
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    u32 next = 0;
    u32 capacity = 0;
    u32 inc = 0;
};

ID3D12DescriptorHeap* gSrvHeap = nullptr;
ImGuiAllocCtx gAlloc{};

double FindSmooth(const char* name, const ImGui_TimingSmooth* arr, u32 n, double fallback)
{
    for (u32 i = 0; i < n; ++i)
    {
        if (std::strcmp(arr[i].name, name) == 0)
        {
            return arr[i].value;
        }
    }
    return fallback;
}

} // namespace

void ImGui_Init(const ImGui_InitParams& p)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (p.fontPath && p.fontPath[0])
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFont* f = io.Fonts->AddFontFromFileTTF(p.fontPath, p.fontSize);
        IM_ASSERT(f && "Failed to load font");
        io.FontDefault = f;
    }

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(Window::GetInstance().GetHwnd());

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 512; // plenty for font + debug images
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = p.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&gSrvHeap));
    IM_ASSERT(SUCCEEDED(hr) && "Failed to create ImGui SRV heap");

    gAlloc.cpu = gSrvHeap->GetCPUDescriptorHandleForHeapStart();
    gAlloc.gpu = gSrvHeap->GetGPUDescriptorHandleForHeapStart();
    gAlloc.capacity = desc.NumDescriptors;
    gAlloc.inc = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ImGui_ImplDX12_InitInfo ii{};
    ii.Device = p.device;
    ii.CommandQueue = p.queue;
    ii.NumFramesInFlight = IE_Constants::frameInFlightCount;
    ii.RTVFormat = p.rtvFormat;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = gSrvHeap;
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        IM_ASSERT(gAlloc.next < gAlloc.capacity && "ImGui SRV heap exhausted");
        outCpu->ptr = gAlloc.cpu.ptr + static_cast<size_t>(gAlloc.next) * gAlloc.inc;
        outGpu->ptr = gAlloc.gpu.ptr + static_cast<size_t>(gAlloc.next) * gAlloc.inc;
        gAlloc.next++;
    };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&ii);
    ImGui_ImplDX12_CreateDeviceObjects();
}

void ImGui_Shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (gSrvHeap)
    {
        gSrvHeap->Release();
        gSrvHeap = nullptr;
    }
    gAlloc = {};
}

void ImGui_Render(const ImGui_RenderParams& p)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y), ImGuiCond_Always);
    ImGuiWindowFlags statsFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("Stats", nullptr, statsFlags);
    ImGui::Text("FPS: %u", p.frame.fps);
    ImGui::Text("Camera pos: %.1f  %.1f  %.1f", p.frame.cameraPos[0], p.frame.cameraPos[1], p.frame.cameraPos[2]);
    ImGui::Separator();
    ImGui::SliderFloat("GPU timing avg window (ms)", &g_Timing_AverageWindowMs, 0.0f, 5000.0f, "%.0f");

    double totalSmooth = 0.0;
    for (u32 i = 0; i < p.timingsRawCount; ++i)
    {
        const auto& r = p.timingsRaw[i];
        totalSmooth += FindSmooth(r.name, p.timingsSmooth, p.timingsSmoothCount, r.ms);
    }

    if (ImGui::BeginTable("GpuTimingsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("% of total", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (u32 i = 0; i < p.timingsRawCount; ++i)
        {
            const auto& r = p.timingsRaw[i];
            const double avg = FindSmooth(r.name, p.timingsSmooth, p.timingsSmoothCount, r.ms);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<f32>(avg));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f%%", (totalSmooth > 0.0) ? static_cast<f32>(avg * 100.0 / totalSmooth) : 0.0f);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Total (sum of listed)");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f", static_cast<f32>(totalSmooth));
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%s", (totalSmooth > 0.0) ? "100.0%" : "0.0%");

        ImGui::EndTable();
    }

    // Grab final Stats pos/size before ending the window
    ImGui::End();

    // Texture debug window: select one texture from a dropdown and preview it
    {
        // Build list of available textures with labels
        std::vector<const char*> labels;
        std::vector<ID3D12Resource*> textures;
        auto maybe_add = [&](const char* name, ID3D12Resource* res) {
            if (res)
            {
                labels.push_back(name);
                textures.push_back(res);
            }
        };
        maybe_add("Albedo", p.gbufferAlbedo);
        maybe_add("Normal", p.gbufferNormal);
        maybe_add("NormalGeo", p.gbufferNormalGeo);
        maybe_add("Material", p.gbufferMaterial);
        maybe_add("Motion", p.gbufferMotion);
        maybe_add("AO", p.gbufferAO);
        maybe_add("Depth", p.depth);
        maybe_add("RT Shadows", p.rtShadows);
        maybe_add("Indirect Diffuse", p.rtIndirectDiffuse);

        if (!labels.empty())
        {
            // SRV cache per resource pointer (persists across frames)
            static std::unordered_map<ID3D12Resource*, D3D12_GPU_DESCRIPTOR_HANDLE> sSrvCache;
            auto getSrv = [&](ID3D12Resource* res) -> D3D12_GPU_DESCRIPTOR_HANDLE {
                auto it = sSrvCache.find(res);
                if (it != sSrvCache.end())
                    return it->second;

                if (gAlloc.next >= gAlloc.capacity)
                {
                    return D3D12_GPU_DESCRIPTOR_HANDLE{}; // out of descriptors
                }
                D3D12_CPU_DESCRIPTOR_HANDLE cpu;
                D3D12_GPU_DESCRIPTOR_HANDLE gpu;
                cpu.ptr = gAlloc.cpu.ptr + static_cast<size_t>(gAlloc.next) * gAlloc.inc;
                gpu.ptr = gAlloc.gpu.ptr + static_cast<size_t>(gAlloc.next) * gAlloc.inc;
                gAlloc.next++;

                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                DXGI_FORMAT viewFmt = res->GetDesc().Format;
                if (viewFmt == DXGI_FORMAT_R32_TYPELESS)
                    viewFmt = DXGI_FORMAT_R32_FLOAT;
                if (viewFmt == DXGI_FORMAT_R16_TYPELESS)
                    viewFmt = DXGI_FORMAT_R16_UNORM;
                srv.Format = viewFmt;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                ID3D12Device* dev = nullptr;
                p.cmd->GetDevice(IID_PPV_ARGS(&dev));
                dev->CreateShaderResourceView(res, &srv, cpu);
                dev->Release();

                sSrvCache.emplace(res, gpu);
                return gpu;
            };

            // Dock GBuffer just under Stats
            const ImVec2 bottomLeft(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y);
            ImGui::SetNextWindowPos(bottomLeft, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(640.0f, 428.0f), ImGuiCond_Always);
            // ImGui::SetNextWindowSize(ImVec2(640.0f * 2, 428.0f * 2), ImGuiCond_Always); // for debug

            ImGui::Begin("GBuffer", nullptr, ImGuiWindowFlags_NoSavedSettings);

            static i32 selected = 0;
            if (selected < 0 || selected >= static_cast<i32>(labels.size()))
                selected = 0;
            ImGui::Combo("Texture", &selected, labels.data(), static_cast<i32>(labels.size()));

            ID3D12Resource* res = textures[selected];
            D3D12_GPU_DESCRIPTOR_HANDLE h = getSrv(res);

            const f32 avail = ImGui::GetContentRegionAvail().x;
            const f32 aspect = (p.renderHeight > 0 && p.renderWidth > 0) ? static_cast<f32>(p.renderHeight) / static_cast<f32>(p.renderWidth) : 1.0f;
            ImVec2 sz(avail, avail * aspect);

            if (h.ptr)
                ImGui::Image(h.ptr, sz);
            else
                ImGui::TextUnformatted("<no descriptor available>");

            ImGui::End();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGuiWindowFlags settingsFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("Settings", nullptr, settingsFlags);

    if (ImGui::Button("Reload Shaders"))
    {
        Renderer::GetInstance().RequestShaderReload();
    }
    if (!g_ShadersCompilationSuccess)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Failed!");
    }

    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* environmentFileLabels[] = {"AutumnField", "BelfastSunset", "PartlyCloudy", "OvercastSoil"};
        static i32 currentEnvironmentFIle = static_cast<i32>(g_EnvironmentFile_Type);
        if (ImGui::Combo("Environment Name", &currentEnvironmentFIle, environmentFileLabels, IM_ARRAYSIZE(environmentFileLabels)))
        {
            g_EnvironmentFile_Type = static_cast<EnvironmentFile>(currentEnvironmentFIle);
        }
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("FOV", &g_Camera_FOV, 10.0f, 120.0f);
        ImGui::SliderFloat("Frustum Culling FOV", &g_Camera_FrustumCullingFOV, 10.0f, 120.0f);
    }

    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderAngle("Azimuth", &g_Sun_Azimuth, 0.0f, 360.0f);
        ImGui::SliderAngle("Elevation", &g_Sun_Elevation, -89.0f, 89.0f);
        ImGui::SliderFloat("Intensity", &g_Sun_Intensity, 0.f, 8.f);
    }

    if (ImGui::CollapsingHeader("IBL", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Specular Intensity", &g_IBL_SpecularIntensity, 0.f, 2.f);
        ImGui::SliderFloat("Sky Intensity", &g_IBL_SkyIntensity, 0.f, 2.f);
    }

    if (ImGui::CollapsingHeader("Path Trace", ImGuiTreeNodeFlags_DefaultOpen))
    {
        {
            const u32 minv = 0, maxv = 8;
            ImGui::SliderScalar("Spp cached", ImGuiDataType_U32, &g_PathTrace_SppCached, &minv, &maxv);
        }
        {
            const u32 minv = 0, maxv = 8;
            ImGui::SliderScalar("Spp not cached", ImGuiDataType_U32, &g_PathTrace_SppNotCached, &minv, &maxv);
        }
        {
            const u32 minv = 0, maxv = 8;
            ImGui::SliderScalar("Bounce count", ImGuiDataType_U32, &g_PathTrace_BounceCount, &minv, &maxv);
        }
        {
            const u32 minv = 0u, maxv = 256u;
            ImGui::SliderScalar("RC min extra spp count", ImGuiDataType_U32, &g_RadianceCache_MinExtraSppCount, &minv, &maxv);
        }
        {
            const u32 minv = 0u, maxv = 4096u;
            ImGui::SliderScalar("RC max age", ImGuiDataType_U32, &g_RadianceCache_MaxAge, &minv, &maxv);
        }
        {
            const u32 minv = 1, maxv = 64;
            ImGui::SliderScalar("RC normal bin res", ImGuiDataType_U32, &g_RadianceCache_NormalBinRes, &minv, &maxv);
        }
        {
            const u32 minv = 0u, maxv = 64u;
            ImGui::SliderScalar("RC max probes", ImGuiDataType_U32, &g_RadianceCache_MaxProbes, &minv, &maxv);
        }
        {
            const u32 minv = 1u, maxv = 16384u;
            ImGui::SliderScalar("RC max samples", ImGuiDataType_U32, &g_RadianceCache_MaxSamples, &minv, &maxv);
        }
        {
            ImGui::SliderFloat("RC cell size", &g_RadianceCache_CellSize, 0.01f, 2.0f);
        }
        ImGui::Checkbox("RC trilinear", &g_RadianceCache_Trilinear);

        ImGui::BeginDisabled(!g_RadianceCache_Trilinear);
        {
            const u32 minv = 0, maxv = 4096;
            ImGui::SliderScalar("RC min corner samples", ImGuiDataType_U32, &g_RadianceCache_TrilinearMinCornerSamples, &minv, &maxv);
        }
        {
            const u32 minv = 0, maxv = 8;
            ImGui::SliderScalar("RC min hits", ImGuiDataType_U32, &g_RadianceCache_TrilinearMinHits, &minv, &maxv);
        }
        {
            const u32 minv = 0, maxv = 4096;
            ImGui::SliderScalar("RC present min samples", ImGuiDataType_U32, &g_RadianceCache_TrilinearPresentMinSamples, &minv, &maxv);
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("RT Shadows", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("RT Shadows Enabled", &g_RTShadows_Enabled);
        const char* rtResLabels[] = {"Full", "FullX_HalfY", "Half", "Quarter"};
        static i32 currentRTRes = static_cast<i32>(g_RTShadows_Type);
        if (ImGui::Combo("Ray-trace Resolution", &currentRTRes, rtResLabels, IM_ARRAYSIZE(rtResLabels)))
        {
            g_RTShadows_Type = static_cast<RayTracingResolution>(currentRTRes);
        }
    }

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("White Point", &g_ToneMapping_WhitePoint, 0.0f, 32.0f);
        ImGui::SliderFloat("Contrast", &g_ToneMapping_Contrast, 0.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &g_ToneMapping_Saturation, 0.0f, 3.0f);
    }

    if (ImGui::CollapsingHeader("Auto-Exposure"))
    {
        ImGui::SliderFloat("Target %", &g_AutoExposure_TargetPct, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Low Reject", &g_AutoExposure_LowReject, 0.0f, 0.2f, "%.2f");
        ImGui::SliderFloat("High Reject", &g_AutoExposure_HighReject, 0.8f, 1.0f, "%.2f");
        ImGui::SliderFloat("Grey (Key)", &g_AutoExposure_Key, 0.05f, 0.50f, "%.2f");
        ImGui::SliderFloat("Min Log Lum", &g_AutoExposure_MinLogLum, -16.0f, 0.0f, "%.1f");
        ImGui::SliderFloat("Max Log Lum", &g_AutoExposure_MaxLogLum, 0.0f, 16.0f, "%.1f");
        ImGui::SliderFloat("Light Adapt Time (s)", &g_AutoExposure_TauBright, 0.05f, 0.5f, "%.2f");
        ImGui::SliderFloat("Dark  Adapt Time (s)", &g_AutoExposure_TauDark, 0.5f, 6.0f, "%.2f");
        ImGui::SliderFloat("Clamp Min", &g_AutoExposure_ClampMin, 1.0f / 256.0f, 1.0f, "%.5f");
        ImGui::SliderFloat("Clamp Max", &g_AutoExposure_ClampMax, 1.0f, 256.0f, "%.1f");
    }

    ImGui::End();

    ImGui::Render();

    p.cmd->OMSetRenderTargets(1, &p.rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {gSrvHeap};
    p.cmd->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), p.cmd);
}
