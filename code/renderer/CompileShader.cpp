// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "CompileShader.h"

#include "../common/Asserts.h"
#include "../common/Log.h"

namespace
{
LPCWSTR ToTargetName(const ShaderType type)
{
    switch (type)
    {
    case IE_SHADER_TYPE_VERTEX:
        return L"vs_6_6";
    case IE_SHADER_TYPE_PIXEL:
        return L"ps_6_6";
    case IE_SHADER_TYPE_COMPUTE:
        return L"cs_6_6";
    case IE_SHADER_TYPE_MESH:
        return L"ms_6_6";
    case IE_SHADER_TYPE_AMPLIFICATION:
        return L"as_6_6";
    case IE_SHADER_TYPE_LIB:
        return L"lib_6_6";
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
        if (m_IncludedFiles.Find(pFilename))
        {
            constexpr char nullStr[] = " ";
            IE_Check(m_Utils->CreateBlobFromPinned(nullStr, ARRAYSIZE(nullStr), DXC_CP_ACP, &pEncoding));
            *ppIncludeSource = pEncoding;
            return S_OK;
        }

        const HRESULT hr = m_Utils->LoadFile(pFilename, nullptr, &pEncoding);
        if (SUCCEEDED(hr))
        {
            m_IncludedFiles.Add(pFilename);
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

IDxcBlob* CompileShader(ShaderType type, const WString& filename, const Vector<WString>& extraArguments)
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
    IE_Check(library->CreateBlobFromFile(shaderPath.Data(), &codePage, &sourceBlob));
    IE_Assert(sourceBlob);

    const DxcBuffer sourceBuffer = {
        .Ptr = sourceBlob->GetBufferPointer(),
        .Size = sourceBlob->GetBufferSize(),
        .Encoding = 0,
    };

    // Add arguments
    Vector<LPCWSTR> arguments;
    arguments.Add(filename.Data());
    switch (type)
    {
    case IE_SHADER_TYPE_AMPLIFICATION:
    case IE_SHADER_TYPE_MESH:
    case IE_SHADER_TYPE_PIXEL:
    case IE_SHADER_TYPE_VERTEX:
    case IE_SHADER_TYPE_COMPUTE:
        arguments.Add(L"-E");
        arguments.Add(L"main");
        break;
    case IE_SHADER_TYPE_LIB:
        break;
    }
    arguments.Add(L"-T");
    arguments.Add(ToTargetName(type));
    arguments.Add(L"-I shaders");
    for (const WString& extraArg : extraArguments)
    {
        arguments.Add(extraArg.Data());
    }

    IncludeHandler includeHandler;
    IDxcResult* compileResult;
    IE_Check(compiler->Compile(&sourceBuffer, arguments.Data(), arguments.Size(), &includeHandler, IID_PPV_ARGS(&compileResult)));
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