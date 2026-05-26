// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar N — ScriptHost (Lua / Wren / Python scripting seam)
//
// 役割:
//   Pillar N (Modding / Scripting) のうち、ゲームロジックを動的に拡張するための
//   スクリプトランタイム (Lua 5.4 / Wren / Python3 / 自前 VM 等) を **interface
//   seam** として隔離する。`ModRegistry` が「Mod の有効化と並び順」を管理する
//   メタデータレイヤであるのに対し、`ScriptHost` は「実際にスクリプトを load /
//   call し、native 関数を bind する」実行レイヤを担当する。
//
//   GameFramework 設計書 v13 では Lua 5.4 が推奨 backend だが、本 module
//   自体は実 Lua ライブラリをリンクしない。具象 backend (LuaVm / WrenVm 等) は
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
//      ScriptHost ワークフロー) の試験を回せる。
//   3. **決定論ゾーンの隔離**:
//      Pillar B/C/F (sim / collision / physics) は固定タイムステップで
//      決定論を保証する。スクリプト実行 (= GC / JIT / native callback) は
//      決定論を保証できないので、`ScriptHost` を呼ぶのは「決定論を捨ててよい所」
//      (UI / イベントトリガー / カットシーン / Mod hook) に限定する規約を
//      API レベルで強制する (= 本 header の呼び出し位置で発見できる)。
//   4. **defensive な未統合フォールバック**:
//      実 Lua backend が同梱されないビルドでも、`ScriptVmStub` が
//      `kSub_NotImplemented` を返すことで、Mod / scripting に依存する path が
//      「常にスクリプト無し」状態でも安全に動く前提を強制できる。
//
// Phase N-2 (本フェーズ) で提供するもの:
//   ・`IScriptVm` の純粋仮想 interface 確定 (Init / Shutdown / LoadScript /
//     CallFunction / RegisterNativeFunction / GetGlobal / SetGlobal / GC)
//   ・タグ付き union 風 POD `ScriptValue` + `ScriptCallFrame` + `NativeFunction`
//     による backend 中立な値受け渡し
//   ・`ScriptVmStub` (全 method NotImplemented を返す defensive 実装)
//   ・global stub アクセサ `GetVmStub()`
//   ・`ScriptHost` 高レベルラッパ (vm を保持 + native 関数 registry + file load
//     + 標準 binding 一括登録)
//
// Phase N-3+ で行うこと (本 header の範囲外):
//   ・実 Lua 5.4 backend (`scripting/LuaVm.{h,cpp}` を別モジュール
//     `ACS::ScriptingLua` で外出し)。lua_State* / lua_newstate / lua_call /
//     lua_pcall / luaL_register を `IScriptVm` 越しに包む。
//   ・coroutine / 非同期 script (lua_yield / lua_resume)
//   ・sandbox (CPU 時間制限 / メモリ制限 / disk アクセス制限 / OS call 禁止)
//   ・型付き binding generator (script 側で型安全に native API を叩く)
//   ・hot-reload (LoadScript 再呼出時の差分マージ)
//
// ACS 規約遵守:
//   ・STL 不使用 / `<string>` 不使用 (文字列は `const char*` のみ)
//   ・例外不使用、エラーは `TResult<T, FErrorCode>` で伝搬
//   ・全 noexcept
//   ・`ScriptHost` / `ScriptVmStub` は シングルトン / 単一所有運用前提で
//     コピー / ムーブ禁止
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// =============================================================================
// EScriptLanguage — backend 識別タグ
// -----------------------------------------------------------------------------
// `IScriptVm::Language()` が返す。複数 backend を同時運用する場合 (例えば
// UI ロジックは Lua、ML 推論 hook だけ Python) に、ScriptHost 側で 1 個の
// vm を選択するための判定に使う。Phase N-2 では Lua54 推奨。
// =============================================================================
enum class EScriptLanguage : u8 {
    Lua54   = 0,   // Lua 5.4 (推奨、GameFramework v13 設計書のデフォルト)
    Wren    = 1,   // Wren (小規模、組み込み向け、class ベース)
    Python3 = 2,   // CPython 3.x (重量級、外部 SDK 多数)
    Custom  = 3,   // ユーザー実装 (アプリ側で IScriptVm 派生クラスを差し込み)
};

// =============================================================================
// EScriptValueKind / ScriptValue — タグ付き union 風 POD
// -----------------------------------------------------------------------------
// backend 中立な値受け渡し用。Lua/Wren/Python いずれも、native 関数呼び出しの
// 引数 / 戻り値は「動的型付け値の配列」として表現される。本構造はその最大公約数
// (Nil / Bool / Number / FString / Handle) を C++ 側で素朴に表現する。
//
//   ・`Nil`    : 値なし。default 状態 / 戻り値なしを表現。
//   ・`Bool`   : 真偽値。
//   ・`Number` : `f64` で統一 (Lua の lua_Number と同じ精度)。整数も f64 に
//                丸めて格納する。Wren/Python も Number は double。
//   ・`FString` : `const char*`、**所有しない**。寿命は呼び出し側 (script source
//                buffer / static literal) が保証する。STL `<string>` 禁止のため。
//   ・`Handle` : `u32`、ゲーム側で割り当てる不透明 ID (entity / scene node /
//                save slot 等)。「pointer を直接 script 側に渡さない」ための
//                安全 indirection 層。
//
// 共用体としているのは「同時には 1 値しか保持しない」という意味合いを表現する
// ためで、サイズ上は struct でも変わらない (u64 1 個分にフィットする)。
// =============================================================================
enum class EScriptValueKind : u8 {
    Nil    = 0,
    Bool   = 1,
    Number = 2,
    FString = 3,
    Handle = 4,
};

struct ScriptValue {
    EScriptValueKind kind = EScriptValueKind::Nil;
    union {
        bool        b;
        f64         num;
        const char* str;
        u32         handle;
    } v;
};

// =============================================================================
// ScriptCallFrame — native function 呼び出し時の引数 / 戻り値ホルダ
// -----------------------------------------------------------------------------
// 「native 関数を script 側から call した瞬間」に、backend が組み立てて
// NativeFunction に渡すペイロード。POD ポインタ + 件数だけを保持し、
// `args` / `ret` のメモリは backend (= スタック / 仮 buffer) が所有する。
//
//   ・`args`      : 引数配列 (immutable, count==arg_count)
//   ・`arg_count` : `args` の長さ。0 でも有効 (引数なし関数)
//   ・`ret`       : 戻り値書き込み先 (nullptr 可 = 戻り値捨て)
//                   native function は ret->kind / ret->v を上書きしてよい
// =============================================================================
struct ScriptCallFrame {
    const ScriptValue* args      = nullptr;
    u32                arg_count = 0;
    ScriptValue*       ret       = nullptr;
};

// =============================================================================
// NativeFunction — script 側から呼べる C++ 関数の signature
// -----------------------------------------------------------------------------
// `user` は `IScriptVm::RegisterNativeFunction` 登録時に渡したコンテキスト
// ポインタがそのまま流れてくる (= this ポインタ等を bind するための一般的な
// trampoline パターン)。例外を投げてはならない (script backend は C 由来で
// stack unwind 安全性を持たないことが多い)。
// =============================================================================
class IScriptVm;
using NativeFunction = void(*)(IScriptVm& vm, ScriptCallFrame& frame, void* user) noexcept;

// =============================================================================
// 安定 subcode 規約 (本 module 内で固定)
// -----------------------------------------------------------------------------
// `ErrCategory::Generic` 配下で subcode を固定し、上位層が `err.subcode ==
// script_err::kSub_NotImplemented` で分岐できるようにする。SaveSlot.h /
// MlRuntime.h と同じ流儀。
// =============================================================================
namespace script_err {
    inline constexpr u16 kSub_NotImplemented = 99;   // stub / backend 未統合
    inline constexpr u16 kSub_InvalidArg     = 1;    // nullptr / 不正引数
    inline constexpr u16 kSub_NotInitialized = 2;    // Init() 前の API 呼び出し
    inline constexpr u16 kSub_LoadFailed     = 10;   // LoadScript 失敗 (parse error 等)
    inline constexpr u16 kSub_CallFailed     = 11;   // CallFunction 失敗 (runtime error 等)
    inline constexpr u16 kSub_FileNotFound   = 20;   // LoadAndRun の file 読み込み失敗
    inline constexpr u16 kSub_FileTooLarge   = 21;   // ファイルが内部上限を超える
    inline constexpr u16 kSub_NoVm           = 30;   // ScriptHost::Init 未呼出
} // namespace script_err

// =============================================================================
// IScriptVm — スクリプト VM の純粋仮想 interface
// -----------------------------------------------------------------------------
// Lua 5.4 / Wren / Python3 / 自前 VM いずれかの backend を差し替えるための
// seam。**全 method がメイン (= 描画 / フレーム) スレッドから呼ばれる前提**。
//
// ライフタイム契約:
//   Init() → (LoadScript / CallFunction / RegisterNativeFunction / Get/Set/GC)*
//          → Shutdown()
//   ・Init() 未呼出での LoadScript / CallFunction は失敗を返してよい。
//   ・Shutdown() 後の再 Init() は実装定義 (stub は常に NotImplemented)。
//
// スレッド契約:
//   ・1 つの IScriptVm instance は **シングルスレッドでのみ** 使う。
//     並列スクリプト実行をしたい場合は instance を複数持つ。
// =============================================================================
class IScriptVm {
public:
    virtual ~IScriptVm() noexcept = default;

    // backend を初期化 (lua_State 作成 / 標準ライブラリ open 等)。
    virtual TResult<void> Init() noexcept = 0;

    // backend を破棄 (lua_close、native registration もこの時点で無効化)。
    virtual void Shutdown() noexcept = 0;

    // backend 識別タグを返す。
    virtual EScriptLanguage Language() const noexcept = 0;

    // ソース文字列を「chunk」としてロード + 即時実行 (Lua の luaL_dostring 相当)。
    // ・`source`      : 実行するスクリプトソース (UTF-8 想定)。所有しない。
    // ・`source_len`  : `source` の有効バイト長 (NUL 終端を含まない)。
    //                   0 渡しは「空 chunk」として成功扱いを推奨。
    // ・`chunk_name`  : エラーメッセージで使う表示名 (= ファイル名や lambda 名)。
    //                   nullptr の場合は backend のデフォルト ("?") を使う。
    virtual TResult<void> LoadScript(const char* source,
                                    u32         source_len,
                                    const char* chunk_name) noexcept = 0;

    // グローバル関数を呼び出す。
    // ・`args` / `arg_count`: 渡す引数 (nullptr + 0 で引数なし)
    // ・`ret_out`            : 戻り値書き込み先 (nullptr で「捨てる」)
    // ・未登録関数 / 実行時エラーは `script_err::kSub_CallFailed` で返す。
    virtual TResult<void> CallFunction(const char*        function_name,
                                      const ScriptValue* args,
                                      u32                arg_count,
                                      ScriptValue*       ret_out) noexcept = 0;

    // C++ 関数を script グローバル空間に bind する。
    // ・`user` は `NativeFunction` の第 3 引数にそのまま渡される (this 束縛用)。
    // ・同名既登録は backend 依存 (上書きの場合が多い)。
    virtual TResult<void> RegisterNativeFunction(const char*    function_name,
                                                NativeFunction fn,
                                                void*          user) noexcept = 0;

    // グローバル数値変数を設定 (script 側から `<name>` で参照可能になる)。
    virtual void SetGlobalNumber(const char* name, f64 value) noexcept = 0;

    // グローバル数値変数を取得 (未定義 / 型不一致時は `default_value`)。
    virtual f64 GetGlobalNumber(const char* name, f64 default_value) const noexcept = 0;

    // 強制 GC を 1 サイクル走らせる (Lua の lua_gc(LUA_GCCOLLECT) 相当)。
    // 決定論ゾーン外で、フレーム境界で明示的に呼ぶ運用を推奨。
    virtual void CollectGarbage() noexcept = 0;

    // 現在の script VM が抱えるメモリ使用量 (近似でよい、debug HUD 用)。
    virtual u64 MemoryUsageBytes() const noexcept = 0;

protected:
    IScriptVm() noexcept = default;
    IScriptVm(const IScriptVm&)            = delete;
    IScriptVm& operator=(const IScriptVm&) = delete;
    IScriptVm(IScriptVm&&)                 = delete;
    IScriptVm& operator=(IScriptVm&&)      = delete;
};

// =============================================================================
// ScriptVmStub — 全 method NotImplemented / 安全側を返す defensive stub
// -----------------------------------------------------------------------------
// 実 Lua / Wren / Python backend と未統合の状態で、上位層が
// 「スクリプト経路が常に失敗する」前提で正しく fallback を書けているかを
// 検証するための実装。Phase N-2 ではこれが唯一の `IScriptVm` 実装。
//
//   ・`Init` / `Shutdown` のみ no-op 成功 (= 起動シーケンスを通せる)。
//   ・`LoadScript` / `CallFunction` / `RegisterNativeFunction` は
//     `kSub_NotImplemented` を返す。
//   ・`SetGlobalNumber` / `GetGlobalNumber` / `CollectGarbage` は no-op。
//   ・`MemoryUsageBytes` は常に 0。
// =============================================================================
class ScriptVmStub final : public IScriptVm {
public:
    ScriptVmStub() noexcept = default;
    ~ScriptVmStub() noexcept override = default;

    TResult<void>    Init()                                                          noexcept override;
    void            Shutdown()                                                      noexcept override;
    EScriptLanguage Language()                                                const noexcept override { return EScriptLanguage::Custom; }
    TResult<void>    LoadScript(const char* source, u32 source_len,
                               const char* chunk_name)                              noexcept override;
    TResult<void>    CallFunction(const char* function_name,
                                 const ScriptValue* args, u32 arg_count,
                                 ScriptValue* ret_out)                              noexcept override;
    TResult<void>    RegisterNativeFunction(const char* function_name,
                                           NativeFunction fn, void* user)           noexcept override;
    void            SetGlobalNumber(const char* name, f64 value)                    noexcept override;
    f64             GetGlobalNumber(const char* name, f64 default_value)      const noexcept override;
    void            CollectGarbage()                                                noexcept override;
    u64             MemoryUsageBytes()                                        const noexcept override { return 0; }

private:
    bool _initialized = false;
};

// process 内で 1 個だけ存在する静的 stub への参照を返す (Meyers singleton)。
// Phase N-2 では `Game` / `Scene` 側からのスクリプト問い合わせはこれを通る。
IScriptVm& GetVmStub() noexcept;

// =============================================================================
// ScriptHost — IScriptVm を集約する高レベルラッパ
// -----------------------------------------------------------------------------
// 役割:
//   ・`IScriptVm*` を 1 個保持し、ゲームコード側に「スクリプト呼び出しの単一窓口」
//     を提供する (= DI ポイントを 1 つに絞る)。
//   ・登録済み native function を本ホスト側でも一覧として持ち、`Vm` 差し替え時
//     (= hot-swap of backend) に再登録できる素地にする。
//   ・`LoadAndRun(file_path)` で Win32 ファイル読み込み → LoadScript 一括ヘルパ。
//   ・標準 binding (Log / Math / Time / Input / Audio 等) を `RegisterStandardBindings`
//     で一括登録する API を提供する (Phase N-3+ で中身が埋まる予定)。
//   ・script 実行中エラーを上位 UI / ログに通知する `ScriptErrorCallback` を保持。
//
// 所有関係:
//   ・vm の生成 / 破棄は **呼び出し側 (= Game / Scene)** が責任を持つ。
//     ScriptHost は raw ポインタを保持するだけで、Shutdown でも delete しない。
//     これにより stub singleton をそのまま差し込んでも安全。
//
// 非コピー・非ムーブ:
//   ・「現在 active なスクリプト窓口は 1 つ」という規約を担保する。
// =============================================================================
class ScriptHost {
public:
    // script 実行中の error / load error を上位に通知する callback。
    // ・`chunk_name` : LoadScript / LoadAndRun に渡した名前 (nullptr 可)
    // ・`line`       : エラー発生行 (backend 未提供時は 0)
    // ・`message`    : backend 由来のエラーメッセージ (literal / 一時バッファ)
    using ScriptErrorCallback = void(*)(void*       user,
                                        const char* chunk_name,
                                        u32         line,
                                        const char* message) noexcept;

    ScriptHost() noexcept = default;
    ~ScriptHost() noexcept = default;

    ScriptHost(const ScriptHost&)            = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
    ScriptHost(ScriptHost&&)                 = delete;
    ScriptHost& operator=(ScriptHost&&)      = delete;

    // 使用する VM を差し込む。nullptr 渡しは Shutdown 相当 (vm を切り離し)。
    // 多重呼び出し可、後勝ち。本クラスは vm を所有 / 破棄しない。
    void Init(IScriptVm* vm) noexcept;

    // vm 参照を切る (vm の破棄は呼び出し側の責任)。
    // 内部の native registration リストもクリアする (空リストになる)。
    void Shutdown() noexcept;

    // 現在保持している vm を返す。未 Init / Shutdown 後は nullptr。
    IScriptVm* Vm() const noexcept;

    // wide path 指定でファイルを読み込み、`IScriptVm::LoadScript` に流す。
    // ・nullptr / vm 未設定 / ファイル open 失敗等は subcode で区別。
    // ・内部読み込み上限は 64 MiB (script 1 本でこれを超えるのは異常と判断)。
    TResult<void> LoadAndRun(const wchar_t* file_path) noexcept;

    // グローバル関数を呼び出す薄いラッパ (vm への単純 delegate)。
    TResult<void> CallGlobalFunction(const char*        function_name,
                                    const ScriptValue* args,
                                    u32                arg_count,
                                    ScriptValue*       ret_out) noexcept;

    // C++ 関数を vm に登録すると同時に、本ホストの内部 registry にも記録する。
    // 失敗時 (vm 未設定 / 引数不正 / backend 拒否) は registry にも追加しない。
    TResult<void> RegisterNative(const char*    function_name,
                                NativeFunction fn,
                                void*          user) noexcept;

    // Log / Math / Time / Input / Audio 等の標準 native function 群を
    // 一括登録する。Phase N-2 ではプレースホルダ (= bindings 数 0) として
    // 提供し、Phase N-3+ の各 Pillar 側 binding 実装で中身を埋める設計。
    void RegisterStandardBindings() noexcept;

    // これまでに `RegisterNative` で登録した native function 件数。
    u32 RegisteredNativeCount() const noexcept;

    // エラー callback を設定 (nullptr で無効化)。`user` は callback の第 1 引数。
    void SetOnErrorCallback(ScriptErrorCallback cb, void* user) noexcept;

private:
    // 登録済み native function の薄いキャッシュ。
    // backend (= vm) を差し替えても再登録できるように、name / fn / user を
    // 保持する (vm 自身が再登録を提供しないことが多いため)。
    // 名前は所有しない (literal / バンドル参照前提)。
    struct NativeEntry {
        const char*    name = nullptr;
        NativeFunction fn   = nullptr;
        void*          user = nullptr;
    };

    // 上位層 (= 内部) からエラー callback を発火する helper。cb 未設定なら no-op。
    void FireError(const char* chunk_name, u32 line, const char* message) const noexcept;

    IScriptVm*           _vm           = nullptr;
    TArray<NativeEntry>   _natives;
    ScriptErrorCallback  _on_error     = nullptr;
    void*                _on_error_user = nullptr;
};

} // namespace acs::game
