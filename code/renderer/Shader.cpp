// Iskur Engine
// Copyright (c) 2026 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "Shader.h"

#include <cctype>
#include <format>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "RuntimeState.h"
#include "common/UtfConversion.h"

namespace
{
struct DxcContext
{
    ComPtr<IDxcLibrary> library;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    HRESULT initHr = S_OK;
    const char* initContext = nullptr;
    bool initialized = false;
};

struct ShaderCompileResult
{
    ComPtr<IDxcBlob> blob;
    String errorLog;
    bool ok : 1;
};

struct ShaderSourceHashResult
{
    u64 hash = 0;
    bool valid = false;
};

Shader::ReloadStats g_ShaderReloadStats;

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

void HashBytes(u64& hash, const void* data, const size_t size)
{
    const u8* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

void HashString(u64& hash, const String& text)
{
    if (!text.empty())
    {
        HashBytes(hash, text.data(), text.size());
    }

    constexpr u8 separator = 0xFF;
    HashBytes(hash, &separator, sizeof(separator));
}

void HashDefines(u64& hash, const Vector<String>& defines)
{
    const u32 defineCount = static_cast<u32>(defines.size());
    HashBytes(hash, &defineCount, sizeof(defineCount));
    for (const String& define : defines)
    {
        HashString(hash, define);
    }
}

bool ReadFileBytes(const std::filesystem::path& path, Vector<u8>& outBytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size < 0)
    {
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size == 0)
    {
        return true;
    }

    return file.read(reinterpret_cast<char*>(outBytes.data()), size).good();
}

bool TryParseIncludeLine(const std::string_view line, String& outInclude)
{
    size_t i = 0;
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF)
    {
        i = 3;
    }

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }

    if (i >= line.size())
    {
        return false;
    }

    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
    {
        return false;
    }

    if (line[i] != '#')
    {
        return false;
    }
    ++i;

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }

    constexpr std::string_view includeToken = "include";
    if (line.substr(i, includeToken.size()) != includeToken)
    {
        return false;
    }
    i += includeToken.size();

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }

    if (i >= line.size())
    {
        return false;
    }

    const char openDelimiter = line[i];
    const char closeDelimiter = openDelimiter == '"' ? '"' : (openDelimiter == '<' ? '>' : '\0');
    if (closeDelimiter == '\0')
    {
        return false;
    }

    ++i;
    const size_t begin = i;
    while (i < line.size() && line[i] != closeDelimiter)
    {
        ++i;
    }

    if (i <= begin || i >= line.size())
    {
        return false;
    }

    outInclude.assign(line.data() + begin, line.data() + i);
    return true;
}

String MakePathKey(std::filesystem::path path)
{
    std::error_code ec;
    path = std::filesystem::absolute(path, ec);
    path = path.lexically_normal();

    const std::u8string utf8Key = path.generic_u8string();
    String key;
    key.reserve(utf8Key.size());
    for (const char8_t c : utf8Key)
    {
        const unsigned char ch = static_cast<unsigned char>(c);
        key.push_back(static_cast<char>(std::tolower(ch)));
    }
    return key;
}

bool ResolveIncludePath(const std::filesystem::path& includingFile, const String& includeName, std::filesystem::path& outPath)
{
    const std::filesystem::path includePath(Utf8ToWide(includeName));
    std::error_code ec;

    if (includePath.is_absolute() && std::filesystem::exists(includePath, ec))
    {
        outPath = includePath;
        return true;
    }

    const std::filesystem::path localPath = includingFile.parent_path() / includePath;
    ec.clear();
    if (std::filesystem::exists(localPath, ec))
    {
        outPath = localPath;
        return true;
    }

    const std::filesystem::path projectPath = std::filesystem::path(L"data/shaders") / includePath;
    ec.clear();
    if (std::filesystem::exists(projectPath, ec))
    {
        outPath = projectPath;
        return true;
    }

    return false;
}

bool HashShaderFileRecursive(const std::filesystem::path& filePath, u64& hash, std::unordered_set<String>& visited)
{
    const String pathKey = MakePathKey(filePath);
    if (visited.find(pathKey) != visited.end())
    {
        return true;
    }
    visited.insert(pathKey);

    Vector<u8> bytes;
    if (!ReadFileBytes(filePath, bytes))
    {
        return false;
    }

    HashString(hash, pathKey);
    if (!bytes.empty())
    {
        HashBytes(hash, bytes.data(), bytes.size());
    }

    const String sourceText = bytes.empty() ? String() : String(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    size_t lineStart = 0;
    while (lineStart <= sourceText.size())
    {
        const size_t lineEnd = sourceText.find_first_of("\r\n", lineStart);
        const size_t lineLength = (lineEnd == String::npos) ? (sourceText.size() - lineStart) : (lineEnd - lineStart);
        const std::string_view line(sourceText.data() + lineStart, lineLength);

        String includeNameUtf8;
        if (TryParseIncludeLine(line, includeNameUtf8))
        {
            std::filesystem::path includePath;
            if (!ResolveIncludePath(filePath, includeNameUtf8, includePath))
            {
                // Some headers (for example shared CPU/GPU headers) may contain non-shader includes
                // behind preprocessor branches. Keep the hash stable without forcing recompilation.
                constexpr std::string_view unresolvedPrefix = "UNRESOLVED_INCLUDE:";
                HashBytes(hash, unresolvedPrefix.data(), unresolvedPrefix.size());
                HashString(hash, includeNameUtf8);
            }
            else if (!HashShaderFileRecursive(includePath, hash, visited))
            {
                return false;
            }
        }

        if (lineEnd == String::npos)
        {
            break;
        }

        lineStart = lineEnd + 1;
        if (lineStart < sourceText.size() && sourceText[lineEnd] == '\r' && sourceText[lineStart] == '\n')
        {
            ++lineStart;
        }
    }

    return true;
}

ShaderSourceHashResult ComputeShaderSourceHash(const ShaderType type, const String& filename, const Vector<String>& defines)
{
    ShaderSourceHashResult result{};
    result.hash = kFnvOffsetBasis;

    const u8 typeValue = static_cast<u8>(type);
    HashBytes(result.hash, &typeValue, sizeof(typeValue));
    HashDefines(result.hash, defines);

    const std::filesystem::path shaderPath = std::filesystem::path(L"data/shaders") / Utf8ToWide(filename);
    std::unordered_set<String> visited;
    result.valid = HashShaderFileRecursive(shaderPath, result.hash, visited);
    return result;
}

DxcContext& GetDxcContext()
{
    static DxcContext ctx = [] {
        DxcContext c{};

        c.initHr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&c.library));
        if (!IE_Try(c.initHr))
        {
            c.initContext = "DxcCreateInstance(CLSID_DxcLibrary)";
            return c;
        }

        c.initHr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&c.compiler));
        if (!IE_Try(c.initHr))
        {
            c.initContext = "DxcCreateInstance(CLSID_DxcCompiler)";
            return c;
        }

        c.initHr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&c.utils));
        if (!IE_Try(c.initHr))
        {
            c.initContext = "DxcCreateInstance(CLSID_DxcUtils)";
            return c;
        }

        c.initialized = true;
        return c;
    }();

    return ctx;
}

LPCWSTR ToTargetName(const ShaderType type)
{
    switch (type)
    {
    case IE_SHADER_TYPE_VERTEX:
        return L"vs_6_9";
    case IE_SHADER_TYPE_PIXEL:
        return L"ps_6_9";
    case IE_SHADER_TYPE_COMPUTE:
        return L"cs_6_9";
    case IE_SHADER_TYPE_MESH:
        return L"ms_6_9";
    case IE_SHADER_TYPE_AMPLIFICATION:
        return L"as_6_9";
    case IE_SHADER_TYPE_LIB:
        return L"lib_6_9";
    }
    return L"undefined";
}

ShaderCompileResult Fail(HRESULT hr, const char* context, ShaderCompileResult& result)
{
    LPSTR errorMessage = nullptr;
    String msg;

    if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPSTR>(&errorMessage), 0, nullptr) == 0)
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

ShaderCompileResult CompileShader(ShaderType type, const String& filename, const Vector<String>& defines)
{
    ShaderCompileResult result{};

    DxcContext& dxc = GetDxcContext();
    if (!dxc.initialized)
    {
        return Fail(dxc.initHr, dxc.initContext ? dxc.initContext : "DXC init", result);
    }

    HRESULT hr = S_OK;
    u32 codePage = CP_UTF8;
    ComPtr<IDxcBlobEncoding> sourceBlob;

    const std::wstring filenameWide = Utf8ToWide(filename);
    const std::wstring shaderPath = L"data/shaders/" + filenameWide;
    hr = dxc.library->CreateBlobFromFile(shaderPath.c_str(), &codePage, &sourceBlob);
    if (!IE_Try(hr))
    {
        return Fail(hr, "CreateBlobFromFile", result);
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = 0;

    Vector<std::wstring> defineArgs;
    defineArgs.reserve(defines.size());
    for (const String& define : defines)
    {
        defineArgs.push_back(L"-D" + Utf8ToWide(define));
    }

    Vector<LPCWSTR> arguments;
    arguments.push_back(filenameWide.c_str());
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
    for (const std::wstring& arg : defineArgs)
    {
        arguments.push_back(arg.data());
    }
    arguments.push_back(L"-enable-16bit-types");
    arguments.push_back(L"-Zpr");
#ifdef _DEBUG
    arguments.push_back(L"-Zi");
    arguments.push_back(L"-Qembed_debug");
#endif

    ComPtr<IDxcIncludeHandler> includeHandler;
    hr = dxc.utils->CreateDefaultIncludeHandler(&includeHandler);
    if (!IE_Try(hr))
    {
        return Fail(hr, "IDxcUtils::CreateDefaultIncludeHandler", result);
    }

    ComPtr<IDxcResult> compileResult;
    hr = dxc.compiler->Compile(&sourceBuffer, arguments.data(), static_cast<u32>(arguments.size()), includeHandler.Get(), IID_PPV_ARGS(&compileResult));
    if (!IE_Try(hr))
    {
        return Fail(hr, "IDxcCompiler3::Compile", result);
    }

    ComPtr<IDxcBlobUtf8> errors;
    hr = compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (!IE_Try(hr))
    {
        return Fail(hr, "IDxcResult::GetOutput(DXC_OUT_ERRORS)", result);
    }

    if (errors && errors->GetStringLength() > 0)
    {
        const char* msgPtr = errors->GetStringPointer();
        const size_t msgLen = errors->GetStringLength();
        result.errorLog.assign(msgPtr, msgPtr + msgLen);
    }

    HRESULT status = S_OK;
    hr = compileResult->GetStatus(&status);
    if (!IE_Try(hr))
    {
        return Fail(hr, "IDxcResult::GetStatus", result);
    }

    if (!IE_Try(status))
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
    if (!IE_Try(hr))
    {
        return Fail(hr, "IDxcResult::GetResult", result);
    }

    result.ok = true;
    result.blob = code;
    return result;
}
} // namespace

Shader::Shader(ShaderType type, String filename, Vector<String> defines)
    : m_Type(type), m_Filename(std::move(filename)), m_Defines(std::move(defines))
{
    const ShaderCompileResult result = CompileShader(type, m_Filename, m_Defines);
    if (!result.ok)
    {
        m_Valid = false;
        m_ErrorLog = result.errorLog;
        return;
    }

    m_Blob = result.blob;
    m_Valid = true;
    m_ErrorLog.clear();

    const ShaderSourceHashResult sourceHash = ComputeShaderSourceHash(m_Type, m_Filename, m_Defines);
    m_SourceHash = sourceHash.hash;
    m_SourceHashValid = sourceHash.valid;
}

bool Shader::Reload()
{
    bool didReload = false;
    return Reload(didReload);
}

bool Shader::Reload(bool& outDidReload)
{
    const ShaderSourceHashResult sourceHash = ComputeShaderSourceHash(m_Type, m_Filename, m_Defines);
    if (sourceHash.valid && m_SourceHashValid && sourceHash.hash == m_SourceHash)
    {
        outDidReload = false;
        return true;
    }

    const ShaderCompileResult result = CompileShader(m_Type, m_Filename, m_Defines);
    if (!result.ok)
    {
        outDidReload = false;
        m_ErrorLog = result.errorLog;
        IE_LogError("Shader reload failed for {}:\n{}\n", m_Filename, m_ErrorLog);
        g_Stats.shadersCompilationSuccess = false;
        return false;
    }

    m_Blob = result.blob;
    m_Valid = true;
    m_ErrorLog.clear();
    m_SourceHash = sourceHash.hash;
    m_SourceHashValid = sourceHash.valid;
    m_CachedRootSig.Reset();
    m_CachedRootSigDevice = nullptr;
    outDidReload = true;
    return true;
}

bool Shader::ReloadOrCreate(SharedPtr<Shader>& shader, ShaderType type, const String& filename, const Vector<String>& defines)
{
    g_ShaderReloadStats.total++;

    auto LogCompileFailure = [&](const SharedPtr<Shader>& failedShader) {
        g_ShaderReloadStats.failed++;
        IE_LogError("Shader compile failed for {}:\n{}\n", filename, failedShader->GetErrorLog());
        g_Stats.shadersCompilationSuccess = false;
    };

    if (!shader)
    {
        SharedPtr<Shader> candidate = IE_MakeSharedPtr<Shader>(type, filename, defines);
        if (!candidate->IsValid())
        {
            LogCompileFailure(candidate);
            return false;
        }

        g_ShaderReloadStats.reloaded++;
        shader = std::move(candidate);
        return true;
    }

    const bool isSameConfig = shader->m_Type == type && shader->m_Filename == filename && shader->m_Defines == defines;
    if (isSameConfig)
    {
        bool didReload = false;
        const bool success = shader->Reload(didReload);
        if (!success)
        {
            g_ShaderReloadStats.failed++;
            return false;
        }

        if (didReload)
        {
            g_ShaderReloadStats.reloaded++;
        }
        else
        {
            g_ShaderReloadStats.skipped++;
        }

        return true;
    }

    SharedPtr<Shader> candidate = IE_MakeSharedPtr<Shader>(type, filename, defines);
    if (!candidate->IsValid())
    {
        LogCompileFailure(candidate);
        return false;
    }

    g_ShaderReloadStats.reloaded++;
    shader = std::move(candidate);
    return true;
}

void Shader::ResetReloadStats()
{
    g_ShaderReloadStats = {};
}

Shader::ReloadStats Shader::GetReloadStats()
{
    return g_ShaderReloadStats;
}

D3D12_SHADER_BYTECODE Shader::GetBytecode() const
{
    IE_Assert(m_Valid && m_Blob && m_Blob->GetBufferSize() > 0);
    D3D12_SHADER_BYTECODE bytecode{};
    bytecode.pShaderBytecode = m_Blob->GetBufferPointer();
    bytecode.BytecodeLength = m_Blob->GetBufferSize();
    return bytecode;
}

const ComPtr<ID3D12RootSignature>& Shader::GetOrCreateRootSignature(const ComPtr<ID3D12Device14>& device)
{
    IE_Assert(device != nullptr);
    IE_Assert(m_Valid && m_Blob && m_Blob->GetBufferSize() > 0);

    if (!m_CachedRootSig || m_CachedRootSigDevice != device.Get())
    {
        IE_Check(device->CreateRootSignature(0, m_Blob->GetBufferPointer(), m_Blob->GetBufferSize(), IID_PPV_ARGS(&m_CachedRootSig)));
        m_CachedRootSigDevice = device.Get();
    }

    return m_CachedRootSig;
}

bool Shader::IsValid() const
{
    return m_Valid;
}

const String& Shader::GetErrorLog() const
{
    return m_ErrorLog;
}
