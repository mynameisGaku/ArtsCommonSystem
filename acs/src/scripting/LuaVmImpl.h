// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "scripting/LuaVm.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace acs::scripting {

namespace lua_vm_detail {

/** 実行時に割り当てられる登録番号の上限。 */
inline constexpr lua_Integer kNativeRegistrationIdMaximum = LUA_MAXINTEGER;

static_assert(kNativeRegistrationIdMaximum > 0);

/**
 * 登録番号を進め、上限割り当て後は恒久的な枯渇値0へ移す。
 * @param current 今回割り当てた登録番号。
 * @return 次回の登録番号。上限到達済みまたは枯渇済みなら0。
 */
constexpr lua_Integer AdvanceNativeRegistrationId(lua_Integer current) noexcept {
    if (current <= 0 || current >= kNativeRegistrationIdMaximum) {
        return 0;
    }
    return current + 1;
}

static_assert(AdvanceNativeRegistrationId(0) == 0);
static_assert(AdvanceNativeRegistrationId(kNativeRegistrationIdMaximum) == 0);
static_assert(kNativeRegistrationIdMaximum == 1 || AdvanceNativeRegistrationId(1) == 2);
static_assert(kNativeRegistrationIdMaximum == 1 || AdvanceNativeRegistrationId(kNativeRegistrationIdMaximum - 1) == kNativeRegistrationIdMaximum);

} // namespace lua_vm_detail

/** CLuaVmが所有するLua状態と登録関数を保持する。 */
class CLuaVm::FLuaVmImpl {
public:
    /**
     * 登録情報の保存先を指定して構築する。
     *
     * @param allocator 登録情報の保存領域を確保する。
     */
    explicit FLuaVmImpl(acs::FAllocator& allocator) noexcept : m_Natives(allocator) {}

private:
    /** 所有元のCLuaVmだけに内部状態の操作を許可する。 */
    friend class CLuaVm;

    /** Initで生成するLua状態。 */
    lua_State* m_L = nullptr;

    /** 登録したC++関数一件分の情報。 */
    struct FNativeReg {
        /** スクリプトから呼び出すC++関数。 */
        acs::game::NativeFunction fn = nullptr;

        /** C++関数へ渡す利用者データ。 */
        void* user = nullptr;

        /** 失敗した登録のclosureを後続登録から区別する番号。 */
        lua_Integer registration_id = 0;
    };

    /** 登録したC++関数の一覧。 */
    acs::TArray<FNativeReg> m_Natives;

    /** Lua関数へ埋め込む次の登録番号。公開開始後は失敗しても消費し、0は番号枯渇を示す。 */
    lua_Integer m_NextNativeRegistrationId = 1;

    /** 同じ実行環境へのC++関数登録再入を拒否する状態。 */
    bool m_NativeRegistrationInProgress = false;

    /** この実装を所有するLua実行環境。 */
    CLuaVm* m_Owner = nullptr;

    /** 直前に保持した戻り文字列の登録番号。 */
    int m_LastStringRef = LUA_NOREF;

    /** 直前に保持した戻り文字列を解放する。 */
    void ReleaseLastString() noexcept;

    /**
     * Luaから登録済みC++関数を呼び出す。
     *
     * @param state 呼び出し元のLua状態。
     * @return Luaへ返す値の個数。
     */
    static int NativeTrampoline(lua_State* state) noexcept;
};

} // namespace acs::scripting
