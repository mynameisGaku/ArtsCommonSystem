// SPDX-License-Identifier: Apache-2.0
#include "foundation/Log.h"
#include "render/Dx12/Dx12DxcCompilerInternal.h"
#include "memory/Memory.h"

#include <dxcapi.h>

namespace acs::render_internal {
namespace {

/** Windowsの長い絶対パスを終端込みで収容する上限。 */
constexpr u32 kDxcPathCapacity = 32768u;

/** 呼出し元の作業場所ではなく、この実装を収録したexeまたはDLLの隣を解決する。 */
HRESULT ResolveDxcLibraryPath_Internal(const wchar_t* filename, wchar_t (&path)[kDxcPathCapacity]) noexcept
{
    /** 実装コードが属するモジュール。参照数は変更しない。 */
    HMODULE owner = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CompileDxilShader_Internal), &owner)) return HRESULT_FROM_WIN32(::GetLastError());
    /** 終端を除くパス長。切詰めを成功扱いしない。 */
    const DWORD length = ::GetModuleFileNameW(owner, path, kDxcPathCapacity);
    if (length == 0u) return HRESULT_FROM_WIN32(::GetLastError());
    if (length >= kDxcPathCapacity) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    /** 最後の区切り直後へファイル名だけを追加する位置。 */
    u32 position = length;
    while (position != 0u && path[position - 1u] != L'\\' && path[position - 1u] != L'/') --position;
    if (position == 0u) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    for (u32 index = 0u; filename[index] != L'\0'; ++index) {
        if (position >= kDxcPathCapacity - 1u) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        path[position++] = filename[index];
    }
    path[position] = L'\0';
    return S_OK;
}

/** 初回の成功時だけDLLをプロセス寿命へ固定する。Windowsが終了時に本体を回収する。 */
BOOL CALLBACK PinDxcLibrary_Internal(PINIT_ONCE, PVOID library_address, PVOID*) noexcept
{
    /** PINによって通常の参照解放後も保持されるモジュール。 */
    HMODULE pinned = nullptr;
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, static_cast<LPCWSTR>(library_address), &pinned);
}

/** 同時作成でも固定処理を一度だけ行う。失敗した初期化は次回再試行できる。 */
HRESULT KeepDxcLibrariesLoaded_Internal(HMODULE compiler, HMODULE validator) noexcept
{
    /** このRender実装が使う作成器のプロセス寿命への固定状態。 */
    static INIT_ONCE compiler_once = INIT_ONCE_STATIC_INIT;
    /** このRender実装が使う検証器のプロセス寿命への固定状態。 */
    static INIT_ONCE validator_once = INIT_ONCE_STATIC_INIT;
    if (!::InitOnceExecuteOnce(&compiler_once, PinDxcLibrary_Internal, compiler, nullptr)) return HRESULT_FROM_WIN32(::GetLastError());
    if (!::InitOnceExecuteOnce(&validator_once, PinDxcLibrary_Internal, validator, nullptr)) return HRESULT_FROM_WIN32(::GetLastError());
    return S_OK;
}

/** 一回の作成が使うCOMとDLLを所有し、失敗箇所によらず逆順で解放する。 */
class FScopedDxcCompilation final {
public:
    /** まだDLLを読み込まない空状態で開始する。 */
    FScopedDxcCompilation() noexcept = default;
    /** 所有権の複製による二重解放を禁止する。 */
    FScopedDxcCompilation(const FScopedDxcCompilation&) = delete;
    /** 所有権の上書きを禁止する。 */
    FScopedDxcCompilation& operator=(const FScopedDxcCompilation&) = delete;

    /** 全COMと今回のDLL参照を解放する。本体は初回の固定によりプロセス終了まで残る。 */
    ~FScopedDxcCompilation() noexcept
    {
        ACS_SAFE_RELEASE(m_ValidationErrors);
        ACS_SAFE_RELEASE(m_ValidatedBytecode);
        ACS_SAFE_RELEASE(m_ValidationResult);
        ACS_SAFE_RELEASE(m_Validator);
        ACS_SAFE_RELEASE(m_Bytecode);
        ACS_SAFE_RELEASE(m_CompileErrors);
        ACS_SAFE_RELEASE(m_CompileResult);
        ACS_SAFE_RELEASE(m_Compiler);
        if (m_CompilerLibrary) ::FreeLibrary(m_CompilerLibrary);
        if (m_ValidatorLibrary) ::FreeLibrary(m_ValidatorLibrary);
    }

    /** 指定形式で作成し、独立した検証器の成功後だけ既存の命令所有者へ渡す。 */
    HRESULT Compile(const FShaderDesc& desc, const char* target, ID3DBlob*& output) noexcept
    {
        /** 同梱DLLの絶対パス。二つの読込みで領域を共用する。 */
        wchar_t path[kDxcPathCapacity]{};
        /** 各外部呼出しの失敗をそのまま返す結果。 */
        HRESULT result = ResolveDxcLibraryPath_Internal(L"dxil.dll", path);
        if (FAILED(result)) return result;
        m_ValidatorLibrary = ::LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_ValidatorLibrary) return HRESULT_FROM_WIN32(::GetLastError());
        result = ResolveDxcLibraryPath_Internal(L"dxcompiler.dll", path);
        if (FAILED(result)) return result;
        m_CompilerLibrary = ::LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_CompilerLibrary) return HRESULT_FROM_WIN32(::GetLastError());

        /** 読込み済みの各DLLから得た作成関数。PATH上の別版は検索しない。 */
        const auto create_compiler = reinterpret_cast<DxcCreateInstanceProc>(::GetProcAddress(m_CompilerLibrary, "DxcCreateInstance"));
        const auto create_validator = reinterpret_cast<DxcCreateInstanceProc>(::GetProcAddress(m_ValidatorLibrary, "DxcCreateInstance"));
        if (!create_compiler || !create_validator) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        // DXC内部の寿命を作成単位で切らない。COMを作る前に常駐を確定する。
        result = KeepDxcLibrariesLoaded_Internal(m_CompilerLibrary, m_ValidatorLibrary);
        if (FAILED(result)) return result;
        result = create_compiler(CLSID_DxcCompiler, IID_PPV_ARGS(&m_Compiler));
        if (FAILED(result) || !m_Compiler) return FAILED(result) ? result : E_FAIL;
        result = create_validator(CLSID_DxcValidator, IID_PPV_ARGS(&m_Validator));
        if (FAILED(result) || !m_Validator) return FAILED(result) ? result : E_FAIL;

        /** UTF-8の関数名を切り詰めずにUTF-16へ変換するための必要長。 */
        const int entry_count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc.entry_point, -1, nullptr, 0);
        if (entry_count <= 0) return E_INVALIDARG;
        /** 作成が終わるまで引数の文字列を所有するACS配列。 */
        TArray<wchar_t> entry;
        if (!entry.TrySetNum(static_cast<usize>(entry_count))) return E_OUTOFMEMORY;
        if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, desc.entry_point, -1, entry.GetData(), entry_count) != entry_count) return E_INVALIDARG;
        /** 呼出し側で形式確認済みの六文字と終端。 */
        wchar_t wide_target[7]{};
        for (u32 index = 0u; index < 6u; ++index) wide_target[index] = static_cast<wchar_t>(target[index]);
        /** 既存HLSLの言語版を固定し、最適化だけビルド種別へ合わせる。 */
#if ACS_BUILD_DEBUG
        const wchar_t* arguments[] = {L"-E", entry.GetData(), L"-T", wide_target, L"-Ges", L"-HV", L"2018", L"-Od", L"-Zi", L"-Qembed_debug"};
#else
        const wchar_t* arguments[] = {L"-E", entry.GetData(), L"-T", wide_target, L"-Ges", L"-HV", L"2018", L"-O3"};
#endif
        /** 終端を除いた入力ソースのバイト長。 */
        usize source_length = 0u;
        while (desc.hlsl_source[source_length] != '\0') ++source_length;
        /** ソースの所有権は同期呼出し中の呼出し元に残る。 */
        const DxcBuffer source{desc.hlsl_source, source_length, DXC_CP_UTF8};
        result = m_Compiler->Compile(&source, arguments, static_cast<UINT32>(sizeof(arguments) / sizeof(arguments[0])), nullptr, IID_PPV_ARGS(&m_CompileResult));
        if (FAILED(result) || !m_CompileResult) return FAILED(result) ? result : E_FAIL;
        /** COM呼出しの成功とは別に、HLSL作成自体の成否を確認する。 */
        HRESULT compile_status = E_FAIL;
        result = m_CompileResult->GetStatus(&compile_status);
        if (FAILED(result)) return result;
        if (SUCCEEDED(m_CompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&m_CompileErrors), nullptr)) && m_CompileErrors && m_CompileErrors->GetStringLength() != 0u) {
            if (FAILED(compile_status)) ACS_LOG_ERROR("DXC (%s): %s", desc.debug_name ? desc.debug_name : "shader", m_CompileErrors->GetStringPointer());
            else ACS_LOG_WARN("DXC (%s): %s", desc.debug_name ? desc.debug_name : "shader", m_CompileErrors->GetStringPointer());
        }
        if (FAILED(compile_status)) return compile_status;
        result = m_CompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&m_Bytecode), nullptr);
        if (FAILED(result) || !m_Bytecode) return FAILED(result) ? result : E_FAIL;

        // 検証器が欠落した場合も、未検証の命令を公開する代替経路へは進まない。
        result = m_Validator->Validate(m_Bytecode, DxcValidatorFlags_InPlaceEdit, &m_ValidationResult);
        if (FAILED(result) || !m_ValidationResult) return FAILED(result) ? result : E_FAIL;
        /** 検証の呼出し成功と、命令の受理を区別する結果。 */
        HRESULT validation_status = E_FAIL;
        result = m_ValidationResult->GetStatus(&validation_status);
        if (FAILED(result)) return result;
        if (FAILED(validation_status)) {
            if (SUCCEEDED(m_ValidationResult->GetErrorBuffer(&m_ValidationErrors)) && m_ValidationErrors) {
                /** 診断文字列の終端に依存しない最大出力長。 */
                const usize length = m_ValidationErrors->GetBufferSize();
                const int bounded_length = static_cast<int>(length < 0x7fffffffu ? length : 0x7fffffffu);
                ACS_LOG_ERROR("DXIL (%s): %.*s", desc.debug_name ? desc.debug_name : "shader", bounded_length, static_cast<const char*>(m_ValidationErrors->GetBufferPointer()));
            }
            return validation_status;
        }
        result = m_ValidationResult->GetResult(&m_ValidatedBytecode);
        if (FAILED(result) || !m_ValidatedBytecode || m_ValidatedBytecode->GetBufferSize() == 0u) return FAILED(result) ? result : E_FAIL;
        /** DXCのDLL寿命から独立して既存FDx12Shaderが保持できる領域。 */
        ID3DBlob* candidate = nullptr;
        result = ::D3DCreateBlob(m_ValidatedBytecode->GetBufferSize(), &candidate);
        if (FAILED(result) || !candidate) {
            ACS_SAFE_RELEASE(candidate);
            return FAILED(result) ? result : E_OUTOFMEMORY;
        }
        MemCopy(candidate->GetBufferPointer(), m_ValidatedBytecode->GetBufferPointer(), candidate->GetBufferSize());
        output = candidate;
        return S_OK;
    }

private:
    /** 検証器のCOMを解放するまで維持するDLL参照。 */
    HMODULE m_ValidatorLibrary = nullptr;
    /** 作成器とその出力を解放するまで維持するDLL参照。 */
    HMODULE m_CompilerLibrary = nullptr;
    /** 一回の作成を担当するCOM。 */
    IDxcCompiler3* m_Compiler = nullptr;
    /** 作成結果と診断を所有するCOM。 */
    IDxcResult* m_CompileResult = nullptr;
    /** UTF-8として取得した作成時の診断。 */
    IDxcBlobUtf8* m_CompileErrors = nullptr;
    /** 作成器が返した命令。 */
    IDxcBlob* m_Bytecode = nullptr;
    /** 同梱dxil.dllから明示作成した検証器。 */
    IDxcValidator* m_Validator = nullptr;
    /** 命令検証の成否と出力。 */
    IDxcOperationResult* m_ValidationResult = nullptr;
    /** 検証が済んだ命令。公開前に独立した領域へコピーする。 */
    IDxcBlob* m_ValidatedBytecode = nullptr;
    /** 検証に失敗した場合の診断。 */
    IDxcBlobEncoding* m_ValidationErrors = nullptr;
};

} // namespace

HRESULT CompileDxilShader_Internal(const FShaderDesc& desc, const char* target, ID3DBlob*& output) noexcept
{
    if (output || !desc.hlsl_source || !desc.entry_point || !target) return E_INVALIDARG;
    /** 非同期呼出し間で共有しない、今回だけの所有者。 */
    FScopedDxcCompilation compiler;
    /** 呼出し元へ伝搬する失敗理由。 */
    const HRESULT result = compiler.Compile(desc, target, output);
    if (FAILED(result)) ACS_LOG_ERROR("DXC (%s): 作成または検証に失敗しました (HRESULT=0x%08X)。", desc.debug_name ? desc.debug_name : "shader", static_cast<u32>(result));
    return result;
}

} // namespace acs::render_internal
