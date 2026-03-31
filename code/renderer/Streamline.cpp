// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Streamline.h"

#include <sl_helpers.h>

namespace
{
String NormalizeStreamlineLogMessage(const char* msg)
{
    String message = msg ? msg : "";

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
    {
        message.pop_back();
    }

    size_t first = 0;
    while (first < message.size() && (message[first] == ' ' || message[first] == '\t'))
    {
        ++first;
    }

    if (first < message.size() && message[first] == '[')
    {
        const size_t firstClose = message.find(']', first + 1);
        if (firstClose != String::npos && firstClose + 1 < message.size() && message[firstClose + 1] == '[')
        {
            const size_t secondOpen = firstClose + 1;
            const size_t secondClose = message.find(']', secondOpen + 1);
            if (secondClose != String::npos)
            {
                const String secondToken = message.substr(secondOpen + 1, secondClose - secondOpen - 1);
                if (secondToken == "streamline")
                {
                    size_t pos = first;
                    while (pos < message.size() && message[pos] == '[')
                    {
                        const size_t close = message.find(']', pos + 1);
                        if (close == String::npos)
                        {
                            break;
                        }
                        pos = close + 1;
                    }
                    while (pos < message.size() && (message[pos] == ' ' || message[pos] == '\t'))
                    {
                        ++pos;
                    }
                    message.erase(0, pos);
                }
            }
        }
    }

    return message;
}
} // namespace

void Streamline::CheckResult(const sl::Result result)
{
    if (result != sl::Result::eOk)
    {
        IE_LogError("Streamline call failed: {}", sl::getResultAsStr(result));
        IE_Assert(false);
    }
}

void Streamline::Init()
{
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = [](sl::LogType type, const char* msg) {
        const String message = NormalizeStreamlineLogMessage(msg);
        const String taggedMessage = message.empty() ? "[SL]" : std::format("[SL] {}", message);
        switch (type)
        {
        case sl::LogType::eInfo:
            IE_LogInfo("{}", taggedMessage);
            break;
        case sl::LogType::eWarn:
            IE_LogWarn("{}", taggedMessage);
            break;
        case sl::LogType::eError:
            IE_LogError("{}", taggedMessage);
            break;
        default:
            IE_LogDebug("{}", taggedMessage);
            break;
        }
    };

    // Keep Streamline setup explicit and prefer the DXGI proxy path for
    // compatibility with other injected overlays/tools.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "IskurEngine";
    pref.projectId = "211d89ab-9b6e-4e94-86e7-d2f238cb5cd8";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    static const sl::Feature features[] = {sl::kFeatureDLSS};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));

    CheckResult(slInit(pref, sl::kSDKVersion));
}

void Streamline::SetD3DDevice(ID3D12Device* device)
{
    CheckResult(slSetD3DDevice(device));
}

void Streamline::Shutdown()
{
    slShutdown();
}
