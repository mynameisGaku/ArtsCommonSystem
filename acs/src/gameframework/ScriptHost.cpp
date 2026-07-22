// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — FScriptHost / ScriptVmStub 実装
//
// 具象 scripting backend (Lua 5.4 / Wren / Python3) はいずれも
// 未統合のため、本ファイルは:
//   1. `ScriptVmStub` … 全 method NotImplemented を返す defensive stub
//   2. `FScriptHost`   … vm を保持して native registry を集約する高レベル wrapper
// の 2 つだけを提供する。
//
// 上位層への効果:
//   ・`ScriptVmStub` を叩いた瞬間に NotImplemented を返すので、scripting
//     に依存したコードは day-0 で **必ず fallback パスを書く** ことを強制
//     される (= 配布パッケージに Lua 同梱しないビルドでもゲームが起動できる)。
//   ・`FScriptHost::LoadAndRun` は Win32 ファイル I/O を直接叩くが、読み込み
//     成功後の処理は `IScriptVm::LoadScript` に丸投げするので、本ファイルは
//     スクリプト構文に一切触らない (= backend 切り替えが純粋な差し替えになる)。
//
// 決定論ゾーン違反検出 (将来):
//   `FGame::Tick()` の固定タイムステップ内で `CallFunction` が
//   呼ばれたら debug build で assert する仕掛けを `FGame.cpp` 側に入れる予定。
//   本 stub では何も検出しないが、コメントとして契約を明示しておく。
#include "gameframework/ScriptHost.h"

#include "foundation/Platform.h"   // <windows.h> (CreateFileW / ReadFile / GetFileSizeEx)
#include "foundation/Log.h"
#include "foundation/Error.h"
#include "memory/Memory.h"         // DefaultAllocator / MemSet
#include "threading/Atomic.h"

namespace acs::game {

namespace {

/**
 * 両者 nullptr 安全な C 文字列等価判定 (native registry の名前比較用)。
 *
 * @details
 * FModRegistry::IdEquals と同じ流儀で、<string.h> の strcmp を直接呼ばずヌルポインタ事故を
 * 抑止する薄いラッパ。strlen に依存しないため NUL を含む短い名前にも安全。
 * @param a 比較する一方の文字列 (nullptr 可)。
 * @param b 比較するもう一方の文字列 (nullptr 可)。
 * @return 同一ポインタ / 両 nullptr / 内容一致なら true、それ以外は false。
 */
bool CStrEquals(const char* a, const char* b) noexcept {
    if (a == b)                       return true;     // 同一ポインタ (or 両 nullptr)
    if (a == nullptr || b == nullptr) return false;
    // 1 文字ずつ比較。strlen に依存しないので NUL 含む短い名前にも安全。
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;  // 両方 '\0' で揃って終わったか
}

/**
 * C文字列が上限内でNUL終端されているかを検証する。
 */
bool TryGetBoundedLength(const char* text, u32 max_bytes, u32& out_length) noexcept {
    out_length = 0u;
    if (text == nullptr) return false;
    while (out_length <= max_bytes && text[out_length] != '\0') {
        ++out_length;
    }
    return out_length <= max_bytes;
}

bool TryGetBoundedLength(
    const wchar_t* text, u32 max_chars, u32& out_length) noexcept {
    out_length = 0u;
    if (text == nullptr) return false;
    while (out_length <= max_chars && text[out_length] != L'\0') {
        ++out_length;
    }
    return out_length <= max_chars;
}

} // anonymous namespace

/** Init の実装 (副作用ゼロで成功、initialized フラグを立てる)。 */
TResult<void> FScriptVmStub::Init() noexcept {
    // Stub は副作用ゼロで成功させる (= 起動シーケンスを通すため)。
    // 実際のスクリプト動作は LoadScript / CallFunction の段階で
    // NotImplemented として落とす。
    m_Initialized = true;
    return Ok();
}

/** Shutdown の実装 (no-op、initialized フラグを下げる)。 */
void FScriptVmStub::Shutdown() noexcept {
    // 何も保持していないので no-op。Init 前に呼ばれても安全。
    m_Initialized = false;
}

/** LoadScript の実装 (Init 前は kSub_NotInitialized、それ以外は kSub_NotImplemented)。 */
TResult<void> FScriptVmStub::LoadScript(const char* /*source*/,
                                      u32         /*source_len*/,
                                      const char* /*chunk_name*/) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized,
                       "ScriptVmStub::LoadScript called before Init()");
    }
    return ACS_ERR(Generic, script_err::kSub_NotImplemented,
                   "ScriptVmStub::LoadScript: scripting backend not integrated (Phase N-2 stub)");
}

/** CallFunction の実装 (Init 前は kSub_NotInitialized、それ以外は kSub_NotImplemented)。 */
TResult<void> FScriptVmStub::CallFunction(const char* /*function_name*/,
                                        const FScriptValue* /*args*/, u32 /*arg_count*/,
                                        FScriptValue* /*ret_out*/) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized,
                       "ScriptVmStub::CallFunction called before Init()");
    }
    return ACS_ERR(Generic, script_err::kSub_NotImplemented,
                   "ScriptVmStub::CallFunction: scripting backend not integrated (Phase N-2 stub)");
}

/** RegisterNativeFunction の実装 (Init 前は kSub_NotInitialized、それ以外は kSub_NotImplemented)。 */
TResult<void> FScriptVmStub::RegisterNativeFunction(const char* /*function_name*/,
                                                  NativeFunction /*fn*/,
                                                  void* /*user*/) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized,
                       "ScriptVmStub::RegisterNativeFunction called before Init()");
    }
    return ACS_ERR(Generic, script_err::kSub_NotImplemented,
                   "ScriptVmStub::RegisterNativeFunction: scripting backend not integrated (Phase N-2 stub)");
}

/** SetGlobalNumber の実装 (値の保存先が無いので no-op)。 */
void FScriptVmStub::SetGlobalNumber(const char* /*name*/, f64 /*value*/) noexcept {
    // 値を保存する場所が無いので no-op (= 後から GetGlobalNumber しても default
    // が返る)。上位コードは「stub では持続しない」前提で書く。
}

/** GetGlobalNumber の実装 (常に default_value を返す)。 */
f64 FScriptVmStub::GetGlobalNumber(const char* /*name*/, f64 default_value) const noexcept {
    return default_value;
}

/** CollectGarbage の実装 (GC 対象が無いので no-op)。 */
void FScriptVmStub::CollectGarbage() noexcept {
    // GC の対象が存在しないので no-op。
}

/**
 * GetVmStub の実装 (Meyers singleton)。
 *
 * @details
 * process 内で 1 個だけ存在する ScriptVmStub インスタンスへの参照を返し、呼んだすべての箇所が
 * 同じ instance を共有する。C++11 以降、関数内 static の初期化は thread-safe (magic statics) だが、
 * stub 自身が状態 (m_Initialized) を持つため複数スレッドから同時アクセスする呼び出し側は外部同期を取ること。
 */
IScriptVm& GetVmStub() noexcept {
    static FScriptVmStub instance;
    return instance;
}

namespace {
/** 登録済みの既定 VM provider (未登録なら nullptr、実 backend の Install* で設定される)。 */
TAtomic<ScriptVmProvider> g_ScriptVmProvider{nullptr};
}

/** SetScriptVmProvider の実装 (provider を上書き保存、後勝ち)。 */
void SetScriptVmProvider(ScriptVmProvider Provider) noexcept
{
    g_ScriptVmProvider.Store(Provider, EMemoryOrder::Release);
}

/** GetDefaultScriptVm の実装 (provider 登録済みならその実 VM、未登録なら GetVmStub)。 */
IScriptVm& GetDefaultScriptVm() noexcept
{
    const ScriptVmProvider Provider = g_ScriptVmProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetVmStub();
}

/** Init の実装 (nullptr は Shutdown 相当、既存 vm があれば警告して上書きしキャッシュをクリア)。 */
void FScriptHost::Init(IScriptVm* vm) noexcept {
    // nullptr 渡しは「明示的に切り離す」意味で許容する (= Shutdown 相当)。
    if (vm == nullptr) {
        Shutdown();
        return;
    }
    // 既に他の vm が設定されていた場合は、警告ログを出して上書きする
    // (後勝ち)。本来は二度差しを避けるべきだが、テスト中の差し替え運用を
    // 楽にするため拒否ではなく警告に留める。
    if (m_Vm != nullptr && m_Vm != vm) {
        ACS_LOG_WARN("FScriptHost::Init: overwriting existing vm (likely a re-init in tests)");
        // 既存の native 登録は新 vm 向けではないのでクリアする。
        m_Natives.Clear();
    }
    m_Vm = vm;
}

/** Shutdown の実装 (vm 参照を切り native キャッシュをクリア、error callback は残す)。 */
void FScriptHost::Shutdown() noexcept {
    // vm 自体は所有していないので破棄しない。参照だけ切る。
    m_Vm = nullptr;
    m_Natives.Clear();
    // error callback はあえて残す (Shutdown 後 → 再 Init で同じ UI に
    // 通知させたいシナリオが多いため)。明示的に SetOnErrorCallback(nullptr)
    // を呼んでクリアすること。
}

/** Vm の実装 (保持中の vm ポインタを返す)。 */
IScriptVm* FScriptHost::Vm() const noexcept {
    return m_Vm;
}

/** 長さ付きsourceを検証してVMへロード・実行する。 */
TResult<void> FScriptHost::LoadAndRunSource(const char* source,
                                            u32         source_len,
                                            const char* chunk_name) noexcept {
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::LoadAndRunSource: vm not initialized");
    }
    if (source == nullptr && source_len > 0u) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::LoadAndRunSource: source is null");
    }
    if (static_cast<u64>(source_len) > kMaxScriptFileBytes) {
        return ACS_ERR(Generic, script_err::kSub_FileTooLarge,
                       "FScriptHost::LoadAndRunSource: source exceeds safety limit");
    }
    if (chunk_name != nullptr) {
        u32 chunk_length = 0u;
        if (!TryGetBoundedLength(chunk_name, kMaxScriptChunkNameBytes, chunk_length)) {
            return ACS_ERR(Generic, script_err::kSub_InvalidName,
                           "FScriptHost::LoadAndRunSource: invalid chunk name");
        }
    }
    for (u32 i = 0u; i < source_len; ++i) {
        if (source[i] == '\0') {
            return ACS_ERR(Generic, script_err::kSub_EmbeddedNul,
                           "FScriptHost::LoadAndRunSource: embedded NUL rejected");
        }
    }

    const char* const safe_source = source != nullptr ? source : "";
    TResult<void> result = m_Vm->LoadScript(
        safe_source,
        source_len,
        chunk_name != nullptr ? chunk_name : "<memory>");
    if (result.IsErr()) {
        FireError(chunk_name != nullptr ? chunk_name : "<memory>",
                  0u,
                  result.Error().message);
    }
    return result;
}

/** LoadAndRun の実装 (Win32 でファイルを読み込み IScriptVm::LoadScript へ流す)。 */
TResult<void> FScriptHost::LoadAndRun(const wchar_t* file_path) noexcept {
    // 事前チェック。
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::LoadAndRun: vm not initialized (call Init(vm) first)");
    }
    u32 path_length = 0u;
    if (!TryGetBoundedLength(file_path, kMaxScriptPathChars, path_length) ||
        path_length == 0u) {
        return ACS_ERR(Generic, script_err::kSub_InvalidPath,
                       "FScriptHost::LoadAndRun: invalid file path");
    }

    // Win32 で読み込み。FSaveArchive.cpp と同じ流儀 (CreateFileW + GetFileSizeEx + ReadFile)。
    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        const u16 subcode =
            (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                ? script_err::kSub_FileNotFound
                : script_err::kSub_Io;
        return ACS_ERR_OS(IO, subcode,
                          "FScriptHost::LoadAndRun: CreateFileW failed", err);
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(h, &size_li)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, script_err::kSub_Io,
                          "FScriptHost::LoadAndRun: GetFileSizeEx failed", err);
    }

    if (size_li.QuadPart < 0) {
        ::CloseHandle(h);
        return ACS_ERR(IO, script_err::kSub_Io,
                       "FScriptHost::LoadAndRun: negative file size");
    }
    const u64 size_u64 = static_cast<u64>(size_li.QuadPart);
    if (size_u64 == 0) {
        if (!::CloseHandle(h)) {
            return ACS_ERR_OS(IO, script_err::kSub_Io,
                              "FScriptHost::LoadAndRun: CloseHandle failed",
                              ::GetLastError());
        }
        return LoadAndRunSource("", 0u, "<empty>");
    }
    if (size_u64 > kMaxScriptFileBytes) {
        ::CloseHandle(h);
        return ACS_ERR(Generic, script_err::kSub_FileTooLarge,
                       "FScriptHost::LoadAndRun: script file exceeds 64 MiB sanity limit");
    }

    // バッファ確保。スクリプトの読み込みは frame 跨ぎではないので、Default
    // FAllocator を直接叩く (TArray<u8> + Resize でも良いが余分な fill cost を
    // 避けるため、生 Alloc → Free でラウンドトリップさせる)。
    const usize buf_size = static_cast<usize>(size_u64);
    FAllocator&  alloc    = DefaultAllocator();
    void*       raw      = alloc.Alloc(buf_size, alignof(u8), FSourceLoc::Current());
    if (raw == nullptr) {
        ::CloseHandle(h);
        return ACS_ERR(Memory, script_err::kSub_AllocationFailed,
                       "FScriptHost::LoadAndRun: failed to allocate script buffer");
    }
    u8* buf = static_cast<u8*>(raw);

    // chunk ループ (4 GiB 以上の script は実用上ありえないが、ReadFile が
    // DWORD 単位なので一応分割で読む。FSaveArchive と同じ流儀)。
    u8*   p         = buf;
    u64   remaining = size_u64;
    DWORD io_err    = 0;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            io_err = ::GetLastError();
            if (io_err == 0) io_err = ERROR_HANDLE_EOF;
            alloc.Free(raw);
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, script_err::kSub_Io,
                              "FScriptHost::LoadAndRun: ReadFile failed", io_err);
        }
        p         += got;
        remaining -= got;
    }

    LARGE_INTEGER final_size{};
    if (!::GetFileSizeEx(h, &final_size)) {
        const DWORD size_err = ::GetLastError();
        alloc.Free(raw);
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, script_err::kSub_Io,
                          "FScriptHost::LoadAndRun: final GetFileSizeEx failed",
                          size_err);
    }

    u8 extra_byte = 0u;
    DWORD extra_read = 0u;
    const BOOL extra_ok = ::ReadFile(h, &extra_byte, 1u, &extra_read, nullptr);
    if (final_size.QuadPart != size_li.QuadPart || (extra_ok && extra_read != 0u)) {
        alloc.Free(raw);
        ::CloseHandle(h);
        return ACS_ERR(IO, script_err::kSub_FileChanged,
                       "FScriptHost::LoadAndRun: file changed during read");
    }
    if (!extra_ok) {
        const DWORD extra_err = ::GetLastError();
        if (extra_err != ERROR_HANDLE_EOF) {
            alloc.Free(raw);
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, script_err::kSub_Io,
                              "FScriptHost::LoadAndRun: EOF verification failed",
                              extra_err);
        }
    }
    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        alloc.Free(raw);
        return ACS_ERR_OS(IO, script_err::kSub_Io,
                          "FScriptHost::LoadAndRun: CloseHandle failed",
                          close_err);
    }

    // backend に渡す。
    // chunk_name は wide path を ASCII 化するのが面倒なので、
    // 固定文字列で代用する。実 Lua backend では caller から
    // explicit chunk name を取る overload を追加予定。
    const u32 src_len = static_cast<u32>(buf_size);
    TResult<void> r = LoadAndRunSource(
        reinterpret_cast<const char*>(buf),
        src_len,
        "<file>");

    // backend が source を内部で複製しているはず (Lua の luaL_loadbuffer は
    // const char* を即時パースして AST 化するので、戻った時点でバッファは
    // 解放してよい)。stub も中身を見ないのでここで解放安全。
    alloc.Free(raw);

    return r;
}

/** CallGlobalFunction の実装 (引数を防御チェックして vm->CallFunction へ委譲)。 */
TResult<void> FScriptHost::CallGlobalFunction(const char*        function_name,
                                            const FScriptValue* args,
                                            u32                arg_count,
                                            FScriptValue*       ret_out) noexcept {
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::CallGlobalFunction: vm not initialized (call Init(vm) first)");
    }
    u32 function_name_length = 0u;
    if (!TryGetBoundedLength(function_name,
                             kMaxScriptFunctionNameBytes,
                             function_name_length) ||
        function_name_length == 0u) {
        return ACS_ERR(Generic, script_err::kSub_InvalidName,
                       "FScriptHost::CallGlobalFunction: invalid function name");
    }
    if (arg_count > 0 && args == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::CallGlobalFunction: arg_count > 0 but args is null");
    }
    if (arg_count > kMaxScriptCallArguments) {
        return ACS_ERR(Generic, script_err::kSub_ArgumentLimit,
                       "FScriptHost::CallGlobalFunction: argument limit exceeded");
    }

    u32 total_string_bytes = 0u;
    for (u32 i = 0u; i < arg_count; ++i) {
        const u32 kind = static_cast<u32>(args[i].kind);
        if (kind > static_cast<u32>(EScriptValueKind::Handle)) {
            return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                           "FScriptHost::CallGlobalFunction: invalid argument kind");
        }
        if (args[i].kind == EScriptValueKind::String) {
            u32 string_length = 0u;
            const u32 remaining =
                kMaxScriptStringArgumentBytes - total_string_bytes;
            if (!TryGetBoundedLength(args[i].v.str, remaining, string_length)) {
                return ACS_ERR(Generic, script_err::kSub_ArgumentLimit,
                               "FScriptHost::CallGlobalFunction: string argument limit exceeded");
            }
            total_string_bytes += string_length;
        }
    }

    FScriptValue staged_return{};
    TResult<void> r = m_Vm->CallFunction(
        function_name,
        args,
        arg_count,
        ret_out != nullptr ? &staged_return : nullptr);
    if (r.IsErr()) {
        FireError(function_name, 0, r.Error().message);
        return r;
    }
    if (ret_out != nullptr) {
        *ret_out = staged_return;
    }
    return r;
}

/** RegisterNative の実装 (vm に登録しつつ内部キャッシュへ記録、同名は上書き、失敗時は追加しない)。 */
TResult<void> FScriptHost::RegisterNative(const char*    function_name,
                                        NativeFunction fn,
                                        void*          user) noexcept {
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::RegisterNative: vm not initialized (call Init(vm) first)");
    }
    u32 function_name_length = 0u;
    if (fn == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::RegisterNative: fn is null");
    }
    if (!TryGetBoundedLength(function_name,
                             kMaxScriptFunctionNameBytes,
                             function_name_length) ||
        function_name_length == 0u) {
        return ACS_ERR(Generic, script_err::kSub_InvalidName,
                       "FScriptHost::RegisterNative: invalid function name");
    }

    usize existing_index = static_cast<usize>(-1);
    for (usize i = 0u; i < m_Natives.Size(); ++i) {
        if (CStrEquals(m_Natives[i].name, function_name)) {
            existing_index = i;
            break;
        }
    }

    const bool replacing = existing_index != static_cast<usize>(-1);
    FNativeEntry previous{};
    if (replacing) {
        previous = m_Natives[existing_index];
        m_Natives[existing_index].fn = fn;
        m_Natives[existing_index].user = user;
    } else {
        if (m_Natives.Size() >= static_cast<usize>(kMaxScriptNativeFunctions)) {
            return ACS_ERR(Container, script_err::kSub_RegistryLimit,
                           "FScriptHost::RegisterNative: registry limit exceeded");
        }
        if (!m_Natives.TryReserve(m_Natives.Size() + 1u)) {
            return ACS_ERR(Memory, script_err::kSub_AllocationFailed,
                           "FScriptHost::RegisterNative: registry allocation failed");
        }
        FNativeEntry entry{};
        for (u32 i = 0u; i <= function_name_length; ++i) {
            entry.name[i] = function_name[i];
        }
        entry.fn = fn;
        entry.user = user;
        if (!m_Natives.TryPushBack(entry)) {
            return ACS_ERR(Memory, script_err::kSub_AllocationFailed,
                           "FScriptHost::RegisterNative: registry append failed");
        }
    }

    const usize staged_index =
        replacing ? existing_index : (m_Natives.Size() - 1u);
    TResult<void> r =
        m_Vm->RegisterNativeFunction(m_Natives[staged_index].name, fn, user);
    if (r.IsErr()) {
        if (replacing) {
            m_Natives[existing_index] = previous;
        } else {
            m_Natives.PopBack();
        }
        ACS_LOG_WARN("FScriptHost::RegisterNative: backend rejected '%s'; cache rolled back",
                     function_name);
        FireError("<register>", 0, r.Error().message);
        return r;
    }
    return Ok();
}

/** RegisterStandardBindings の実装 (現状プレースホルダ、vm 未設定なら警告して何もしない)。 */
void FScriptHost::RegisterStandardBindings() noexcept {
    // 現状はプレースホルダ。将来的に以下のような binding 群を
    // 一括登録する設計:
    //   ・Log.Info / Log.Warn / Log.Error / Log.Debug
    //   ・Math.Sin / Math.Cos / Math.Sqrt / Math.Clamp / Math.Lerp
    //   ・Time.Now / Time.DeltaSeconds / Time.Frame
    //   ・Input.IsKeyDown / Input.GetMouseX / Input.GetMouseY
    //   ・Audio.PlaySe / Audio.PlayBgm / Audio.StopBgm
    //   ・Scene.SpawnEntity / Scene.GetEntityById / Scene.Destroy
    //
    // 各 Pillar が backend 側で「自分の binding」を `FScriptHost::RegisterNative`
    // で登録するインターフェースを別途持ち、ここはその dispatch を呼ぶだけに
    // 留める想定 (= 本 module が全 Pillar に依存しないようにする)。
    if (m_Vm == nullptr) {
        // Init 前の呼び出しは事故 (FGame の startup 順序を疑え)。
        ACS_LOG_WARN("FScriptHost::RegisterStandardBindings: vm not initialized; skipping");
        return;
    }
    // 現状は何も登録しない (= bindings 数 0)。binding 実装は
    // Pillar 別タスクで埋める。
}

/** RegisteredNativeCount の実装 (内部キャッシュの件数を返す)。 */
u32 FScriptHost::RegisteredNativeCount() const noexcept {
    return static_cast<u32>(m_Natives.Size());
}

/** 登録済みnative functionを名前で取得する。 */
bool FScriptHost::TryGetRegisteredNative(const char*     function_name,
                                         NativeFunction& out_fn,
                                         void*&          out_user) const noexcept {
    u32 function_name_length = 0u;
    if (!TryGetBoundedLength(function_name,
                             kMaxScriptFunctionNameBytes,
                             function_name_length) ||
        function_name_length == 0u) {
        return false;
    }
    for (usize i = 0u; i < m_Natives.Size(); ++i) {
        if (CStrEquals(m_Natives[i].name, function_name)) {
            out_fn = m_Natives[i].fn;
            out_user = m_Natives[i].user;
            return true;
        }
    }
    return false;
}

/** SetOnErrorCallback の実装 (callback と user コンテキストを保存)。 */
void FScriptHost::SetOnErrorCallback(ScriptErrorCallback cb, void* user) noexcept {
    m_OnError      = cb;
    m_OnErrorUser = user;
}

/** FireError の実装 (callback 未設定なら no-op、設定済みなら通知)。 */
void FScriptHost::FireError(const char* chunk_name, u32 line, const char* message) const noexcept {
    if (m_OnError == nullptr) return;
    // callback は noexcept 必須 (= 例外伝搬しない、scripting backend 由来の
    // C スタックを traverse しないため)。
    m_OnError(m_OnErrorUser, chunk_name, line, message);
}

} // namespace acs::game
