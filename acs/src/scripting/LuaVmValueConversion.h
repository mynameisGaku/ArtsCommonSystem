// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/ScriptHost.h"

extern "C" {
#include "lua.h"
}

namespace acs::scripting::lua_vm_detail {

/**
 * Luaスタックの値をACSのスクリプト値へ変換する。
 *
 * @param state 読み取るLua状態。
 * @param index 読み取るスタック位置。
 * @return 変換した値。
 */
inline acs::game::FScriptValue LuaToScriptValue(lua_State* state, int index) noexcept {
    acs::game::FScriptValue value{}; // 変換結果を保持する。
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            value.kind = acs::game::EScriptValueKind::Bool;
            value.v.b = lua_toboolean(state, index) != 0;
            break;
        case LUA_TNUMBER:
            value.kind = acs::game::EScriptValueKind::Number;
            value.v.num = static_cast<acs::f64>(lua_tonumber(state, index));
            break;
        case LUA_TSTRING:
            value.kind = acs::game::EScriptValueKind::String;
            value.v.str = lua_tostring(state, index);
            break;
        default:
            value.kind = acs::game::EScriptValueKind::Nil;
            break;
    }
    return value;
}

/**
 * ACSのスクリプト値をLuaスタックへ積む。
 *
 * @param state 書き込むLua状態。
 * @param value 書き込む値。
 */
inline void PushScriptValue(lua_State* state, const acs::game::FScriptValue& value) noexcept {
    switch (value.kind) {
        case acs::game::EScriptValueKind::Bool:
            lua_pushboolean(state, value.v.b ? 1 : 0);
            break;
        case acs::game::EScriptValueKind::Number:
            lua_pushnumber(state, static_cast<lua_Number>(value.v.num));
            break;
        case acs::game::EScriptValueKind::String:
            lua_pushstring(state, value.v.str ? value.v.str : "");
            break;
        case acs::game::EScriptValueKind::Handle:
            lua_pushinteger(state, static_cast<lua_Integer>(value.v.handle));
            break;
        case acs::game::EScriptValueKind::Nil:
        default:
            lua_pushnil(state);
            break;
    }
}

} // namespace acs::scripting::lua_vm_detail
