// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/ScriptHost.h"

namespace acs {
/** 動的な保存領域を確保する型。 */
class IAllocator;
}

namespace acs::scripting {

/** Lua 5.4 の実行状態と登録関数を管理する。 */
class CLuaVm final : public acs::game::IScriptVm {
public:
    /** 空状態で構築する (lua_State は Init で生成)。 */
    CLuaVm() noexcept;

    /**
     * 指定 allocator を native function 登録簿に使う空状態で構築する。
     *
     * @details CLuaVmはallocatorへの参照を内部登録簿に保持するため、allocatorは構築した
     * CLuaVmより後まで生存しなければならない。
     * @param allocator native function 登録簿の確保に使い、CLuaVmより長く生存するallocator。
     */
    explicit CLuaVm(acs::IAllocator& allocator) noexcept;

    /** Lua状態を終了して内部データを解放する。 */
    ~CLuaVm() noexcept override;

    /** コピー禁止 (lua_State を単独所有するため)。 */
    CLuaVm(const CLuaVm&)            = delete;

    /** コピー代入も禁止。 */
    CLuaVm& operator=(const CLuaVm&) = delete;

    /** 内部データの位置を固定するためムーブを禁止する。 */
    CLuaVm(CLuaVm&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CLuaVm& operator=(CLuaVm&&)      = delete;

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
    acs::TResult<void>       LoadScript(const char* source, acs::u32 source_len, const char* chunk_name) noexcept override;

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
    acs::TResult<void>       CallFunction(const char* function_name, const acs::game::FScriptValue* args, acs::u32 arg_count, acs::game::FScriptValue* ret_out) noexcept override;

    /**
     * C++ 関数を script グローバル空間に bind する。
     *
     * @details
     * native registry に (fn, user) を追加し、その index と内部データを upvalue に載せた
     * trampoline closure を function_name のグローバルへ公開する。登録領域を確保できない
     * 場合はLuaのグローバルを変更せず、登録番号も消費しない。Lua側の公開は保護呼び出しで実行し、同じclosureを
     * 関数名から取得できた場合だけ確定する。失敗時はstackとnative registry要素を戻し、
     * 同名globalの復元も試みる。復元処理もLuaのメモリ不足になった場合は同名globalの値を
     * 保証しない。Lua公開処理または注入allocatorから同じ実行環境へ再入した登録は、状態を
     * 変えずに失敗させる。公開失敗時にLua側へ退避されたclosureは、登録ごとの識別番号で
     * 後続登録と区別する。最大の登録番号は一度だけ割り当て、その後は再初期化しても登録を
     * 恒久的に拒否する。
     * @param function_name script 側から見える関数名。
     * @param fn 呼び出される native 関数。
     * @param user fn の第 3 引数にそのまま渡されるコンテキスト (this 束縛用)。
     * @return 成功なら空の TResult、未初期化 / 引数不正 / 登録領域の確保失敗 / Lua公開失敗ならエラー。
     */
    acs::TResult<void>       RegisterNativeFunction(const char* function_name, acs::game::NativeFunction fn, void* user) noexcept override;

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
    acs::f64                 GetGlobalNumber(const char* name, acs::f64 default_value) const noexcept override;

    /** lua_gc(LUA_GCCOLLECT) で強制 GC を 1 サイクル走らせる (未初期化なら no-op)。 */
    void                     CollectGarbage()                                    noexcept override;

    /**
     * Lua VM が抱えるメモリ使用量を返す。
     *
     * @details lua_gc の GCCOUNT (KiB) + GCCOUNTB (端数 byte) を合算する。未初期化なら 0。
     * @return 使用メモリのバイト数。
     */
    acs::u64                 MemoryUsageBytes()                            const noexcept override;

#if defined(ACS_SCRIPTING_TEST_HOOKS)
    /** 次回のnative関数登録へ最大の識別番号を割り当てるテスト専用境界設定。 */
    void SetNextNativeRegistrationIdToMaximumForTest() noexcept;
#endif
private:
    /** Lua状態と登録関数を隠す内部データ。 */
    class FLuaVmImpl;

    /** 内部データへの所有ポインタ。 */
    FLuaVmImpl* m_Impl = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLuaVm = CLuaVm;

/**
 * プロセス共有の既定 CLuaVm singleton を返す (provider の実体)。
 *
 * @details
 * 本物の Lua 5.4 VM を Meyers singleton で 1 個だけ保持し、その参照を返す。
 * InstallLuaAsDefault が gameframework の provider にこの関数を登録する。
 * @return 共有 CLuaVm インスタンスへの IScriptVm 参照。
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
