// Iskur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Asserts.h"

#include <d3d12.h>
#include <dxgi.h>

#include "Log.h"

namespace
{
ID3D12Device*& DeviceRemovedReasonDeviceStorage()
{
    static ID3D12Device* device = nullptr;
    return device;
}

String BuildHrMessage(HRESULT hr)
{
    LPSTR errorMessage = nullptr;
    const DWORD formatResult = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                              reinterpret_cast<LPSTR>(&errorMessage), 0, nullptr);
    if (formatResult == 0 || errorMessage == nullptr)
    {
        return "Unknown error";
    }

    String message = errorMessage;
    LocalFree(errorMessage);

    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back())))
    {
        message.pop_back();
    }

    return message.empty() ? String("Unknown error") : message;
}

bool IsDeviceRemovalHr(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void LogDeviceRemovedReason(bool fatal)
{
    ID3D12Device* const device = DeviceRemovedReasonDeviceStorage();
    if (!device)
    {
        return;
    }

    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
    {
        return;
    }

    const String message = BuildHrMessage(reason);
    const unsigned long hrHex = static_cast<unsigned long>(reason);
    if (fatal)
    {
        IE_LogFatal("GetDeviceRemovedReason: HRESULT 0x{:08X} ({})", hrHex, message);
    }
    else
    {
        IE_LogError("GetDeviceRemovedReason: HRESULT 0x{:08X} ({})", hrHex, message);
    }
}

void LogHrFailure(HRESULT hr, bool fatal)
{
    const String message = BuildHrMessage(hr);
    const unsigned long hrHex = static_cast<unsigned long>(hr);
    if (fatal)
    {
        IE_LogFatal("HRESULT 0x{:08X} ({})", hrHex, message);
    }
    else
    {
        IE_LogError("HRESULT 0x{:08X} ({})", hrHex, message);
    }

    if (IsDeviceRemovalHr(hr))
    {
        LogDeviceRemovedReason(fatal);
    }
}
} // namespace

void IE_Assert(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

void IE_SetDeviceRemovedReasonDevice(ID3D12Device* const device)
{
    DeviceRemovedReasonDeviceStorage() = device;
}

bool IE_Try(HRESULT hr)
{
    if (FAILED(hr))
    {
        LogHrFailure(hr, false);
        return false;
    }

    return true;
}

void IE_Check(HRESULT hr)
{
    if (FAILED(hr))
    {
        LogHrFailure(hr, true);
        abort();
    }
}
