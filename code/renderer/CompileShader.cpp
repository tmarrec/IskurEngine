// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CompileShader.h"

#include "common/Asserts.h"
#include "common/Log.h"

#include <algorithm>

namespace
{
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
                constexpr char nullStr[] = " ";
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
} // namespace

ComPtr<IDxcBlob> CompileShader(ShaderType type, const WString& filename, const Vector<WString>& extraArguments)
{
    IDxcLibrary* library;
    IE_Check(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
    IE_Assert(library);

    IDxcCompiler3* compiler;
    IE_Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    IE_Assert(compiler);

    u32 codePage = CP_UTF8;
    IDxcBlobEncoding* sourceBlob;

    const WString shaderPath = L"data/shaders/" + filename;
    IE_Check(library->CreateBlobFromFile(shaderPath.data(), &codePage, &sourceBlob));
    IE_Assert(sourceBlob);

    const DxcBuffer sourceBuffer = {
        .Ptr = sourceBlob->GetBufferPointer(),
        .Size = sourceBlob->GetBufferSize(),
        .Encoding = 0,
    };

    // Add arguments
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
    arguments.push_back(L"-I shaders");
    for (const WString& extraArg : extraArguments)
    {
        arguments.push_back(extraArg.data());
    }
    arguments.push_back(L"-enable-16bit-types");
    arguments.push_back(L"-Zpr");

    IncludeHandler includeHandler;
    IDxcResult* compileResult;
    IE_Check(compiler->Compile(&sourceBuffer, arguments.data(), arguments.size(), &includeHandler, IID_PPV_ARGS(&compileResult)));
    IE_Assert(compileResult);

    IDxcBlobUtf8* errors;
    IE_Check(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr));
    IE_Assert(errors);

    if (errors->GetStringLength() > 0)
    {
        const String errorStr(static_cast<char*>(errors->GetBufferPointer()));
        IE_Error(errorStr);
        abort();
    }

    IDxcBlob* code;
    IE_Check(compileResult->GetResult(&code));
    IE_Assert(code);
    return code;
}