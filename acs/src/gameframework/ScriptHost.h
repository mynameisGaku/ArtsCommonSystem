// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — CScriptHost (Lua / Wren / Python scripting seam)
//
// 役割:
//   Pillar N (Modding / Scripting) のうち、ゲームロジックを動的に拡張するための
//   スクリプトランタイム (Lua 5.4 / Wren / Python3 / 自前 VM 等) を **interface
//   seam** として隔離する。`CModRegistry` が「Mod の有効化と並び順」を管理する
//   メタデータレイヤであるのに対し、`CScriptHost` は「実際にスクリプトを load /
//   call し、native 関数を bind する」実行レイヤを担当する。
//
//   GameFramework 設計書 v13 では Lua 5.4 が推奨 backend だが、本 module
//   自体は実 Lua ライブラリをリンクしない。具象 backend (CLuaVm / Wren VM 実装等) は
//   別モジュール (将来の `ACS::ScriptingLua` = `src/scripting/`) で `IScriptVm`
//   を override する形で実装される。
//
// なぜ seam で隔離するか:
//   1. **ライセンス / 配布の独立性**:
//      Lua は MIT で扱いやすいが、Wren (MIT) / Python (PSF) / mruby (MIT) は
//      それぞれ別のヘッダ + ランタイムを持つ。GameFramework 本体に直接リンク
//      すると配布形態が縛られるため、interface だけを本層に置く。
//   2. **テスト容易性**:
//      `IScriptVm` の純粋仮想ポインタを差し替えるだけで mock backend に置換
//      でき、CI で Lua ライブラリを持たないマシンでも上位層 (native 関数登録 /
//      CScriptHost ワークフロー) の試験を回せる。
//   3. **決定論ゾーンの隔離**:
//      Pillar B/C/F (sim / collision / physics) は固定タイムステップで
//      決定論を保証する。スクリプト実行 (= GC / JIT / native callback) は
//      決定論を保証できないので、`CScriptHost` を呼ぶのは「決定論を捨ててよい所」
//      (UI / イベントトリガー / カットシーン / Mod hook) に限定する規約を
//      API レベルで強制する (= 本 header の呼び出し位置で発見できる)。
//   4. **defensive な未統合フォールバック**:
//      実 Lua backend が同梱されないビルドでも、`CScriptVmStub` が
//      `kSub_NotImplemented` を返すことで、Mod / scripting に依存する path が
//      「常にスクリプト無し」状態でも安全に動く前提を強制できる。
//
// 本 header が提供するもの:
//   ・`IScriptVm` の純粋仮想 interface 確定 (Init / Shutdown / LoadScript /
//     CallFunction / RegisterNativeFunction / GetGlobal / SetGlobal / GC)
//   ・タグ付き union 風 POD `FScriptValue` + `FScriptCallFrame` + `NativeFunction`
//     による backend 中立な値受け渡し
//   ・`CScriptVmStub` (全 method NotImplemented を返す defensive 実装)
//   ・global stub アクセサ `GetVmStub()`
//   ・`CScriptHost` 高レベルラッパ (vm を保持 + native 関数 registry + file load
//     + 標準 binding 一括登録)
//
// ACS 規約遵守:
//   ・STL 不使用 / `<string>` 不使用 (文字列は `const char*` のみ)
//   ・例外不使用、エラーは `TResult<T, FErrorCode>` で伝搬
//   ・全 noexcept
//   ・`CScriptHost` / `CScriptVmStub` は シングルトン / 単一所有運用前提で
//     コピー / ムーブ禁止
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * スクリプト backend を識別するタグ。
 *
 * @details
 * IScriptVm::Language() が返す。複数 backend を同時運用する場合 (例: UI ロジックは
 * Lua、ML 推論 hook だけ Python) に、CScriptHost 側で 1 個の vm を選択するための
 * 判定に使う。Lua54 推奨。
 */
enum class EScriptLanguage : u8 {
    /** Lua 5.4 (推奨、GameFramework v13 設計書のデフォルト)。 */
    Lua54   = 0,

    /** Wren (小規模、組み込み向け、class ベース)。 */
    Wren    = 1,

    /** CPython 3.x (重量級、外部 SDK 多数)。 */
    Python3 = 2,

    /** ユーザー実装 (アプリ側で IScriptVm 派生クラスを差し込み)。 */
    Custom  = 3,
};

/**
 * FScriptValue が現在保持している値の種別タグ。
 *
 * @details
 * backend 中立な値受け渡し用。Lua/Wren/Python いずれの native 関数も、引数 / 戻り値は
 * 「動的型付け値の配列」として表現される。本 enum はその最大公約数を表す。
 */
enum class EScriptValueKind : u8 {
    /** 値なし (default 状態 / 戻り値なしを表現)。 */
    Nil    = 0,

    /** 真偽値。 */
    Bool   = 1,

    /** 数値 (f64 で統一、Lua の lua_Number と同じ精度。整数も f64 に丸める)。 */
    Number = 2,

    /** 文字列 (const char*、所有しない。寿命は呼び出し側が保証)。 */
    String = 3,

    /** 不透明ハンドル (u32、entity / scene node / save slot 等のゲーム側 ID)。 */
    Handle = 4,
};

/**
 * backend 中立な値受け渡し用のタグ付き union 風 POD。
 *
 * @details
 * 共用体にしているのは「同時には 1 値しか保持しない」という意味合いを表すためで、
 * サイズ上は struct でも変わらない (u64 1 個分にフィットする)。
 */
struct FScriptValue {
    /** 現在 v が保持している値の種別。 */
    EScriptValueKind kind = EScriptValueKind::Nil;

    /** kind に対応する 1 値だけが有効な共用体本体。 */
    union {
        /** kind == Bool のときの真偽値。 */
        bool        b;

        /** kind == Number のときの数値。 */
        f64         num;

        /** kind == FString のときの文字列 (非所有)。 */
        const char* str;

        /** kind == Handle のときの不透明 ID。 */
        u32         handle;
    } v;
};

/**
 * native function 呼び出し時の引数 / 戻り値ホルダ。
 *
 * @details
 * script 側から native 関数を call した瞬間に backend が組み立てて NativeFunction に
 * 渡すペイロード。POD ポインタ + 件数だけを保持し、args / ret のメモリは backend
 * (スタック / 仮 buffer) が所有する。
 */
struct FScriptCallFrame {
    /** 引数配列 (immutable、長さは arg_count)。 */
    const FScriptValue* args      = nullptr;

    /** args の長さ (0 でも有効 = 引数なし関数)。 */
    u32                arg_count = 0;

    /** 戻り値書き込み先 (nullptr 可 = 戻り値を捨てる)。native は kind / v を上書きしてよい。 */
    FScriptValue*       ret       = nullptr;
};

class IScriptVm;

/**
 * script 側から呼べる C++ 関数の signature。
 *
 * @details
 * user は RegisterNativeFunction 登録時のコンテキストポインタがそのまま流れてくる
 * (this 束縛用の trampoline パターン)。例外を投げてはならない (script backend は
 * C 由来で stack unwind 安全性を持たないことが多い)。
 */
using NativeFunction = void(*)(IScriptVm& vm, FScriptCallFrame& frame, void* user) noexcept;

/**
 * 本 module 内で固定する安定 subcode の名前空間。
 *
 * @details
 * EErrCategory::Generic 配下で subcode を固定し、上位層が err.subcode ==
 * script_err::kSub_NotImplemented などで分岐できるようにする。SaveSlot.h /
 * MlRuntime.h と同じ流儀。
 */
namespace script_err {
    /** stub / backend 未統合。 */
    inline constexpr u16 kSub_NotImplemented = 99;

    /** nullptr / 不正引数。 */
    inline constexpr u16 kSub_InvalidArg     = 1;

    /** Init() 前の API 呼び出し。 */
    inline constexpr u16 kSub_NotInitialized = 2;

    /** LoadScript 失敗 (parse error 等)。 */
    inline constexpr u16 kSub_LoadFailed     = 10;

    /** CallFunction 失敗 (runtime error 等)。 */
    inline constexpr u16 kSub_CallFailed     = 11;

    /** LoadAndRun の file 読み込み失敗。 */
    inline constexpr u16 kSub_FileNotFound   = 20;

    /** ファイルが内部上限を超える。 */
    inline constexpr u16 kSub_FileTooLarge   = 21;

    /** ファイルの open/read/close または snapshot 整合性確認に失敗。 */
    inline constexpr u16 kSub_Io             = 22;

    /** 一時バッファまたは native registry の確保に失敗。 */
    inline constexpr u16 kSub_AllocationFailed = 23;

    /** 読み込み中にファイルの長さが変化した。 */
    inline constexpr u16 kSub_FileChanged    = 24;

    /** スクリプトsourceに埋込みNULが含まれる。 */
    inline constexpr u16 kSub_EmbeddedNul    = 25;

    /** ファイルパスがnull、空、長すぎる、またはNUL終端されていない。 */
    inline constexpr u16 kSub_InvalidPath    = 26;

    /** CScriptHost::Init 未呼出 (vm 未設定)。 */
    inline constexpr u16 kSub_NoVm           = 30;

    /** 関数名が空、長すぎる、またはNUL終端されていない。 */
    inline constexpr u16 kSub_InvalidName    = 31;

    /** 呼び出し引数数または文字列引数量が上限を超える。 */
    inline constexpr u16 kSub_ArgumentLimit  = 32;

    /** native function registry が上限に達した。 */
    inline constexpr u16 kSub_RegistryLimit  = 33;
} // namespace script_err

/** 1スクリプトファイル/sourceの最大バイト数 (64 MiB)。 */
inline constexpr u64 kMaxScriptFileBytes = 64ull * 1024ull * 1024ull;

/** 関数名の最大バイト数 (終端NULを含まない)。 */
inline constexpr u32 kMaxScriptFunctionNameBytes = 127u;

/** source診断に渡すchunk名の最大バイト数。 */
inline constexpr u32 kMaxScriptChunkNameBytes = 255u;

/** script file pathの最大UTF-16 code unit数。 */
inline constexpr u32 kMaxScriptPathChars = 1023u;

/** 1回のscript callに渡せる引数数。 */
inline constexpr u32 kMaxScriptCallArguments = 1024u;

/** 1回のscript callに含められる文字列引数の合計バイト数。 */
inline constexpr u32 kMaxScriptStringArgumentBytes = 1024u * 1024u;

/** 1 hostが保持できるnative function登録数。 */
inline constexpr u32 kMaxScriptNativeFunctions = 4096u;

/**
 * スクリプト VM の純粋仮想 interface (backend 差し替え用 seam)。
 *
 * @details
 * Lua 5.4 / Wren / Python3 / 自前 VM いずれかの backend を差し替えるための seam。
 * 全 method はメイン (描画 / フレーム) スレッドから呼ばれる前提で、1 つの instance は
 * シングルスレッドでのみ使う (並列実行したい場合は instance を複数持つ)。
 * ライフタイム契約は Init() → (LoadScript / CallFunction / RegisterNativeFunction /
 * Get/Set/GC)* → Shutdown()。Init() 未呼出での API は失敗を返してよく、Shutdown()
 * 後の再 Init() は実装定義。
 */
class IScriptVm {
public:
    /** 派生 backend を正しく破棄するための仮想デストラクタ。 */
    virtual ~IScriptVm() noexcept = default;

    /**
     * backend を初期化する (lua_State 作成 / 標準ライブラリ open 等)。
     *
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    virtual TResult<void> Init() noexcept = 0;

    /** backend を破棄する (lua_close、native registration もこの時点で無効化)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * backend 識別タグを返す。
     *
     * @return この VM の EScriptLanguage。
     */
    virtual EScriptLanguage Language() const noexcept = 0;

    /**
     * ソース文字列を chunk としてロード + 即時実行する (Lua の luaL_dostring 相当)。
     *
     * @param source 実行するスクリプトソース (UTF-8 想定、所有しない)。
     * @param source_len source の有効バイト長 (NUL 終端を含まない)。0 は空 chunk として成功扱い推奨。
     * @param chunk_name エラーメッセージで使う表示名 (nullptr なら backend のデフォルト)。
     * @return 成功なら空の TResult、parse / 実行失敗ならエラー。
     */
    virtual TResult<void> LoadScript(const char* source,
                                    u32         source_len,
                                    const char* chunk_name) noexcept = 0;

    /**
     * グローバル関数を呼び出す。
     *
     * @param function_name 呼び出すグローバル関数名。
     * @param args 渡す引数配列 (nullptr + arg_count 0 で引数なし)。
     * @param arg_count args の要素数。
     * @param ret_out 戻り値書き込み先 (nullptr で捨てる)。
     * @return 成功なら空の TResult、未登録関数 / 実行時エラーは kSub_CallFailed。
     */
    virtual TResult<void> CallFunction(const char*        function_name,
                                      const FScriptValue* args,
                                      u32                arg_count,
                                      FScriptValue*       ret_out) noexcept = 0;

    /**
     * C++ 関数を script グローバル空間に bind する。
     *
     * @param function_name script 側から見える関数名。
     * @param fn 呼び出される native 関数。
     * @param user fn の第 3 引数にそのまま渡されるコンテキスト (this 束縛用)。
     * @return 成功なら空の TResult、失敗ならエラー (同名既登録の扱いは backend 依存)。
     */
    virtual TResult<void> RegisterNativeFunction(const char*    function_name,
                                                NativeFunction fn,
                                                void*          user) noexcept = 0;

    /**
     * グローバル数値変数を設定する (script 側から name で参照可能になる)。
     *
     * @param name グローバル変数名。
     * @param value 設定する数値。
     */
    virtual void SetGlobalNumber(const char* name, f64 value) noexcept = 0;

    /**
     * グローバル数値変数を取得する。
     *
     * @param name グローバル変数名。
     * @param default_value 未定義 / 型不一致時に返す値。
     * @return 取得した数値 (取得できなければ default_value)。
     */
    virtual f64 GetGlobalNumber(const char* name, f64 default_value) const noexcept = 0;

    /**
     * 強制 GC を 1 サイクル走らせる (Lua の lua_gc(LUA_GCCOLLECT) 相当)。
     *
     * @details 決定論ゾーン外で、フレーム境界で明示的に呼ぶ運用を推奨。
     */
    virtual void CollectGarbage() noexcept = 0;

    /**
     * 現在の script VM が抱えるメモリ使用量を返す (debug HUD 用、近似で可)。
     *
     * @return 使用メモリのバイト数。
     */
    virtual u64 MemoryUsageBytes() const noexcept = 0;

protected:
    /** 派生クラスからのみ構築可能。 */
    IScriptVm() noexcept = default;

    /** コピー禁止。 */
    IScriptVm(const IScriptVm&)            = delete;

    /** コピー代入も禁止。 */
    IScriptVm& operator=(const IScriptVm&) = delete;

    /** ムーブ禁止。 */
    IScriptVm(IScriptVm&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IScriptVm& operator=(IScriptVm&&)      = delete;
};

/**
 * 全 method を NotImplemented / 安全側で返す defensive な IScriptVm 実装。
 *
 * @details
 * 実 Lua / Wren / Python backend と未統合の状態で、上位層が「スクリプト経路が常に
 * 失敗する」前提で正しく fallback を書けているかを検証するための実装 (現状唯一の
 * IScriptVm 実装)。Init / Shutdown のみ no-op 成功で起動シーケンスを通し、
 * LoadScript / CallFunction / RegisterNativeFunction は kSub_NotImplemented を返す。
 * SetGlobalNumber / GetGlobalNumber / CollectGarbage は no-op、MemoryUsageBytes は常に 0。
 */
class CScriptVmStub final : public IScriptVm {
public:
    /** 空状態で構築する。 */
    CScriptVmStub() noexcept = default;

    /** 破棄する (no-op)。 */
    ~CScriptVmStub() noexcept override = default;

    /**
     * Init を no-op 成功させる (起動シーケンスを通すため)。
     *
     * @return 常に成功 (内部 initialized フラグを立てる)。
     */
    TResult<void>    Init()                                                          noexcept override;

    /** Shutdown を no-op で処理する (initialized フラグを下げる)。 */
    void            Shutdown()                                                      noexcept override;

    /**
     * backend 識別タグを返す。
     *
     * @return 常に EScriptLanguage::Custom。
     */
    EScriptLanguage Language()                                                const noexcept override { return EScriptLanguage::Custom; }

    /**
     * LoadScript を未実装として失敗させる。
     *
     * @param source 無視される (未実装)。
     * @param source_len 無視される (未実装)。
     * @param chunk_name 無視される (未実装)。
     * @return Init 前なら kSub_NotInitialized、それ以外は kSub_NotImplemented。
     */
    TResult<void>    LoadScript(const char* source, u32 source_len,
                               const char* chunk_name)                              noexcept override;

    /**
     * CallFunction を未実装として失敗させる。
     *
     * @param function_name 無視される (未実装)。
     * @param args 無視される (未実装)。
     * @param arg_count 無視される (未実装)。
     * @param ret_out 無視される (未実装)。
     * @return Init 前なら kSub_NotInitialized、それ以外は kSub_NotImplemented。
     */
    TResult<void>    CallFunction(const char* function_name,
                                 const FScriptValue* args, u32 arg_count,
                                 FScriptValue* ret_out)                              noexcept override;

    /**
     * RegisterNativeFunction を未実装として失敗させる。
     *
     * @param function_name 無視される (未実装)。
     * @param fn 無視される (未実装)。
     * @param user 無視される (未実装)。
     * @return Init 前なら kSub_NotInitialized、それ以外は kSub_NotImplemented。
     */
    TResult<void>    RegisterNativeFunction(const char* function_name,
                                           NativeFunction fn, void* user)           noexcept override;

    /**
     * SetGlobalNumber を no-op で処理する (値を保持しない)。
     *
     * @param name 無視される (未実装)。
     * @param value 無視される (未実装)。
     */
    void            SetGlobalNumber(const char* name, f64 value)                    noexcept override;

    /**
     * GetGlobalNumber を常に default を返して処理する。
     *
     * @param name 無視される (未実装)。
     * @param default_value そのまま返す値。
     * @return 常に default_value。
     */
    f64             GetGlobalNumber(const char* name, f64 default_value)      const noexcept override;

    /** CollectGarbage を no-op で処理する (GC 対象が存在しない)。 */
    void            CollectGarbage()                                                noexcept override;

    /**
     * メモリ使用量を返す。
     *
     * @return 常に 0。
     */
    u64             MemoryUsageBytes()                                        const noexcept override { return 0; }

private:
    /** Init 済みかどうか (Init 前の API 呼び出しを検出するため)。 */
    bool m_Initialized = false;
};

/**
 * process 内で 1 個だけ存在する静的 stub への参照を返す (Meyers singleton)。
 *
 * @details CGame / AScene 側からのスクリプト問い合わせはこれを通る。
 * @return 共有 CScriptVmStub インスタンスへの参照。
 */
IScriptVm& GetVmStub() noexcept;

/**
 * 既定 IScriptVm を返す provider 関数型 (実 backend モジュールへの委譲点)。
 *
 * @details
 * gameframework は実 backend モジュール (例: ACS::Scripting / CLuaVm) に依存できない
 * (循環依存になる: backend 側が本 interface に依存する)。そこで実 backend 側が
 * SetScriptVmProvider() で「既定 VM を返す関数」を登録し、ゲームコードは
 * GetDefaultScriptVm() を通じて backend 非依存に既定 VM を取得する。典型例として
 * アプリ起動時に一度だけ WITH_ACS_SCRIPTING ガード内で
 * acs::scripting::InstallLuaAsDefault() を呼び provider を登録すると、以降はどこでも
 * GetDefaultScriptVm() で実 Lua VM が得られる。
 * @return 既定として使う IScriptVm への参照。
 */
using ScriptVmProvider = IScriptVm& (*)() noexcept;

/**
 * 既定 VM provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details 後勝ち。nullptr 登録で stub に戻す。
 * @param provider 既定 VM を返す provider 関数 (nullptr で stub に戻す)。
 */
void SetScriptVmProvider(ScriptVmProvider provider) noexcept;

/**
 * 既定 IScriptVm を返す。
 *
 * @return provider 登録済みならその実 VM、未登録なら GetVmStub()。
 */
IScriptVm& GetDefaultScriptVm() noexcept;

/**
 * IScriptVm を集約し、ゲームコードに単一の「スクリプト呼び出し窓口」を提供する高レベルラッパ。
 *
 * @details
 * IScriptVm* を 1 個保持し、DI ポイントを 1 つに絞る。登録済み native function を本ホスト側でも
 * 一覧として持ち、vm 差し替え時 (= hot-swap of backend) に再登録できる素地にする。
 * LoadAndRun(file_path) で Win32 ファイル読み込み → LoadScript の一括ヘルパを、
 * RegisterStandardBindings で標準 binding (Log / Math / Time / Input / Audio 等) の一括登録 API を
 * 提供し、script 実行中エラーを上位 UI / ログに通知する ScriptErrorCallback を保持する。
 * vm の生成 / 破棄は呼び出し側 (= CGame / AScene) が責任を持ち、CScriptHost は raw ポインタを保持する
 * だけで Shutdown でも delete しない (stub singleton をそのまま差し込んでも安全)。「現在 active な
 * スクリプト窓口は 1 つ」という規約を担保するため non-copy / non-move。
 */
class CScriptHost {
public:
    /**
     * script 実行中の error / load error を上位に通知する callback の signature。
     *
     * @details noexcept 必須 (例外を script backend の C スタックへ伝搬させない)。
     * @param user SetOnErrorCallback で渡したコンテキストポインタ。
     * @param chunk_name LoadScript / LoadAndRun に渡した名前 (nullptr 可)。
     * @param line エラー発生行 (backend 未提供時は 0)。
     * @param message backend 由来のエラーメッセージ (literal / 一時バッファ)。
     */
    using ScriptErrorCallback = void(*)(void*       user,
                                        const char* chunk_name,
                                        u32         line,
                                        const char* message) noexcept;

    /** 空状態で構築する (vm 未設定)。 */
    CScriptHost() noexcept = default;

    /** 破棄する (vm を所有しないので参照を持つだけ)。 */
    ~CScriptHost() noexcept = default;

    /** コピー禁止 (active なスクリプト窓口は 1 つの規約のため)。 */
    CScriptHost(const CScriptHost&)            = delete;

    /** コピー代入も禁止。 */
    CScriptHost& operator=(const CScriptHost&) = delete;

    /** ムーブ禁止。 */
    CScriptHost(CScriptHost&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CScriptHost& operator=(CScriptHost&&)      = delete;

    /**
     * 使用する VM を差し込む。
     *
     * @details
     * 多重呼び出し可、後勝ち。既に別 vm が設定済みなら警告ログを出して上書きし、内部の
     * native 登録キャッシュをクリアする。nullptr 渡しは Shutdown 相当 (vm を切り離し)。
     * 本クラスは vm を所有 / 破棄しない。
     * @param vm 差し込む VM (nullptr で切り離し)。
     */
    void Init(IScriptVm* vm) noexcept;

    /**
     * vm 参照を切る (vm の破棄は呼び出し側の責任)。
     *
     * @details 内部の native registration リストもクリアする (空リストになる)。error callback は残す。
     */
    void Shutdown() noexcept;

    /**
     * 現在保持している vm を返す。
     *
     * @return 保持中の VM (未 Init / Shutdown 後は nullptr)。
     */
    IScriptVm* Vm() const noexcept;

    /**
     * wide path 指定でファイルを読み込み、IScriptVm::LoadScript に流す。
     *
     * @details
     * nullptr / vm 未設定 / ファイル open 失敗等は subcode で区別する。空ファイルは空 chunk
     * として LoadScript を呼ぶ。内部読み込み上限は 64 MiB (script 1 本でこれを超えるのは
     * 異常と判断)。LoadScript がエラーを返した場合は error callback を発火する。
     * @param file_path 読み込むスクリプトファイルへの wide パス。
     * @return 成功なら空の TResult、vm 未設定 / 読み込み失敗 / 上限超過 / LoadScript 失敗ならエラー。
     */
    TResult<void> LoadAndRun(const wchar_t* file_path) noexcept;

    /**
     * 長さ付きsourceを検証してVMへロード・実行する。
     *
     * @details sourceの上限と埋込みNULをVM呼び出し前に検証する。backend失敗時は
     * error callbackを発火する。sourceは呼び出し中だけ参照される。
     * @param source UTF-8 source。source_len==0ならnullptrも許可する。
     * @param source_len sourceの有効バイト数。
     * @param chunk_name 診断名 (nullptr可)。
     * @return 成功または安定したscript_err subcode。
     */
    TResult<void> LoadAndRunSource(const char* source,
                                   u32         source_len,
                                   const char* chunk_name = nullptr) noexcept;

    /**
     * グローバル関数を呼び出す薄いラッパ (vm への単純 delegate)。
     *
     * @details vm 未設定 / 引数不正は subcode で区別し、CallFunction がエラーなら error callback を発火する。
     * @param function_name 呼び出すグローバル関数名。
     * @param args 渡す引数配列 (arg_count 0 なら nullptr 可)。
     * @param arg_count args の要素数。
     * @param ret_out 戻り値書き込み先 (nullptr で捨てる)。
     * @return 成功なら空の TResult、vm 未設定 / 引数不正 / 実行失敗ならエラー。
     */
    TResult<void> CallGlobalFunction(const char*        function_name,
                                    const FScriptValue* args,
                                    u32                arg_count,
                                    FScriptValue*       ret_out) noexcept;

    /**
     * C++ 関数を vm に登録すると同時に、本ホストの内部 registry にも記録する。
     *
     * @details
     * 同名既登録なら内部 entry を上書きする。内部registryを先にstagingし、backendが
     * 拒否した場合は新規追加・既存上書きのどちらも完全にロールバックする。
     * 失敗時はerror callbackも発火する。
     * @param function_name script 側から見える関数名。
     * @param fn 呼び出される native 関数。
     * @param user fn の第 3 引数に渡すコンテキスト (this 束縛用)。
     * @return 成功なら空の TResult、vm 未設定 / 引数不正 / backend 拒否ならエラー。
     */
    TResult<void> RegisterNative(const char*    function_name,
                                NativeFunction fn,
                                void*          user) noexcept;

    /**
     * Log / Math / Time / Input / Audio 等の標準 native function 群を一括登録する。
     *
     * @details
     * 現状はプレースホルダ (= bindings 数 0) で、各 Pillar 側の binding 実装で中身を埋める設計。
     * vm 未設定で呼ぶと警告ログを出して何もしない。
     */
    void RegisterStandardBindings() noexcept;

    /**
     * これまでに RegisterNative で登録した native function 件数を返す。
     *
     * @return 内部 registry に保持している native function の数。
     */
    u32 RegisteredNativeCount() const noexcept;

    /**
     * 登録済みnative functionを名前で取得する。
     *
     * @param function_name 検索する上限付きNUL終端名。
     * @param out_fn 成功時の関数。
     * @param out_user 成功時のcontext。
     * @return 登録済みならtrue。失敗時は出力を変更しない。
     */
    bool TryGetRegisteredNative(const char*     function_name,
                                NativeFunction& out_fn,
                                void*&          out_user) const noexcept;

    /**
     * エラー callback を設定する。
     *
     * @param cb 設定する callback (nullptr で無効化)。
     * @param user callback の第 1 引数に渡すコンテキストポインタ。
     */
    void SetOnErrorCallback(ScriptErrorCallback cb, void* user) noexcept;

private:
    /**
     * 登録済み native function の薄いキャッシュ entry。
     *
     * @details
     * backend (= vm) を差し替えても再登録できるように name / fn / user を保持する
     * (vm 自身が再登録を提供しないことが多いため)。名前はentry内へ複製し、
     * 呼び出し側文字列の寿命に依存しない。
     */
    struct FNativeEntry {
        /** script 側から見える関数名 (NUL終端、entry所有)。 */
        char           name[kMaxScriptFunctionNameBytes + 1u] = {};

        /** 呼び出される native 関数。 */
        NativeFunction fn   = nullptr;

        /** fn の第 3 引数に渡すコンテキストポインタ。 */
        void*          user = nullptr;
    };

    /**
     * エラー callback を発火する内部 helper。
     *
     * @details cb 未設定なら no-op。callback は noexcept 前提。
     * @param chunk_name エラー元の chunk 名 (nullptr 可)。
     * @param line エラー発生行 (不明なら 0)。
     * @param message backend 由来のエラーメッセージ。
     */
    void FireError(const char* chunk_name, u32 line, const char* message) const noexcept;

    /** 保持中の VM (非所有、未設定なら nullptr)。 */
    IScriptVm*           m_Vm           = nullptr;

    /** 登録済み native function のキャッシュ (vm 差し替え時の再登録用)。 */
    TArray<FNativeEntry>   m_Natives;

    /** エラー通知 callback (未設定なら nullptr)。 */
    ScriptErrorCallback  m_OnError     = nullptr;

    /** エラー callback の第 1 引数に渡すコンテキストポインタ。 */
    void*                m_OnErrorUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FScriptHost = CScriptHost;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FScriptVmStub = CScriptVmStub;

} // namespace acs::game
