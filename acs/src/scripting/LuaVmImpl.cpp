// SPDX-License-Identifier: Apache-2.0
#include "scripting/LuaVmImpl.h"

#include "scripting/LuaVmValueConversion.h"

namespace acs::scripting {

/** 直前に保持した戻り文字列をLuaの登録領域から外す。 */
void CLuaVm::FLuaVmImpl::ReleaseLastString() noexcept {
    if (m_L && m_LastStringRef != LUA_NOREF && m_LastStringRef != LUA_REFNIL) {
        luaL_unref(m_L, LUA_REGISTRYINDEX, m_LastStringRef);
    }
    m_LastStringRef = LUA_NOREF;
}

/**
 * Luaから登録済みC++関数を呼び出す。
 *
 * @param state 呼び出し元のLua状態。
 * @return Luaへ返す値の個数。
 */
int CLuaVm::FLuaVmImpl::NativeTrampoline(lua_State* state) noexcept {
    /** 呼び出す登録情報の位置。 */
    const int registration_index = static_cast<int>(lua_tointeger(state, lua_upvalueindex(1)));
    /** 呼び出し先を保持する実装。 */
    auto* implementation = static_cast<CLuaVm::FLuaVmImpl*>(lua_touserdata(state, lua_upvalueindex(2)));
    const lua_Integer registration_id = lua_tointeger(state, lua_upvalueindex(3)); // closureを作成した登録の識別番号。
    if (!implementation || registration_index < 0 || static_cast<usize>(registration_index) >= implementation->m_Natives.Size()) {
        return 0;
    }

    /** 呼び出す関数と利用者データ。 */
    const CLuaVm::FLuaVmImpl::FNativeReg& registration = implementation->m_Natives[static_cast<usize>(registration_index)];
    if (!registration.fn || registration.registration_id != registration_id) return 0;

    constexpr int kMaxArgs = 16; // 一度に受け取る引数の上限。
    acs::game::FScriptValue arguments[kMaxArgs]; // C++関数へ渡す引数。
    int argument_count = lua_gettop(state); // 実際に渡す引数の個数。
    if (argument_count > kMaxArgs) argument_count = kMaxArgs;
    for (int index = 0; index < argument_count; ++index) { // 変換中の引数位置。
        arguments[index] = lua_vm_detail::LuaToScriptValue(state, index + 1);
    }

    acs::game::FScriptValue return_value{}; // C++関数から受け取る戻り値。
    acs::game::FScriptCallFrame frame{}; // 引数と戻り値をまとめた呼び出し情報。
    frame.args = arguments;
    frame.arg_count = static_cast<u32>(argument_count);
    frame.ret = &return_value;
    registration.fn(*implementation->m_Owner, frame, registration.user);

    if (return_value.kind != acs::game::EScriptValueKind::Nil) {
        lua_vm_detail::PushScriptValue(state, return_value);
        return 1;
    }
    return 0;
}

} // namespace acs::scripting
