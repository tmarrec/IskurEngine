// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "ImGui.h"

#include "Renderer.h"
#include "SceneUtils.h"
#include "common/MathUtils.h"
#include "common/StringUtils.h"
#include "window/Window.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <unordered_map>
#include <vector>

namespace
{
constexpr u32 kImGuiTotalSrvDescriptors = 512;
constexpr u32 kImGuiDebugSrvReserve = 128;

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
u32 gDebugSrvCount = 0;
std::vector<u32> gFreeSrvIndices;
std::unordered_map<ID3D12Resource*, D3D12_GPU_DESCRIPTOR_HANDLE> gDebugSrvCache;

D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(u32 index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    cpu.ptr = gAlloc.cpu.ptr + static_cast<size_t>(index) * gAlloc.inc;
    return cpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(u32 index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    gpu.ptr = gAlloc.gpu.ptr + static_cast<size_t>(index) * gAlloc.inc;
    return gpu;
}

u32 GetHandleIndex(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    IM_ASSERT(gAlloc.inc > 0 && cpu.ptr >= gAlloc.cpu.ptr);
    const size_t byteOffset = cpu.ptr - gAlloc.cpu.ptr;
    IM_ASSERT((byteOffset % gAlloc.inc) == 0 && "Descriptor handle must be aligned to the heap increment");
    return static_cast<u32>(byteOffset / gAlloc.inc);
}

double UpdateCpuFrameAvgMs(double frameMs, float windowMs)
{
    struct Sample
    {
        double value = 0.0;
        float dtMs = 0.0f;
    };

    constexpr u32 kHistoryCapacity = 8192u;
    static Vector<Sample> s_History(kHistoryCapacity);
    static u32 s_HistoryStart = 0u;
    static u32 s_HistoryCount = 0u;

    const float sampleDtMs = static_cast<float>(frameMs);
    if (s_HistoryCount < kHistoryCapacity)
    {
        const u32 writeIndex = (s_HistoryStart + s_HistoryCount) % kHistoryCapacity;
        s_History[writeIndex].value = frameMs;
        s_History[writeIndex].dtMs = sampleDtMs;
        ++s_HistoryCount;
    }
    else
    {
        s_History[s_HistoryStart].value = frameMs;
        s_History[s_HistoryStart].dtMs = sampleDtMs;
        s_HistoryStart = (s_HistoryStart + 1u) % kHistoryCapacity;
    }

    if (windowMs <= 0.0f)
    {
        return frameMs;
    }

    double weightedSum = 0.0;
    double coveredMs = 0.0;
    for (u32 age = 0; age < s_HistoryCount && coveredMs < windowMs; ++age)
    {
        const u32 idx = (s_HistoryStart + s_HistoryCount - 1u - age) % kHistoryCapacity;
        const Sample& entry = s_History[idx];
        const double entryDtMs = entry.dtMs > 0.0f ? static_cast<double>(entry.dtMs) : 0.0;
        if (entryDtMs <= 0.0)
        {
            continue;
        }

        const double remainingMs = static_cast<double>(windowMs) - coveredMs;
        const double takeMs = std::min(entryDtMs, remainingMs);
        weightedSum += entry.value * takeMs;
        coveredMs += takeMs;
    }

    if (coveredMs <= 0.0)
    {
        return frameMs;
    }

    return weightedSum / coveredMs;
}

f32 HorizontalFovFromVerticalDegrees(f32 verticalFovDeg, f32 aspectRatio)
{
    const f32 clampedAspect = IE_Max(aspectRatio, 1e-4f);
    const f32 verticalFovRad = IE_ToRadians(verticalFovDeg);
    const f32 horizontalFovRad = 2.0f * std::atan(std::tan(verticalFovRad * 0.5f) * clampedAspect);
    return IE_ToDegrees(horizontalFovRad);
}

f32 VerticalFovFromHorizontalDegrees(f32 horizontalFovDeg, f32 aspectRatio)
{
    const f32 clampedAspect = IE_Max(aspectRatio, 1e-4f);
    const f32 horizontalFovRad = IE_ToRadians(horizontalFovDeg);
    const f32 verticalFovRad = 2.0f * std::atan(std::tan(horizontalFovRad * 0.5f) / clampedAspect);
    return IE_ToDegrees(verticalFovRad);
}

bool IsShowcaseScene(const String& sceneName)
{
    return EqualsIgnoreCaseAscii(sceneName, "Bistro") || EqualsIgnoreCaseAscii(sceneName, "San-Miguel") || EqualsIgnoreCaseAscii(sceneName, "Sponza");
}

String FormatSceneFileSize(const String& sceneName)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path packPath = SceneUtils::ResolveScenePackPath(sceneName);
    const uintmax_t sizeBytes = fs::file_size(packPath, ec);
    if (ec)
    {
        return "?";
    }

    char buffer[32];
    if (sizeBytes < 1024ull)
    {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(sizeBytes));
    }
    else if (sizeBytes < 1024ull * 1024ull)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(sizeBytes) / 1024.0);
    }
    else if (sizeBytes < 1024ull * 1024ull * 1024ull)
    {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(sizeBytes) / (1024.0 * 1024.0));
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB", static_cast<double>(sizeBytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buffer;
}

} // namespace

void ImGui_Init(const ImGui_InitParams& p)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    style.Colors[ImGuiCol_ChildBg].w = 1.0f;
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.Colors[ImGuiCol_TitleBg].w = 1.0f;
    style.Colors[ImGuiCol_TitleBgActive].w = 1.0f;
    style.Colors[ImGuiCol_MenuBarBg].w = 1.0f;

    if (p.fontPath && p.fontPath[0])
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFont* f = io.Fonts->AddFontFromFileTTF(p.fontPath, p.fontSize);
        IM_ASSERT(f && "Failed to load font");
        io.FontDefault = f;
    }

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(p.hwnd);

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kImGuiTotalSrvDescriptors;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    IE_Check(p.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&gSrvHeap)));

    IM_ASSERT(desc.NumDescriptors > kImGuiDebugSrvReserve && "ImGui debug SRV reserve must fit within the descriptor heap");
    gAlloc.cpu = gSrvHeap->GetCPUDescriptorHandleForHeapStart();
    gAlloc.gpu = gSrvHeap->GetGPUDescriptorHandleForHeapStart();
    gAlloc.capacity = desc.NumDescriptors - kImGuiDebugSrvReserve;
    gAlloc.inc = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gFreeSrvIndices.clear();

    ImGui_ImplDX12_InitInfo ii{};
    ii.Device = p.device;
    ii.CommandQueue = p.queue;
    ii.NumFramesInFlight = IE_Constants::frameInFlightCount;
    ii.RTVFormat = p.rtvFormat;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = gSrvHeap;
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        u32 index = 0;
        if (!gFreeSrvIndices.empty())
        {
            index = gFreeSrvIndices.back();
            gFreeSrvIndices.pop_back();
        }
        else
        {
            IM_ASSERT(gAlloc.next < gAlloc.capacity && "ImGui SRV heap exhausted");
            index = gAlloc.next;
            gAlloc.next++;
        }

        *outCpu = GetCpuHandle(index);
        *outGpu = GetGpuHandle(index);
    };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
        if (cpu.ptr == 0)
            return;

        const u32 index = GetHandleIndex(cpu);
        IM_ASSERT(index < gAlloc.capacity && "ImGui backend tried to free a debug-reserved descriptor");
        gFreeSrvIndices.push_back(index);
    };
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
    gDebugSrvCount = 0;
    gFreeSrvIndices.clear();
    gDebugSrvCache.clear();
}

void ImGui_ResetDebugTextureCache()
{
    gDebugSrvCache.clear();
    gDebugSrvCount = 0;
}

void ImGui_Render(const ImGui_RenderParams& p)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    IE_Assert(p.renderer != nullptr);
    const double displayedFrameTimeMs = UpdateCpuFrameAvgMs(p.renderer->GetWindow().GetFrameTimeMs(), g_Settings.timingAverageWindowMs);
    const f32 displayedFps = (displayedFrameTimeMs > 0.0) ? static_cast<f32>(1000.0 / displayedFrameTimeMs) : 0.0f;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    constexpr float topToolbarHeight = 42.0f;
    static bool showStatsWindow = true;
    static bool showViewWindow = false;
    static bool showEnvironmentWindow = false;
    static bool showPathTraceWindow = false;
    static bool showTonemapWindow = false;
    static bool showDebugWindow = false;

    if (showStatsWindow)
    {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + topToolbarHeight), ImGuiCond_Always);
        ImGuiWindowFlags statsFlags =
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
        ImGui::Begin("##Stats", nullptr, statsFlags);
        float timingAverageWindowSec = g_Settings.timingAverageWindowMs * 0.001f;
        if (ImGui::SliderFloat("Timing Average Window (s)", &timingAverageWindowSec, 0.0f, 10.0f, "%.1f"))
        {
            g_Settings.timingAverageWindowMs = timingAverageWindowSec * 1000.0f;
        }
        const double statsFrameTimeMs = UpdateCpuFrameAvgMs(p.renderer->GetWindow().GetFrameTimeMs(), g_Settings.timingAverageWindowMs);
        ImGui::Text("%s %.3f", (g_Settings.timingAverageWindowMs <= 0.0f) ? "Frame Time (Raw, ms):" : "Frame Time (Average, ms):", static_cast<f32>(statsFrameTimeMs));

        const char* timingValueLabel = (g_Settings.timingAverageWindowMs <= 0.0f) ? "Raw (ms)" : "Average (ms)";

        if (ImGui::BeginTable("CpuTimingsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("CPU Pass");
            ImGui::TableSetupColumn(timingValueLabel, ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("% of Total", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            double cpuTotal = 0.0;
            for (u32 i = 0; i < p.cpuTimingsCount; ++i)
            {
                cpuTotal += p.cpuTimings[i].ms;
            }

            for (u32 i = 0; i < p.cpuTimingsCount; ++i)
            {
                const auto& timing = p.cpuTimings[i];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(timing.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", static_cast<f32>(timing.ms));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f%%", (cpuTotal > 0.0) ? static_cast<f32>(timing.ms * 100.0 / cpuTotal) : 0.0f);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Total");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<f32>(cpuTotal));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", (cpuTotal > 0.0) ? "100.0%" : "0.0%");

            ImGui::EndTable();
        }

        if (ImGui::BeginTable("GpuTimingsTbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("GPU Pass");
            ImGui::TableSetupColumn(timingValueLabel, ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("% of Total", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            double gpuTotal = 0.0;
            for (u32 i = 0; i < p.gpuTimingsCount; ++i)
            {
                gpuTotal += p.gpuTimings[i].ms;
            }

            for (u32 i = 0; i < p.gpuTimingsCount; ++i)
            {
                const auto& timing = p.gpuTimings[i];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(timing.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", static_cast<f32>(timing.ms));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f%%", (gpuTotal > 0.0) ? static_cast<f32>(timing.ms * 100.0 / gpuTotal) : 0.0f);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Total");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<f32>(gpuTotal));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", (gpuTotal > 0.0) ? "100.0%" : "0.0%");

            ImGui::EndTable();
        }
        const f32 culledPct =
            (g_Stats.cpuFrustumCullTotalInstances > 0) ? (100.0f * static_cast<f32>(g_Stats.cpuFrustumCullRasterCulled) / static_cast<f32>(g_Stats.cpuFrustumCullTotalInstances)) : 0.0f;
        ImGui::Text("Raster Instances: %u submitted / %u total (%u culled, %.1f%%)", g_Stats.cpuFrustumCullRasterSubmitted, g_Stats.cpuFrustumCullTotalInstances,
                    g_Stats.cpuFrustumCullRasterCulled, culledPct);
        ImGui::Text("Radiance Cache Memory: %.2f / %.2f MB (%.1f%%)", p.radianceCacheUsedMB, p.radianceCacheMaxMB,
                    (p.radianceCacheMaxMB > 0.0f) ? (p.radianceCacheUsedMB * 100.0f / p.radianceCacheMaxMB) : 0.0f);

        ImGui::End();
    }

    static bool showRenderTargets = false;

    Renderer& renderer = *p.renderer;
    String currentScene = renderer.GetCurrentSceneFile();
    bool sceneSwitchPending = renderer.HasPendingSceneSwitch();
    const String pendingScene = renderer.GetPendingSceneFile();
    static i32 selectedScene = 0;
    static String syncedCurrentScene;

    const Vector<String>& scenes = renderer.GetAvailableScenes();
    if (!scenes.empty())
    {
        if (selectedScene < 0 || selectedScene >= static_cast<i32>(scenes.size()))
            selectedScene = 0;

        auto syncSceneSelection = [&](const String& sceneName) {
            for (i32 i = 0; i < static_cast<i32>(scenes.size()); ++i)
            {
                if (scenes[static_cast<size_t>(i)] == sceneName)
                {
                    selectedScene = i;
                    break;
                }
            }
        };

        if (sceneSwitchPending && !pendingScene.empty() && syncedCurrentScene != pendingScene)
        {
            syncSceneSelection(pendingScene);
            syncedCurrentScene = pendingScene;
        }
        else if (!sceneSwitchPending && syncedCurrentScene != currentScene)
        {
            syncSceneSelection(currentScene);
            syncedCurrentScene = currentScene;
        }
    }

    ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, topToolbarHeight), ImGuiCond_Always);
    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("##TopToolbar", nullptr, toolbarFlags);
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);

        auto drawToolbarToggle = [&](const char* label, bool& enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(enabled ? ImGuiCol_ButtonActive : ImGuiCol_Button));
            if (ImGui::Button(label))
            {
                enabled = !enabled;
            }
            ImGui::PopStyleColor();
        };
        auto setExclusiveRightPanel = [&](bool& enabled) {
            const bool shouldEnable = !enabled;
            showViewWindow = false;
            showEnvironmentWindow = false;
            showPathTraceWindow = false;
            showTonemapWindow = false;
            showDebugWindow = false;
            showRenderTargets = false;
            enabled = shouldEnable;
        };
        auto drawExclusiveToolbarToggle = [&](const char* label, bool& enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(enabled ? ImGuiCol_ButtonActive : ImGuiCol_Button));
            if (ImGui::Button(label))
            {
                setExclusiveRightPanel(enabled);
            }
            ImGui::PopStyleColor();
        };

        drawToolbarToggle("Stats", showStatsWindow);
        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Scene");
        ImGui::SameLine();

        if (!scenes.empty())
        {
            const char* preview = scenes[static_cast<size_t>(selectedScene)].c_str();
            ImGui::SetNextItemWidth(360.0f);
            ImGui::BeginDisabled(sceneSwitchPending);
            if (ImGui::BeginCombo("##SceneToolbar", preview, ImGuiComboFlags_HeightLargest))
            {
                Vector<i32> showcaseSceneIndices;
                Vector<i32> testingSceneIndices;
                showcaseSceneIndices.reserve(scenes.size());
                testingSceneIndices.reserve(scenes.size());
                bool sceneSelectionChanged = false;

                for (i32 i = 0; i < static_cast<i32>(scenes.size()); ++i)
                {
                    const String& sceneName = scenes[static_cast<size_t>(i)];
                    if (IsShowcaseScene(sceneName))
                    {
                        showcaseSceneIndices.push_back(i);
                    }
                    else
                    {
                        testingSceneIndices.push_back(i);
                    }
                }

                auto drawSceneSection = [&](const char* title, const Vector<i32>& sceneIndices) {
                    if (sceneIndices.empty())
                        return;

                    ImGui::SeparatorText(title);
                    for (i32 sceneIndex : sceneIndices)
                    {
                        const String& sceneName = scenes[static_cast<size_t>(sceneIndex)];
                        const String sceneSizeLabel = FormatSceneFileSize(sceneName);
                        const bool selected = (sceneIndex == selectedScene);
                        ImGui::PushID(sceneIndex);
                        if (ImGui::Selectable(sceneName.c_str(), selected) && !selected)
                        {
                            selectedScene = sceneIndex;
                            sceneSelectionChanged = true;
                        }

                        const ImVec2 itemMax = ImGui::GetItemRectMax();
                        const ImVec2 framePadding = ImGui::GetStyle().FramePadding;
                        const float sizeTextWidth = ImGui::CalcTextSize(sceneSizeLabel.c_str()).x;
                        const float sizeRight = itemMax.x - framePadding.x;
                        const float textY = ImGui::GetItemRectMin().y + framePadding.y;

                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                        const ImU32 sizeColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                        drawList->AddText(ImVec2(sizeRight - sizeTextWidth, textY), sizeColor, sceneSizeLabel.c_str());

                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                };

                drawSceneSection("Showcase", showcaseSceneIndices);
                drawSceneSection("Testing", testingSceneIndices);
                ImGui::EndCombo();

                if (sceneSelectionChanged && !sceneSwitchPending)
                {
                    renderer.RequestSceneSwitch(scenes[static_cast<size_t>(selectedScene)]);
                    sceneSwitchPending = renderer.HasPendingSceneSwitch();
                    syncedCurrentScene = scenes[static_cast<size_t>(selectedScene)];
                }
            }
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Environment Preset");
        ImGui::SameLine();

        const Vector<String>& environmentNames = renderer.GetEnvironmentNames();
        if (!environmentNames.empty())
        {
            i32 currentEnvironmentIndex = renderer.GetCurrentEnvironmentIndex();
            if (currentEnvironmentIndex < 0 || currentEnvironmentIndex >= static_cast<i32>(environmentNames.size()))
            {
                currentEnvironmentIndex = 0;
            }

            ImGui::SetNextItemWidth(240.0f);
            const char* preview = environmentNames[static_cast<size_t>(currentEnvironmentIndex)].c_str();
            if (ImGui::BeginCombo("##EnvironmentToolbar", preview, ImGuiComboFlags_HeightLargest))
            {
                for (i32 i = 0; i < static_cast<i32>(environmentNames.size()); ++i)
                {
                    const bool isSelected = (i == currentEnvironmentIndex);
                    const String& environmentName = environmentNames[static_cast<size_t>(i)];
                    if (ImGui::Selectable(environmentName.c_str(), isSelected) && !isSelected)
                    {
                        currentEnvironmentIndex = i;
                        renderer.SetCurrentEnvironmentIndex(currentEnvironmentIndex);
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("No environment presets available");
        }

        if (scenes.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("No scenes found in data/scenes");
        }

        ImGui::SameLine();
        if (ImGui::Button("Reload Shaders"))
        {
            renderer.RequestShaderReload();
        }
        if (!g_Stats.shadersCompilationSuccess)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Shader Reload Failed");
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        auto toggleButtonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
        const f32 viewButtonWidth = toggleButtonWidth("View");
        const f32 environmentButtonWidth = toggleButtonWidth("Environment");
        const f32 rayTracingButtonWidth = toggleButtonWidth("Ray Tracing");
        const f32 tonemapButtonWidth = toggleButtonWidth("Tone Mapping");
        const f32 debugButtonWidth = toggleButtonWidth("Debug");
        const f32 renderTargetsWidth = toggleButtonWidth("Texture Debug");
        const f32 buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
        char toolbarStats[160];
        std::snprintf(toolbarStats, sizeof(toolbarStats), "FPS %.1f | Position %.1f %.1f %.1f | Yaw %.1f | Pitch %.1f", displayedFps, p.frame.cameraPos[0],
                      p.frame.cameraPos[1], p.frame.cameraPos[2],
                      p.frame.cameraYaw, p.frame.cameraPitch);
        const f32 toolbarStatsWidth = ImGui::CalcTextSize(toolbarStats).x;
        const f32 buttonBlockWidth = viewButtonWidth + environmentButtonWidth + rayTracingButtonWidth + tonemapButtonWidth + debugButtonWidth + renderTargetsWidth + buttonSpacing * 5.0f;
        const f32 toolbarStatsX = ImGui::GetWindowContentRegionMax().x - (buttonBlockWidth + buttonSpacing + toolbarStatsWidth);
        if (toolbarStatsX > ImGui::GetCursorPosX())
        {
            ImGui::SameLine(toolbarStatsX);
        }
        else
        {
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(toolbarStats);
        ImGui::SameLine();

        drawExclusiveToolbarToggle("View", showViewWindow);
        ImGui::SameLine();
        drawExclusiveToolbarToggle("Environment", showEnvironmentWindow);
        ImGui::SameLine();
        drawExclusiveToolbarToggle("Ray Tracing", showPathTraceWindow);
        ImGui::SameLine();
        drawExclusiveToolbarToggle("Tone Mapping", showTonemapWindow);
        ImGui::SameLine();
        drawExclusiveToolbarToggle("Debug", showDebugWindow);
        ImGui::SameLine();
        drawExclusiveToolbarToggle("Texture Debug", showRenderTargets);
    }
    ImGui::End();

    const bool showLoadingPopup = (p.loadingLabel != nullptr) || sceneSwitchPending;
    if (showLoadingPopup)
    {
        const char* pendingSceneLabel = "scene";
        if (p.loadingLabel != nullptr && p.loadingLabel[0] != '\0')
        {
            pendingSceneLabel = p.loadingLabel;
        }
        else if (!pendingScene.empty())
        {
            pendingSceneLabel = pendingScene.c_str();
        }
        else if (!scenes.empty() && selectedScene >= 0 && selectedScene < static_cast<i32>(scenes.size()))
        {
            pendingSceneLabel = scenes[static_cast<size_t>(selectedScene)].c_str();
        }

        char loadingText[512];
        std::snprintf(loadingText, sizeof(loadingText), "Loading %s...", pendingSceneLabel);

        ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 textSize = ImGui::CalcTextSize(loadingText);
        const ImVec2 padding(style.WindowPadding.x * 1.5f, style.WindowPadding.y * 1.25f);
        const ImVec2 panelSize(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        const ImVec2 panelMin(vp->Pos.x + (vp->Size.x - panelSize.x) * 0.5f, vp->Pos.y + (vp->Size.y - panelSize.y) * 0.5f);
        const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
        const ImVec2 textPos(panelMin.x + padding.x, panelMin.y + padding.y);
        const ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_PopupBg);
        const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

        foregroundDrawList->AddRectFilled(panelMin, panelMax, bgColor, style.PopupRounding);
        if (style.PopupBorderSize > 0.0f)
        {
            foregroundDrawList->AddRect(panelMin, panelMax, borderColor, style.PopupRounding, 0, style.PopupBorderSize);
        }
        foregroundDrawList->AddText(textPos, textColor, loadingText);
    }

    ImGuiWindowFlags panelWindowFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar;
    const f32 pinnedPanelTop = vp->Pos.y + topToolbarHeight;
    const f32 pinnedPanelRight = vp->Pos.x + vp->Size.x;
    const f32 pinnedPanelMaxHeight = IE_Max(240.0f, vp->Size.y - topToolbarHeight);
    auto setPinnedPanelLayout = [&](f32 maxWidth, const char* widthHint, f32 widgetWidth) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 labelWidth = (widthHint && widthHint[0]) ? (ImGui::CalcTextSize(widthHint).x + style.FramePadding.x * 2.0f) : 0.0f;
        const f32 contentWidth = IE_Min(maxWidth, labelWidth + widgetWidth + style.CellPadding.x * 4.0f);
        ImGui::SetNextWindowPos(ImVec2(pinnedPanelRight, pinnedPanelTop), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(maxWidth, pinnedPanelMaxHeight));
        ImGui::SetNextWindowContentSize(ImVec2(contentWidth, 0.0f));
    };
    auto beginSettingsTable = [&](const char* id, const char* widthHint, f32 widgetWidth) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 labelWidth = (widthHint && widthHint[0]) ? (ImGui::CalcTextSize(widthHint).x + style.FramePadding.x * 2.0f) : 0.0f;
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
            return false;
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, widgetWidth);
        return true;
    };
    auto settingsRow = [&](const char* label, auto&& widgetFn) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        return widgetFn();
    };

    if (showViewWindow)
    {
        setPinnedPanelLayout(520.0f, "Frustum Culling Horizontal FOV", 220.0f);
        ImGui::Begin("##View", nullptr, panelWindowFlags);

        ImGui::SeparatorText("Camera");
        const f32 aspectRatio = IE_Max(renderer.GetWindow().GetAspectRatio(), 1e-4f);
        f32 horizontalFov = HorizontalFovFromVerticalDegrees(g_Settings.cameraFov, aspectRatio);
        if (beginSettingsTable("##ViewCameraTbl", "Frustum Culling Horizontal FOV", 220.0f))
        {
            if (settingsRow("Horizontal FOV", [&] { return ImGui::SliderFloat("##ViewHorizontalFov", &horizontalFov, 10.0f, 120.0f); }))
            {
                g_Settings.cameraFov = VerticalFovFromHorizontalDegrees(horizontalFov, aspectRatio);
            }
            f32 horizontalFrustumCullFov = HorizontalFovFromVerticalDegrees(g_Settings.cameraFrustumCullingFov, aspectRatio);
            if (settingsRow("Frustum Culling Horizontal FOV", [&] { return ImGui::SliderFloat("##ViewFrustumCullHorizontalFov", &horizontalFrustumCullFov, 10.0f, 120.0f); }))
            {
                g_Settings.cameraFrustumCullingFov = VerticalFovFromHorizontalDegrees(horizontalFrustumCullFov, aspectRatio);
            }
            settingsRow("CPU Frustum Culling", [&] { return ImGui::Checkbox("##ViewCpuFrustumCulling", &g_Settings.cpuFrustumCulling); });
            settingsRow("GPU Frustum Culling", [&] { return ImGui::Checkbox("##ViewGpuFrustumCulling", &g_Settings.gpuFrustumCulling); });
            settingsRow("GPU Backface Culling", [&] { return ImGui::Checkbox("##ViewGpuBackfaceCulling", &g_Settings.gpuBackfaceCulling); });
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Upscaling");
        const DLSS::Mode dlssModeValues[] = {
            DLSS::Mode::Disabled, DLSS::Mode::DLAA, DLSS::Mode::Quality, DLSS::Mode::Balanced, DLSS::Mode::Performance, DLSS::Mode::UltraPerformance,
        };
        const char* dlssModeLabels[] = {"Disabled", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
        const DLSS::Mode activeDLSSMode = renderer.GetDLSSMode();
        i32 currentDLSSMode = 0;
        for (i32 i = 0; i < static_cast<i32>(IM_ARRAYSIZE(dlssModeValues)); ++i)
        {
            if (dlssModeValues[i] == activeDLSSMode)
            {
                currentDLSSMode = i;
                break;
            }
        }
        if (beginSettingsTable("##ViewUpscalingTbl", "DLSS Mode", 220.0f))
        {
            if (settingsRow("DLSS Mode", [&] { return ImGui::Combo("##ViewDlssMode", &currentDLSSMode, dlssModeLabels, IM_ARRAYSIZE(dlssModeLabels)); }))
            {
                renderer.RequestDLSSMode(dlssModeValues[currentDLSSMode]);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    if (showEnvironmentWindow)
    {
        setPinnedPanelLayout(700.0f, "Ray-Traced Indirect Diffuse Strength", 240.0f);
        ImGui::Begin("##Environment", nullptr, panelWindowFlags);

        ImGui::SeparatorText("Sun");
        if (beginSettingsTable("##EnvironmentSunTbl", "Ray-Traced Indirect Diffuse Strength", 240.0f))
        {
            settingsRow("Azimuth", [&] { return ImGui::SliderAngle("##EnvironmentSunAzimuth", &g_Settings.sunAzimuth, 0.0f, 360.0f); });
            settingsRow("Elevation", [&] { return ImGui::SliderAngle("##EnvironmentSunElevation", &g_Settings.sunElevation, 180.0f, 360.0f); });
            settingsRow("Intensity", [&] { return ImGui::SliderFloat("##EnvironmentSunIntensity", &g_Settings.sunIntensity, 0.f, 10.f); });
            settingsRow("Ambient Strength", [&] { return ImGui::SliderFloat("##EnvironmentAmbientStrength", &g_Settings.ambientStrength, 0.0f, 0.05f, "%.3f"); });
            settingsRow("Shadow Minimum Visibility", [&] { return ImGui::SliderFloat("##EnvironmentShadowMinVisibility", &g_Settings.shadowMinVisibility, 0.0f, 0.1f, "%.3f"); });
            settingsRow("Ray-Traced Indirect Diffuse Strength",
                        [&] { return ImGui::SliderFloat("##EnvironmentRTIndirectDiffuseStrength", &g_Settings.rtIndirectDiffuseStrength, 0.0f, 2.0f, "%.3f"); });
            settingsRow("Exposure Compensation (EV)", [&] { return ImGui::SliderFloat("##EnvironmentExposureCompensationEV", &g_Settings.toneMappingExposureCompensationEV, -4.0f, 4.0f, "%.2f"); });
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Sky Lighting");
        if (beginSettingsTable("##EnvironmentSkyLightingTbl", "Sky Intensity", 240.0f))
        {
            settingsRow("Sky Intensity", [&] { return ImGui::SliderFloat("##EnvironmentSkyIntensity", &g_Settings.skyIntensity, 0.f, 10.f); });
            ImGui::EndTable();
        }

        Environment& env = renderer.GetCurrentEnvironment();
        Environment::SkySettings& sky = env.sky;

        ImGui::SeparatorText("Sun Disk");
        if (beginSettingsTable("##EnvironmentSunDiskTbl", "Disk Intensity Scale", 240.0f))
        {
            settingsRow("Sun Color", [&] { return ImGui::ColorEdit3("##EnvironmentSunColor", &sky.sunColor.x); });
            settingsRow("Disk Angle (Degrees)", [&] { return ImGui::SliderFloat("##EnvironmentSunDiskAngle", &sky.sunDiskAngleDeg, 0.001f, 8.0f, "%.3f"); });
            settingsRow("Disk Intensity Scale", [&] { return ImGui::SliderFloat("##EnvironmentSunDiskScale", &sky.sunDiskIntensityScale, 0.0f, 12.0f); });
            settingsRow("Glow Power", [&] { return ImGui::SliderFloat("##EnvironmentSunGlowPower", &sky.sunGlowPower, 4.0f, 128.0f); });
            settingsRow("Glow Intensity", [&] { return ImGui::SliderFloat("##EnvironmentSunGlowIntensity", &sky.sunGlowIntensity, 0.0f, 1.0f); });
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Atmosphere");
        Environment::SkySettings::AtmosphereSettings& atmosphere = sky.atmosphere;
        if (beginSettingsTable("##EnvironmentAtmosphereTbl", "Atmosphere Sun Intensity Scale", 240.0f))
        {
            settingsRow("Rayleigh Scattering", [&] { return ImGui::SliderFloat3("##EnvironmentAtmosphereRayleighScattering", &atmosphere.rayleighScattering.x, 0.0f, 0.08f, "%.4f"); });
            settingsRow("Rayleigh Scale Height (km)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereRayleighScaleHeight", &atmosphere.rayleighScaleHeightKm, 0.1f, 20.0f); });
            settingsRow("Mie Scattering", [&] { return ImGui::SliderFloat3("##EnvironmentAtmosphereMieScattering", &atmosphere.mieScattering.x, 0.0f, 0.08f, "%.4f"); });
            settingsRow("Mie Scale Height (km)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereMieScaleHeight", &atmosphere.mieScaleHeightKm, 0.05f, 8.0f); });
            settingsRow("Mie Anisotropy (g)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereMieG", &atmosphere.mieG, -0.98f, 0.98f); });
            settingsRow("Atmosphere Thickness (km)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereThickness", &atmosphere.atmosphereThicknessKm, 1.0f, 200.0f); });
            settingsRow("Atmosphere Sun Intensity Scale", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereSunScale", &atmosphere.sunIntensityScale, 0.0f, 8.0f); });
            settingsRow("Ozone Absorption", [&] { return ImGui::SliderFloat3("##EnvironmentAtmosphereOzoneAbsorption", &atmosphere.ozoneAbsorption.x, 0.0f, 0.01f, "%.5f"); });
            settingsRow("Ozone Layer Center (km)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereOzoneLayerCenter", &atmosphere.ozoneLayerCenterKm, 5.0f, 60.0f); });
            settingsRow("Ozone Layer Width (km)", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereOzoneLayerWidth", &atmosphere.ozoneLayerWidthKm, 1.0f, 30.0f); });
            settingsRow("Multi-Scattering Strength", [&] { return ImGui::SliderFloat("##EnvironmentAtmosphereMultiScattering", &atmosphere.multiScatteringStrength, 0.0f, 2.0f); });
            ImGui::EndTable();
        }
        ImGui::End();
    }

    if (showPathTraceWindow)
    {
        setPinnedPanelLayout(760.0f, "Minimum Samples for Present", 240.0f);
        ImGui::Begin("##RayTracing", nullptr, panelWindowFlags);

        ImGui::SeparatorText("Path Tracing");
        if (beginSettingsTable("##RayTracingPathTracingTbl", "Samples Per Pixel", 240.0f))
        {
            {
                const u32 minv = 0, maxv = 8;
                settingsRow("Samples Per Pixel", [&] { return ImGui::SliderScalar("##RayTracingSpp", ImGuiDataType_U32, &g_Settings.pathTraceSpp, &minv, &maxv); });
            }
            {
                const u32 minv = 0, maxv = 8;
                settingsRow("Bounce Count", [&] { return ImGui::SliderScalar("##RayTracingBounceCount", ImGuiDataType_U32, &g_Settings.pathTraceBounceCount, &minv, &maxv); });
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Radiance Cache");
        if (beginSettingsTable("##RayTracingRadianceCacheTbl", "Soft Normal Interpolation", 240.0f))
        {
            {
                const u32 minv = 0u, maxv = 256u;
                settingsRow("Minimum Extra Samples",
                            [&] { return ImGui::SliderScalar("##RayTracingRcMinExtraSppCount", ImGuiDataType_U32, &g_Settings.radianceCacheMinExtraSppCount, &minv, &maxv); });
            }
            {
                const u32 minv = 1, maxv = 64;
                settingsRow("Normal Bin Resolution",
                            [&] { return ImGui::SliderScalar("##RayTracingRcNormalBinRes", ImGuiDataType_U32, &g_Settings.radianceCacheNormalBinRes, &minv, &maxv); });
            }
            {
                const u32 minv = 0u, maxv = 64u;
                settingsRow("Maximum Probes", [&] { return ImGui::SliderScalar("##RayTracingRcMaxProbes", ImGuiDataType_U32, &g_Settings.radianceCacheMaxProbes, &minv, &maxv); });
            }
            {
                const u32 minv = 1u, maxv = 262144u;
                settingsRow("Maximum Samples", [&] { return ImGui::SliderScalar("##RayTracingRcMaxSamples", ImGuiDataType_U32, &g_Settings.radianceCacheMaxSamples, &minv, &maxv); });
            }
            settingsRow("Cell Size", [&] { return ImGui::SliderFloat("##RayTracingRcCellSize", &g_Settings.radianceCacheCellSize, 0.01f, 2.0f); });
            settingsRow("Trilinear Filtering", [&] { return ImGui::Checkbox("##RayTracingRcTrilinear", &g_Settings.radianceCacheTrilinear); });
            settingsRow("Soft Normal Interpolation", [&] { return ImGui::Checkbox("##RayTracingRcSoftNormalInterpolation", &g_Settings.radianceCacheSoftNormalInterpolation); });
            ImGui::BeginDisabled(!g_Settings.radianceCacheSoftNormalInterpolation);
            settingsRow("Soft Normal Dot Threshold", [&] { return ImGui::SliderFloat("##RayTracingRcSoftNormalMinDot", &g_Settings.radianceCacheSoftNormalMinDot, 0.0f, 0.9f, "%.2f"); });
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!g_Settings.radianceCacheTrilinear);
            {
                const u32 minv = 0, maxv = 4096;
                settingsRow("Minimum Corner Samples",
                            [&] { return ImGui::SliderScalar("##RayTracingRcMinCornerSamples", ImGuiDataType_U32, &g_Settings.radianceCacheTrilinearMinCornerSamples, &minv, &maxv); });
            }
            {
                const u32 minv = 0, maxv = 8;
                settingsRow("Minimum Hits", [&] { return ImGui::SliderScalar("##RayTracingRcMinHits", ImGuiDataType_U32, &g_Settings.radianceCacheTrilinearMinHits, &minv, &maxv); });
            }
            {
                const u32 minv = 0, maxv = 4096;
                settingsRow("Minimum Samples for Present",
                            [&] { return ImGui::SliderScalar("##RayTracingRcPresentMinSamples", ImGuiDataType_U32, &g_Settings.radianceCacheTrilinearPresentMinSamples, &minv, &maxv); });
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Ray-Traced Shadows");
        const char* rtResLabels[] = {"Full Resolution", "Full Width / Half Height", "Half Resolution", "Quarter Resolution"};
        i32 currentRTRes = static_cast<i32>(g_Settings.rtShadowsResolution);
        if (beginSettingsTable("##RayTracingShadowsTbl", "Resolution", 240.0f))
        {
            settingsRow("Enabled", [&] { return ImGui::Checkbox("##RayTracingShadowsEnabled", &g_Settings.rtShadowsEnabled); });
            if (settingsRow("Resolution", [&] { return ImGui::Combo("##RayTracingResolution", &currentRTRes, rtResLabels, IM_ARRAYSIZE(rtResLabels)); }))
            {
                g_Settings.rtShadowsResolution = static_cast<RayTracingResolution>(currentRTRes);
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Ray-Traced Specular");
        i32 currentRTSpecRes = static_cast<i32>(g_Settings.rtSpecularResolution);
        if (beginSettingsTable("##RayTracingSpecularTbl", "Maximum Samples Per Pixel", 240.0f))
        {
            settingsRow("Enabled", [&] { return ImGui::Checkbox("##RayTracingSpecularEnabled", &g_Settings.rtSpecularEnabled); });
            if (settingsRow("Resolution", [&] { return ImGui::Combo("##RayTracingSpecularResolution", &currentRTSpecRes, rtResLabels, IM_ARRAYSIZE(rtResLabels)); }))
            {
                g_Settings.rtSpecularResolution = static_cast<RayTracingResolution>(currentRTSpecRes);
            }
            settingsRow("Strength", [&] { return ImGui::SliderFloat("##RayTracingSpecularStrength", &g_Settings.rtSpecularStrength, 0.0f, 2.0f); });
            {
                const u32 minv = 0u, maxv = 16u;
                settingsRow("Minimum Samples Per Pixel",
                            [&] { return ImGui::SliderScalar("##RayTracingSpecularSppMin", ImGuiDataType_U32, &g_Settings.rtSpecularSppMin, &minv, &maxv); });
                settingsRow("Maximum Samples Per Pixel",
                            [&] { return ImGui::SliderScalar("##RayTracingSpecularSppMax", ImGuiDataType_U32, &g_Settings.rtSpecularSppMax, &minv, &maxv); });
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    if (showTonemapWindow)
    {
        setPinnedPanelLayout(620.0f, "High Reject Percentile", 220.0f);
        ImGui::Begin("##Tonemap", nullptr, panelWindowFlags);

        ImGui::SeparatorText("Tone Mapping");
        if (beginSettingsTable("##TonemapToneMappingTbl", "Saturation", 220.0f))
        {
            settingsRow("Contrast", [&] { return ImGui::SliderFloat("##TonemapContrast", &g_Settings.toneMappingContrast, 0.6f, 1.4f); });
            settingsRow("Saturation", [&] { return ImGui::SliderFloat("##TonemapSaturation", &g_Settings.toneMappingSaturation, 0.0f, 2.0f); });
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Bloom");
        if (beginSettingsTable("##TonemapBloomTbl", "Soft Knee", 220.0f))
        {
            settingsRow("Enabled", [&] { return ImGui::Checkbox("##TonemapBloomEnabled", &g_Settings.bloomEnabled); });
            settingsRow("Intensity", [&] { return ImGui::SliderFloat("##TonemapBloomIntensity", &g_Settings.bloomIntensity, 0.0f, 2.0f, "%.2f"); });
            settingsRow("Threshold", [&] { return ImGui::SliderFloat("##TonemapBloomThreshold", &g_Settings.bloomThreshold, 0.0f, 5.0f, "%.2f"); });
            settingsRow("Soft Knee", [&] { return ImGui::SliderFloat("##TonemapBloomSoftKnee", &g_Settings.bloomSoftKnee, 0.0f, 1.0f, "%.2f"); });
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Auto Exposure");
        if (beginSettingsTable("##TonemapAutoExposureTbl", "High Reject Percentile", 220.0f))
        {
            settingsRow("Target Percentile", [&] { return ImGui::SliderFloat("##TonemapAutoExposureTargetPct", &g_Settings.autoExposureTargetPct, 0.0f, 1.0f, "%.2f"); });
            settingsRow("Low Reject Percentile", [&] { return ImGui::SliderFloat("##TonemapAutoExposureLowReject", &g_Settings.autoExposureLowReject, 0.0f, 0.2f, "%.2f"); });
            settingsRow("High Reject Percentile", [&] { return ImGui::SliderFloat("##TonemapAutoExposureHighReject", &g_Settings.autoExposureHighReject, 0.8f, 1.0f, "%.2f"); });
            settingsRow("Middle Gray (Key)", [&] { return ImGui::SliderFloat("##TonemapAutoExposureKey", &g_Settings.autoExposureKey, 0.05f, 0.50f, "%.2f"); });
            settingsRow("Minimum Log Luminance", [&] { return ImGui::SliderFloat("##TonemapAutoExposureMinLogLum", &g_Settings.autoExposureMinLogLum, -16.0f, 0.0f, "%.1f"); });
            settingsRow("Maximum Log Luminance", [&] { return ImGui::SliderFloat("##TonemapAutoExposureMaxLogLum", &g_Settings.autoExposureMaxLogLum, 0.0f, 16.0f, "%.1f"); });
            settingsRow("Bright Scene Adapt Time (s)", [&] { return ImGui::SliderFloat("##TonemapAutoExposureTauBright", &g_Settings.autoExposureTauBright, 0.05f, 0.5f, "%.2f"); });
            settingsRow("Dark Scene Adapt Time (s)", [&] { return ImGui::SliderFloat("##TonemapAutoExposureTauDark", &g_Settings.autoExposureTauDark, 0.5f, 6.0f, "%.2f"); });
            settingsRow("Minimum Exposure Clamp", [&] { return ImGui::SliderFloat("##TonemapAutoExposureClampMin", &g_Settings.autoExposureClampMin, 1.0f / 256.0f, 1.0f, "%.5f"); });
            settingsRow("Maximum Exposure Clamp", [&] { return ImGui::SliderFloat("##TonemapAutoExposureClampMax", &g_Settings.autoExposureClampMax, 1.0f, 256.0f, "%.1f"); });
            ImGui::EndTable();
        }
        ImGui::End();
    }

    if (showDebugWindow)
    {
        setPinnedPanelLayout(520.0f, "Lighting Debug View", 220.0f);
        ImGui::Begin("##Debug", nullptr, panelWindowFlags);

        const char* lightingDebugLabels[] = {"None", "Indirect Diffuse", "Surface Normals"};
        i32 lightingDebugMode = static_cast<i32>(g_Settings.lightingDebugMode);
        if (beginSettingsTable("##DebugTbl", "Lighting Debug View", 220.0f))
        {
            settingsRow("Meshlet Debug Colors", [&] { return ImGui::Checkbox("##DebugMeshletColor", &g_Settings.debugMeshletColor); });
            if (settingsRow("Lighting Debug View", [&] { return ImGui::Combo("##DebugLightingMode", &lightingDebugMode, lightingDebugLabels, IM_ARRAYSIZE(lightingDebugLabels)); }))
            {
                g_Settings.lightingDebugMode = static_cast<u32>(lightingDebugMode);
            }
            settingsRow("Move Test Instances", [&] { return ImGui::Checkbox("##DebugMoveInstances", &g_Settings.testMoveInstances); });
            settingsRow("Motion Amplitude", [&] { return ImGui::SliderFloat("##DebugMoveAmplitude", &g_Settings.testMoveInstancesAmplitude, 0.0f, 5.0f, "%.2f"); });
            settingsRow("Motion Speed", [&] { return ImGui::SliderFloat("##DebugMoveSpeed", &g_Settings.testMoveInstancesSpeed, 0.0f, 5.0f, "%.2f"); });
            const u32 minv = 0, maxv = 1024;
            settingsRow("Instance Count (0 = All)", [&] { return ImGui::SliderScalar("##DebugMoveCount", ImGuiDataType_U32, &g_Settings.testMoveInstancesCount, &minv, &maxv); });
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Texture debug window: select one texture from a dropdown and preview it.
    if (showRenderTargets)
    {
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
        maybe_add("Shading Normal", p.gbufferNormal);
        maybe_add("Geometric Normal", p.gbufferNormalGeo);
        maybe_add("Metallic / Roughness", p.gbufferMaterial);
        maybe_add("Motion Vectors", p.gbufferMotion);
        maybe_add("Ambient Occlusion", p.gbufferAO);
        maybe_add("Emissive", p.gbufferEmissive);
        maybe_add("Scene Depth", p.depth);
        maybe_add("Ray-Traced Shadows", p.rtShadows);
        maybe_add("Ray-Traced Indirect Diffuse", p.rtIndirectDiffuse);
        maybe_add("Ray-Traced Specular", p.rtSpecular);
        if (!labels.empty())
        {
            auto getSrv = [&](ID3D12Resource* res) -> D3D12_GPU_DESCRIPTOR_HANDLE {
                auto it = gDebugSrvCache.find(res);
                if (it != gDebugSrvCache.end())
                    return it->second;

                if (gDebugSrvCount >= kImGuiDebugSrvReserve)
                {
                    return D3D12_GPU_DESCRIPTOR_HANDLE{}; // out of descriptors
                }
                const u32 debugIndex = kImGuiTotalSrvDescriptors - 1 - gDebugSrvCount;
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = GetCpuHandle(debugIndex);
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = GetGpuHandle(debugIndex);
                gDebugSrvCount++;

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

                gDebugSrvCache.emplace(res, gpu);
                return gpu;
            };

            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + topToolbarHeight), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x, IE_Max(0.0f, vp->Size.y - topToolbarHeight)), ImGuiCond_Always);

            ImGui::SetNextWindowBgAlpha(1.0f);
            ImGui::Begin("##RenderTargets", nullptr,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

            static i32 selected = 0;
            if (selected < 0 || selected >= static_cast<i32>(labels.size()))
                selected = 0;
            const char* selectedLabel = labels[static_cast<size_t>(selected)];
            if (ImGui::BeginCombo("Debug Texture", selectedLabel, ImGuiComboFlags_HeightLargest))
            {
                for (i32 i = 0; i < static_cast<i32>(labels.size()); ++i)
                {
                    const bool isSelected = (i == selected);
                    if (ImGui::Selectable(labels[static_cast<size_t>(i)], isSelected))
                    {
                        selected = i;
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ID3D12Resource* res = textures[selected];
            D3D12_GPU_DESCRIPTOR_HANDLE h = getSrv(res);

            const f32 avail = ImGui::GetContentRegionAvail().x;
            const D3D12_RESOURCE_DESC textureDesc = res->GetDesc();
            const f32 aspect = (textureDesc.Width > 0 && textureDesc.Height > 0) ? static_cast<f32>(textureDesc.Height) / static_cast<f32>(textureDesc.Width) : 1.0f;
            const f32 previewScale = 0.91f;
            ImVec2 sz(avail * previewScale, avail * previewScale * aspect);

            if (h.ptr)
                ImGui::Image(h.ptr, sz);
            else
                ImGui::TextUnformatted("No descriptor available");

            ImGui::End();
        }
    }

    ImGui::Render();

    p.cmd->OMSetRenderTargets(1, &p.rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {gSrvHeap};
    p.cmd->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), p.cmd);
}
