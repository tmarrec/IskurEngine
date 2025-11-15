// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "ImGui.h"

#include "window/Window.h"

#include <cassert>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <unordered_map>
#include <vector>

// ========= TWEAKABLE DEFINITIONS =========
f32 g_Timing_AverageWindowMs = 2000.0f;

f32 g_ToneMapping_WhitePoint = 1.0f;
f32 g_ToneMapping_Contrast = 1.0f;
f32 g_ToneMapping_Saturation = 1.4f;

f32 g_Camera_FOV = 60.f;
f32 g_Camera_FrustumCullingFOV = 60.f;

f32 g_Sun_Azimuth = 210.0f * (3.1415926535f / 180.f);
f32 g_Sun_Elevation = -48.0f * (3.1415926535f / 180.f);
f32 g_Sun_Intensity = 1.0f;

f32 g_IBL_DiffuseIntensity = 1.0f;
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
RayTracingResolution g_RTShadows_Type = RayTracingResolution::FullX_HalfY;
f32 g_RTShadows_IBLDiffuseIntensity = 0.25f;
f32 g_RTShadows_IBLSpecularIntensity = 0.75f;

f32 g_SSAO_SampleRadius = 0.05f;
f32 g_SSAO_SampleBias = 0.0001f;
f32 g_SSAO_Power = 1.35f;

namespace
{
struct ImGuiAllocCtx
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t next = 0;
    uint32_t capacity = 0;
    uint32_t inc = 0;
};

ID3D12DescriptorHeap* gSrvHeap = nullptr;
ImGuiAllocCtx gAlloc{};

double FindSmooth(const char* name, const ImGui_TimingSmooth* arr, uint32_t n, double fallback)
{
    for (uint32_t i = 0; i < n; ++i)
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
    for (uint32_t i = 0; i < p.timingsRawCount; ++i)
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

        for (uint32_t i = 0; i < p.timingsRawCount; ++i)
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
    ImVec2 statsPos = ImGui::GetWindowPos();
    ImVec2 statsSize = ImGui::GetWindowSize();
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
        maybe_add("Material", p.gbufferMaterial);
        maybe_add("Motion", p.gbufferMotion);
        maybe_add("AO", p.gbufferAO);
        maybe_add("Depth", p.depth);
        maybe_add("RT Shadows", p.rtShadows);
        maybe_add("SSAO", p.ssao);

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
                D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
                D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
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
            ImGui::SetNextWindowSize(ImVec2(640.0f, 426.0f), ImGuiCond_Always);

            ImGui::Begin("GBuffer", nullptr, ImGuiWindowFlags_NoSavedSettings);

            static int selected = 0;
            if (selected < 0 || selected >= (int)labels.size())
                selected = 0;
            ImGui::Combo("Texture", &selected, labels.data(), (int)labels.size());

            ID3D12Resource* res = textures[selected];
            D3D12_GPU_DESCRIPTOR_HANDLE h = getSrv(res);

            const float avail = ImGui::GetContentRegionAvail().x;
            const float aspect = (p.renderHeight > 0 && p.renderWidth > 0) ? (float)p.renderHeight / (float)p.renderWidth : 1.0f;
            ImVec2 sz(avail, avail * aspect);

            if (h.ptr)
                ImGui::Image((ImTextureID)h.ptr, sz);
            else
                ImGui::TextUnformatted("<no descriptor available>");

            ImGui::End();
        }
    }

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGuiWindowFlags settingsFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("Settings", nullptr, settingsFlags);

    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("White Point", &g_ToneMapping_WhitePoint, 0.0f, 32.0f);
        ImGui::SliderFloat("Contrast", &g_ToneMapping_Contrast, 0.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &g_ToneMapping_Saturation, 0.0f, 3.0f);
    }

    if (ImGui::CollapsingHeader("Auto-Exposure", ImGuiTreeNodeFlags_DefaultOpen))
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
        ImGui::SliderFloat("Diffuse Intensity", &g_IBL_DiffuseIntensity, 0.f, 2.f);
        ImGui::SliderFloat("Specular Intensity", &g_IBL_SpecularIntensity, 0.f, 2.f);
        ImGui::SliderFloat("Sky Intensity", &g_IBL_SkyIntensity, 0.f, 2.f);
    }

    if (ImGui::CollapsingHeader("RT Shadows", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("RT Shadows Enabled", &g_RTShadows_Enabled);
        const char* RTResLabels[] = {"Full", "FullX_HalfY", "Half", "Quarter"};
        static int currentRTRes = static_cast<int>(g_RTShadows_Type);
        if (ImGui::Combo("Ray-trace Resolution", &currentRTRes, RTResLabels, IM_ARRAYSIZE(RTResLabels)))
        {
            g_RTShadows_Type = static_cast<RayTracingResolution>(currentRTRes);
        }
        ImGui::SliderFloat("IBL Diffuse Intensity", &g_RTShadows_IBLDiffuseIntensity, 0.f, 1.f);
        ImGui::SliderFloat("IBL Specular Intensity", &g_RTShadows_IBLSpecularIntensity, 0.f, 1.f);
    }

    if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Sample Radius", &g_SSAO_SampleRadius, 0.f, 0.1f, "%.6f");
        ImGui::SliderFloat("Sample Bias", &g_SSAO_SampleBias, 0.f, 0.001f, "%.8f");
        ImGui::SliderFloat("Power", &g_SSAO_Power, 0.f, 3.f);
    }

    ImGui::End();

    ImGui::Render();

    p.cmd->OMSetRenderTargets(1, &p.rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {gSrvHeap};
    p.cmd->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), p.cmd);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = p.rtvResource;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmd->ResourceBarrier(1, &b);
}