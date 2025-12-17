// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "LoadShader.h"

#include "ImGui.h"
#include "Renderer.h"
#include "common/Asserts.h"
#include "common/Log.h"
#include "common/ToUtf8.h"

#include <algorithm>
#include <format>

namespace
{
struct ShaderCompileResult
{
    ComPtr<IDxcBlob> blob;
    String errorLog;
    bool ok : 1;
};

LPCWSTR ToTargetName(const ShaderType type)
{
    switch (type)
    {
    case IE_SHADER_TYPE_VERTEX:
        return L"vs_6_7";
    case IE_SHADER_TYPE_PIXEL:
        return L"ps_6_7";
    case IE_SHADER_TYPE_COMPUTE:
        return L"cs_6_6";
    case IE_SHADER_TYPE_MESH:
        return L"ms_6_7";
    case IE_SHADER_TYPE_AMPLIFICATION:
        return L"as_6_7";
    case IE_SHADER_TYPE_LIB:
        return L"lib_6_7";
    }
    return L"undefined";
}

class IncludeHandler : public IDxcIncludeHandler
{
  public:
    IncludeHandler() = default;
    virtual ~IncludeHandler() = default;

    IncludeHandler(const IncludeHandler&) = delete;
    IncludeHandler& operator=(const IncludeHandler&) = delete;
    IncludeHandler(IncludeHandler&&) = delete;
    IncludeHandler& operator=(IncludeHandler&&) = delete;

    HRESULT LoadSource(const LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override
    {
        if (!m_Utils)
        {
            IE_Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_Utils)));
        }

        IDxcBlobEncoding* pEncoding;
        for (const WString& includedFile : m_IncludedFiles)
        {
            if (includedFile == pFilename)
            {
                constexpr char nullStr[] = "";
                IE_Check(m_Utils->CreateBlobFromPinned(nullStr, ARRAYSIZE(nullStr), DXC_CP_ACP, &pEncoding));
                *ppIncludeSource = pEncoding;
                return S_OK;
            }
        }

        const HRESULT hr = m_Utils->LoadFile(pFilename, nullptr, &pEncoding);
        if (SUCCEEDED(hr))
        {
            m_IncludedFiles.push_back(pFilename);
            *ppIncludeSource = pEncoding;
        }
        return hr;
    }

    HRESULT QueryInterface(REFIID riid, void** ppvObject) override
    {
        return E_NOINTERFACE;
    }

    ULONG AddRef() override
    {
        return 0;
    }

    ULONG Release() override
    {
        return 0;
    }

  private:
    Vector<WString> m_IncludedFiles;
    ComPtr<IDxcUtils> m_Utils;
};

ShaderCompileResult Fail(HRESULT hr, const char* context, ShaderCompileResult& result)
{
    LPSTR errorMessage = nullptr;
    String msg;

    if (FAILED(FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&errorMessage), 0, nullptr)))
    {
        msg = "Unknown error.";
    }
    else
    {
        msg = errorMessage;
        LocalFree(errorMessage);
    }

    result.ok = false;
    result.errorLog = String(context) + " failed with HRESULT 0x" + std::format("{:08X}", static_cast<unsigned long>(hr)) + " : " + msg;
    return result;
}

ShaderCompileResult CompileShader(ShaderType type, const WString& filename, const Vector<WString>& extraArguments)
{
    ShaderCompileResult result{};

    ComPtr<IDxcLibrary> library;
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hr))
    {
        return Fail(hr, "DxcCreateInstance(CLSID_DxcLibrary)", result);
    }

    ComPtr<IDxcCompiler3> compiler;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr))
    {
        return Fail(hr, "DxcCreateInstance(CLSID_DxcCompiler)", result);
    }

    u32 codePage = CP_UTF8;
    ComPtr<IDxcBlobEncoding> sourceBlob;

    const WString shaderPath = L"data/shaders/" + filename;
    hr = library->CreateBlobFromFile(shaderPath.data(), &codePage, &sourceBlob);
    if (FAILED(hr))
    {
        return Fail(hr, "CreateBlobFromFile", result);
    }

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = 0;

    // Arguments
    Vector<LPCWSTR> arguments;
    arguments.push_back(filename.data());
    switch (type)
    {
    case IE_SHADER_TYPE_AMPLIFICATION:
    case IE_SHADER_TYPE_MESH:
    case IE_SHADER_TYPE_PIXEL:
    case IE_SHADER_TYPE_VERTEX:
    case IE_SHADER_TYPE_COMPUTE:
        arguments.push_back(L"-E");
        arguments.push_back(L"main");
        break;
    case IE_SHADER_TYPE_LIB:
        break;
    }
    arguments.push_back(L"-T");
    arguments.push_back(ToTargetName(type));
    arguments.push_back(L"-I");
    arguments.push_back(L"data/shaders");
    for (const WString& extraArg : extraArguments)
    {
        arguments.push_back(extraArg.data());
    }
    arguments.push_back(L"-enable-16bit-types");
    arguments.push_back(L"-Zpr");
#ifdef _DEBUG
    arguments.push_back(L"-Zi");
    arguments.push_back(L"-Qembed_debug");
    // arguments.push_back(L"-Od"); // disable optimization if you want
#endif

    IncludeHandler includeHandler;

    ComPtr<IDxcResult> compileResult;
    hr = compiler->Compile(&sourceBuffer, arguments.data(), static_cast<u32>(arguments.size()), &includeHandler, IID_PPV_ARGS(&compileResult));
    if (FAILED(hr))
    {
        return Fail(hr, "IDxcCompiler3::Compile", result);
    }

    ComPtr<IDxcBlobUtf8> errors;
    hr = compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(hr))
    {
        return Fail(hr, "IDxcResult::GetOutput(DXC_OUT_ERRORS)", result);
    }

    if (errors && errors->GetStringLength() > 0)
    {
        const char* msgPtr = errors->GetStringPointer();
        size_t msgLen = errors->GetStringLength();
        result.errorLog.assign(msgPtr, msgPtr + msgLen);
    }

    HRESULT status = S_OK;
    hr = compileResult->GetStatus(&status);
    if (FAILED(hr))
    {
        return Fail(hr, "IDxcResult::GetStatus", result);
    }

    if (FAILED(status))
    {
        if (result.errorLog.empty())
        {
            result.errorLog = "Shader compilation failed with no diagnostic output.";
        }
        result.ok = false;
        return result;
    }

    ComPtr<IDxcBlob> code;
    hr = compileResult->GetResult(&code);
    if (FAILED(hr))
    {
        return Fail(hr, "IDxcResult::GetResult", result);
    }

    result.ok = true;
    result.blob = code;
    return result;
}
} // namespace

SharedPtr<Shader> IE_LoadShader(ShaderType type, const WString& filename, const Vector<WString>& defines, const SharedPtr<Shader>& oldShader)
{
    Vector<WString> definesStrings;
    for (const WString& define : defines)
    {
        definesStrings.push_back(L"-D" + define);
    }

    ShaderCompileResult r = CompileShader(type, filename, definesStrings);
    if (!r.ok)
    {
        IE_Error("Shader reload failed for {}:\n{}\n", IE_ToUtf8(filename), r.errorLog);
        IE_Assert(oldShader != nullptr);
        g_ShadersCompilationSuccess = false;
        return oldShader;
    }

    Shader dxShader;
    dxShader.blob = r.blob;
    dxShader.filename = filename;
    dxShader.defines = defines;
    return IE_MakeSharedPtr(dxShader);
}