// SPDX-License-Identifier: Apache-2.0
// Lua 5.4 backend (acs::game::IScriptVm の real 実装)。
//
// Lua は MIT / FetchContent 取得可能 / gating 無しなので、stub と違い
// 実ビルド + 実動作まで完全検証できる real backend。
//
// 利用例:
//   acs::scripting::FLuaVm vm;
//   if (vm.Init().IsOk()) {
//       vm.LoadScript("function add(a,b) return a+b end", 0, "inline");
//       acs::game::ScriptValue args[2];
//       args[0].kind = acs::game::EScriptValueKind::Number; args[0].v.num = 2;
//       args[1].kind = acs::game::EScriptValueKind::Number; args[1].v.num = 3;
//       acs::game::ScriptValue ret;
//       vm.CallFunction("add", args, 2, &ret);  // ret.v.num == 5
//       vm.Shutdown();
//   }
//
// 設計:
//   ・Pimpl で lua.h を public header から隠す (lua_State* を漏らさない)。
//   ・NativeFunction の bridge は lua_pushcclosure の upvalue に
//     (registry index, this) を載せる trampoline 方式。
//   ・ScriptValue <-> Lua stack 変換は Nil/Bool/Number/FString/Handle の 5 種。
//   ・全 noexcept、STL/<string> 不使用、TResult でエラー伝搬。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/ScriptHost.h"

namespace acs {
class FAllocator;
}

namespace acs::scripting {

/**
 * Lua 5.4 を backend にした acs::game::IScriptVm の実装。
 *
 * @details
 * Lua は MIT / FetchContent 取得可能 / gating 無しなので、stub と違い実ビルド +
 * 実動作まで完全検証できる real backend。Pimpl で lua.h を public header から隠し
 * (lua_State* を漏らさない)、NativeFunction の bridge は lua_pushcclosure の upvalue
 * に (registry index, this) を載せる trampoline 方式で実現する。ScriptValue <-> Lua
 * stack 変換は Nil/Bool/Number/FString/Handle の 5 種をサポートする。全 method は
 * noexcept で、STL/<string> 不使用、エラーは TResult で伝搬する。
 */
class FLuaVm final : public acs::game::IScriptVm {
public:
    /** 空状態で構築する (lua_State は Init で生成)。 */
    FLuaVm() noexcept;

    /**
     * 指定 allocator を native function 登録簿に使う空状態で構築する。
     *
     * @param allocator native function 登録簿の確保に使う allocator。
     */
    explicit FLuaVm(acs::FAllocator& allocator) noexcept;

    /** 破棄する (Shutdown を呼んで lua_State を解放してから Impl を delete)。 */
    ~FLuaVm() noexcept override;

    /** コピー禁止 (lua_State を単独所有するため)。 */
    FLuaVm(const FLuaVm&)            = delete;

    /** コピー代入も禁止。 */
    FLuaVm& operator=(const FLuaVm&) = delete;

    /** ムーブ禁止 (Impl ポインタの安定性を保つため)。 */
    FLuaVm(FLuaVm&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FLuaVm& operator=(FLuaVm&&)      = delete;

    /**
     * lua_State を生成し標準ライブラリを open する。
     *
     * @details 既に初期化済みなら何もせず成功を返す。luaL_newstate 失敗時はエラー。
     * @return 成功なら空の TResult、生成失敗 (メモリ不足) ならエラー。
     */
    acs::TResult<void>       Init()                                              noexcept override;

    /** lua_State を lua_close で破棄し、native registry と戻り文字列 anchor をリセットする。 */
    void                     Shutdown()                                          noexcept override;

    /**
     * backend 識別タグを返す。
     *
     * @return 常に EScriptLanguage::Lua54。
     */
    acs::game::EScriptLanguage Language()                                  const noexcept override {
        return acs::game::EScriptLanguage::Lua54;
    }

    /**
     * ソース文字列を chunk としてロード + 即時実行する。
     *
     * @details luaL_loadbuffer で parse し lua_pcall で実行する。source_len が 0 なら strlen で算出。
     * @param source 実行するスクリプトソース (非所有)。
     * @param source_len source の有効バイト長 (0 なら strlen で算出)。
     * @param chunk_name エラーメッセージで使う表示名 (nullptr なら "=(load)")。
     * @return 成功なら空の TResult、未初期化 / source null / parse / 実行失敗ならエラー。
     */
    acs::TResult<void>       LoadScript(const char* source, acs::u32 source_len,
                                        const char* chunk_name)                  noexcept override;

    /**
     * グローバル関数を呼び出す。
     *
     * @details
     * 戻り値が文字列のとき、Lua スタックを pop すると GC 対象になるため registry へ
     * anchor し、次の CallFunction / Shutdown まで const char* を有効に保つ。
     * @param function_name 呼び出すグローバル関数名。
     * @param args 渡す引数配列 (arg_count 0 なら nullptr 可)。
     * @param arg_count args の要素数。
     * @param ret_out 戻り値書き込み先 (nullptr で捨てる、その場合 0 値返し)。
     * @return 成功なら空の TResult、未初期化 / 引数不正 / 非関数 / 実行失敗ならエラー。
     */
    acs::TResult<void>       CallFunction(const char* function_name,
                                          const acs::game::ScriptValue* args,
                                          acs::u32 arg_count,
                                          acs::game::ScriptValue* ret_out)       noexcept override;

    /**
     * C++ 関数を script グローバル空間に bind する。
     *
     * @details
     * native registry に (fn, user) を追加し、その index と Impl* を upvalue に載せた
     * trampoline closure を function_name のグローバルに setglobal する。
     * @param function_name script 側から見える関数名。
     * @param fn 呼び出される native 関数。
     * @param user fn の第 3 引数にそのまま渡されるコンテキスト (this 束縛用)。
     * @return 成功なら空の TResult、未初期化 / 引数不正ならエラー。
     */
    acs::TResult<void>       RegisterNativeFunction(const char* function_name,
                                                    acs::game::NativeFunction fn,
                                                    void* user)                  noexcept override;

    /**
     * グローバル数値変数を設定する。
     *
     * @details 未初期化 / name null のときは何もしない。
     * @param name グローバル変数名。
     * @param value 設定する数値。
     */
    void                     SetGlobalNumber(const char* name, acs::f64 value)   noexcept override;

    /**
     * グローバル数値変数を取得する。
     *
     * @details 未初期化 / name null / 数値でないときは default_value を返す。
     * @param name グローバル変数名。
     * @param default_value 未定義 / 型不一致時に返す値。
     * @return 取得した数値 (取得できなければ default_value)。
     */
    acs::f64                 GetGlobalNumber(const char* name,
                                             acs::f64 default_value)       const noexcept override;

    /** lua_gc(LUA_GCCOLLECT) で強制 GC を 1 サイクル走らせる (未初期化なら no-op)。 */
    void                     CollectGarbage()                                    noexcept override;

    /**
     * Lua VM が抱えるメモリ使用量を返す。
     *
     * @details lua_gc の GCCOUNT (KiB) + GCCOUNTB (端数 byte) を合算する。未初期化なら 0。
     * @return 使用メモリのバイト数。
     */
    acs::u64                 MemoryUsageBytes()                            const noexcept override;

private:
    /** lua_State* と native registry を隠す Pimpl 本体 (lua.h を public header から隠す)。 */
    struct Impl;

    /** Pimpl 本体へのポインタ (構築時に new、破棄時に delete)。 */
    Impl* m_Impl = nullptr;
};

/**
 * プロセス共有の既定 FLuaVm singleton を返す (provider の実体)。
 *
 * @details
 * 本物の Lua 5.4 VM を Meyers singleton で 1 個だけ保持し、その参照を返す。
 * InstallLuaAsDefault が gameframework の provider にこの関数を登録する。
 * @return 共有 FLuaVm インスタンスへの IScriptVm 参照。
 */
acs::game::IScriptVm& GetDefaultLuaVm() noexcept;

/**
 * gameframework の ScriptVm provider に GetDefaultLuaVm を登録する。
 *
 * @details
 * アプリ起動時に一度だけ呼ぶと、以降 acs::game::GetDefaultScriptVm() が本物の
 * Lua 5.4 VM (プロセス共有 singleton) を返すようになる。これにより上位コードは
 * backend 非依存に GetDefaultScriptVm() だけで実 VM を取得でき、#if WITH_ACS_SCRIPTING
 * はこの Install 呼び出し 1 箇所だけで済む。
 */
void InstallLuaAsDefault() noexcept;

} // namespace acs::scripting
