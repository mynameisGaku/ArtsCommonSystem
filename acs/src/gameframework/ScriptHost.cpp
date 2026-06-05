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
 * LoadAndRun の読み込み上限 (64 MiB)。
 *
 * @details 実用的なスクリプト 1 本でこれを超えるのは「ファイルが壊れている / バイナリを掴んだ」と判断して即座にエラーにする。
 */
inline constexpr u64 kMaxScriptFileBytes = 64ull * 1024ull * 1024ull;  // 64 MiB

} // anonymous namespace

/** Init の実装 (副作用ゼロで成功、initialized フラグを立てる)。 */
TResult<void> ScriptVmStub::Init() noexcept {
    // Stub は副作用ゼロで成功させる (= 起動シーケンスを通すため)。
    // 実際のスクリプト動作は LoadScript / CallFunction の段階で
    // NotImplemented として落とす。
    m_Initialized = true;
    return Ok();
}

/** Shutdown の実装 (no-op、initialized フラグを下げる)。 */
void ScriptVmStub::Shutdown() noexcept {
    // 何も保持していないので no-op。Init 前に呼ばれても安全。
    m_Initialized = false;
}

/** LoadScript の実装 (Init 前は kSub_NotInitialized、それ以外は kSub_NotImplemented)。 */
TResult<void> ScriptVmStub::LoadScript(const char* /*source*/,
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
TResult<void> ScriptVmStub::CallFunction(const char* /*function_name*/,
                                        const ScriptValue* /*args*/, u32 /*arg_count*/,
                                        ScriptValue* /*ret_out*/) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized,
                       "ScriptVmStub::CallFunction called before Init()");
    }
    return ACS_ERR(Generic, script_err::kSub_NotImplemented,
                   "ScriptVmStub::CallFunction: scripting backend not integrated (Phase N-2 stub)");
}

/** RegisterNativeFunction の実装 (Init 前は kSub_NotInitialized、それ以外は kSub_NotImplemented)。 */
TResult<void> ScriptVmStub::RegisterNativeFunction(const char* /*function_name*/,
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
void ScriptVmStub::SetGlobalNumber(const char* /*name*/, f64 /*value*/) noexcept {
    // 値を保存する場所が無いので no-op (= 後から GetGlobalNumber しても default
    // が返る)。上位コードは「stub では持続しない」前提で書く。
}

/** GetGlobalNumber の実装 (常に default_value を返す)。 */
f64 ScriptVmStub::GetGlobalNumber(const char* /*name*/, f64 default_value) const noexcept {
    return default_value;
}

/** CollectGarbage の実装 (GC 対象が無いので no-op)。 */
void ScriptVmStub::CollectGarbage() noexcept {
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
    static ScriptVmStub instance;
    return instance;
}

namespace {
/** 登録済みの既定 VM provider (未登録なら nullptr、実 backend の Install* で設定される)。 */
ScriptVmProvider g_ScriptVmProvider = nullptr;
}

/** SetScriptVmProvider の実装 (provider を上書き保存、後勝ち)。 */
void SetScriptVmProvider(ScriptVmProvider provider) noexcept {
    g_ScriptVmProvider = provider;
}

/** GetDefaultScriptVm の実装 (provider 登録済みならその実 VM、未登録なら GetVmStub)。 */
IScriptVm& GetDefaultScriptVm() noexcept {
    return g_ScriptVmProvider ? g_ScriptVmProvider() : GetVmStub();
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

/** LoadAndRun の実装 (Win32 でファイルを読み込み IScriptVm::LoadScript へ流す)。 */
TResult<void> FScriptHost::LoadAndRun(const wchar_t* file_path) noexcept {
    // 事前チェック。
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::LoadAndRun: vm not initialized (call Init(vm) first)");
    }
    if (file_path == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::LoadAndRun: file_path is null");
    }

    // Win32 で読み込み。FSaveArchive.cpp と同じ流儀 (CreateFileW + GetFileSizeEx + ReadFile)。
    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_READ,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, script_err::kSub_FileNotFound,
                          "FScriptHost::LoadAndRun: CreateFileW failed", err);
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(h, &size_li)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, script_err::kSub_FileNotFound,
                          "FScriptHost::LoadAndRun: GetFileSizeEx failed", err);
    }

    const u64 size_u64 = static_cast<u64>(size_li.QuadPart);
    if (size_u64 == 0) {
        // 空ファイル → 空 chunk として LoadScript を呼ぶ (backend が許容する想定)。
        ::CloseHandle(h);
        return m_Vm->LoadScript("", 0, "<empty>");
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
        return ACS_ERR(Memory, script_err::kSub_FileTooLarge,
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
            return ACS_ERR_OS(IO, script_err::kSub_FileNotFound,
                              "FScriptHost::LoadAndRun: ReadFile failed", io_err);
        }
        p         += got;
        remaining -= got;
    }
    ::CloseHandle(h);

    // backend に渡す。
    // chunk_name は wide path を ASCII 化するのが面倒なので、
    // 固定文字列で代用する。実 Lua backend では caller から
    // explicit chunk name を取る overload を追加予定。
    const u32 src_len = (buf_size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<u32>(buf_size);
    TResult<void> r = m_Vm->LoadScript(reinterpret_cast<const char*>(buf),
                                     src_len,
                                     "<file>");

    // backend が source を内部で複製しているはず (Lua の luaL_loadbuffer は
    // const char* を即時パースして AST 化するので、戻った時点でバッファは
    // 解放してよい)。stub も中身を見ないのでここで解放安全。
    alloc.Free(raw);

    if (r.IsErr()) {
        FireError("<file>", 0, r.Error().message);
    }
    return r;
}

/** CallGlobalFunction の実装 (引数を防御チェックして vm->CallFunction へ委譲)。 */
TResult<void> FScriptHost::CallGlobalFunction(const char*        function_name,
                                            const ScriptValue* args,
                                            u32                arg_count,
                                            ScriptValue*       ret_out) noexcept {
    if (m_Vm == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_NoVm,
                       "FScriptHost::CallGlobalFunction: vm not initialized (call Init(vm) first)");
    }
    if (function_name == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::CallGlobalFunction: function_name is null");
    }
    // arg_count > 0 のときは args が null だと壊れるので防御。
    if (arg_count > 0 && args == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::CallGlobalFunction: arg_count > 0 but args is null");
    }
    TResult<void> r = m_Vm->CallFunction(function_name, args, arg_count, ret_out);
    if (r.IsErr()) {
        FireError(function_name, 0, r.Error().message);
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
    if (function_name == nullptr || fn == nullptr) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg,
                       "FScriptHost::RegisterNative: function_name or fn is null");
    }

    // 同名既登録なら entry を上書きする (= 内部 registry は名前ユニーク前提)。
    bool replaced = false;
    for (u32 i = 0; i < m_Natives.Size(); ++i) {
        if (CStrEquals(m_Natives[i].name, function_name)) {
            m_Natives[i].fn   = fn;
            m_Natives[i].user = user;
            replaced         = true;
            break;
        }
    }

    // vm 側にも登録 (backend が同名上書きをどう扱うかは backend 依存)。
    TResult<void> r = m_Vm->RegisterNativeFunction(function_name, fn, user);
    if (r.IsErr()) {
        // backend 失敗時は本 host の registry にも追加しない (= 巻き戻し)。
        // 既に上書き済みだった場合は中途半端な状態を避けるため、巻き戻しは
        // 行わない (上書き前に snapshot を取るコストが見合わない)。warn ログ
        // だけ残す。
        if (!replaced) {
            ACS_LOG_WARN("FScriptHost::RegisterNative: backend rejected '%s' (not cached)",
                         function_name);
        } else {
            ACS_LOG_WARN("FScriptHost::RegisterNative: backend rejected '%s' but local cache was overwritten",
                         function_name);
        }
        FireError("<register>", 0, r.Error().message);
        return r;
    }

    // 新規 entry は末尾追加。
    if (!replaced) {
        NativeEntry e{};
        e.name = function_name;
        e.fn   = fn;
        e.user = user;
        m_Natives.PushBack(e);
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
