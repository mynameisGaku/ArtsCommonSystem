// SPDX-License-Identifier: Apache-2.0
// Lua 5.4 backend 実装 (acs::game::IScriptVm)。
#include "scripting/LuaVm.h"

#include "foundation/Log.h"
#include "foundation/Error.h"
#include "container/Array.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>

namespace acs::scripting {

using acs::game::ScriptValue;
using acs::game::ScriptCallFrame;
using acs::game::EScriptValueKind;
using acs::game::NativeFunction;
namespace script_err = acs::game::script_err;

// ============================================================================
// Impl: lua_State* + native function registry
// ============================================================================
struct FLuaVm::Impl {
    lua_State* m_L = nullptr;

    // 登録済み native function。lua closure の upvalue に index を載せ、
    // trampoline がここから fn/user を引く。
    struct NativeReg {
        NativeFunction fn   = nullptr;
        void*          user = nullptr;
    };
    acs::TArray<NativeReg> m_Natives;
    FLuaVm*               m_Owner = nullptr;  // trampoline が IScriptVm& を渡すため

    // CallFunction が返す FString は Lua 所有メモリを指す。スタックから pop
    // すると GC 対象になり dangling になるため、直前に registry へ anchor して
    // VM 寿命の間 (= 次の CallFunction まで) 値を生かす。LUA_NOREF=未保持。
    int m_LastStringRef = LUA_NOREF;  // lauxlib.h。-2 = 未保持。

    // 直前に anchor した戻り文字列を registry から外す (GC 許可)。
    void ReleaseLastString() noexcept;

    // lua_CFunction (int(*)(lua_State*))。static メンバなので Impl の private
    // メンバにアクセスできる。匿名 namespace の free 関数だと Impl が
    // FLuaVm の private nested 型なので触れない。
    static int NativeTrampoline(lua_State* L) noexcept;
};

// 直前に anchor した戻り文字列を解放。CallFunction の度に呼び、前回の
// 戻り文字列を解放してから今回の値を anchor し直す (1 個だけ生かす方式)。
void FLuaVm::Impl::ReleaseLastString() noexcept {
    if (m_L && m_LastStringRef != LUA_NOREF && m_LastStringRef != LUA_REFNIL) {
        luaL_unref(m_L, LUA_REGISTRYINDEX, m_LastStringRef);
    }
    m_LastStringRef = LUA_NOREF;
}

namespace {

// Lua stack の 1 値 (index idx) を ScriptValue に変換。
ScriptValue LuaToScriptValue(lua_State* L, int idx) noexcept {
    ScriptValue sv{};
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            sv.kind = EScriptValueKind::Bool;
            sv.v.b  = lua_toboolean(L, idx) != 0;
            break;
        case LUA_TNUMBER:
            sv.kind  = EScriptValueKind::Number;
            sv.v.num = static_cast<f64>(lua_tonumber(L, idx));
            break;
        case LUA_TSTRING:
            sv.kind = EScriptValueKind::FString;
            sv.v.str = lua_tostring(L, idx);  // Lua 所有、call 中のみ有効
            break;
        default:
            sv.kind = EScriptValueKind::Nil;
            break;
    }
    return sv;
}

// ScriptValue を Lua stack に push。
void PushScriptValue(lua_State* L, const ScriptValue& sv) noexcept {
    switch (sv.kind) {
        case EScriptValueKind::Bool:    lua_pushboolean(L, sv.v.b ? 1 : 0);          break;
        case EScriptValueKind::Number:  lua_pushnumber(L, static_cast<lua_Number>(sv.v.num)); break;
        case EScriptValueKind::FString: lua_pushstring(L, sv.v.str ? sv.v.str : ""); break;
        case EScriptValueKind::Handle:  lua_pushinteger(L, static_cast<lua_Integer>(sv.v.handle)); break;
        case EScriptValueKind::Nil:
        default:                        lua_pushnil(L);                              break;
    }
}

} // namespace

// native function trampoline。upvalue(1)=registry index、upvalue(2)=FLuaVm::Impl*。
int FLuaVm::Impl::NativeTrampoline(lua_State* L) noexcept {
    const int reg_index = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    auto* impl = static_cast<FLuaVm::Impl*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!impl || reg_index < 0 ||
        static_cast<usize>(reg_index) >= impl->m_Natives.Size()) {
        return 0;
    }
    const FLuaVm::Impl::NativeReg& reg = impl->m_Natives[static_cast<usize>(reg_index)];
    if (!reg.fn) return 0;

    // Lua stack 上の引数を ScriptValue 配列に変換 (固定上限 16)。
    constexpr int kMaxArgs = 16;
    ScriptValue args[kMaxArgs];
    int n = lua_gettop(L);
    if (n > kMaxArgs) n = kMaxArgs;
    for (int i = 0; i < n; ++i) {
        args[i] = LuaToScriptValue(L, i + 1);
    }

    ScriptValue ret{};
    ScriptCallFrame frame{};
    frame.args      = args;
    frame.arg_count = static_cast<u32>(n);
    frame.ret       = &ret;

    reg.fn(*impl->m_Owner, frame, reg.user);

    // ret が Nil 以外なら 1 値返す。
    if (ret.kind != EScriptValueKind::Nil) {
        PushScriptValue(L, ret);
        return 1;
    }
    return 0;
}

// ============================================================================
// 公開 API
// ============================================================================

FLuaVm::FLuaVm() noexcept {
    m_Impl = new Impl();
    m_Impl->m_Owner = this;
}

FLuaVm::~FLuaVm() noexcept {
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

acs::TResult<void> FLuaVm::Init() noexcept {
    if (m_Impl->m_L) return acs::Ok();  // 既に初期化済み
    m_Impl->m_L = luaL_newstate();
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized,
                       "luaL_newstate failed (out of memory)");
    }
    luaL_openlibs(m_Impl->m_L);  // base / string / table / math / os / io 等
    ACS_LOG_INFO("Lua 5.4 VM initialized (%s)", LUA_RELEASE);
    return acs::Ok();
}

void FLuaVm::Shutdown() noexcept {
    if (m_Impl && m_Impl->m_L) {
        lua_close(m_Impl->m_L);              // registry ごと破棄されるので ref は無効化
        m_Impl->m_L = nullptr;
        m_Impl->m_Natives.Clear();
        // 旧 state の registry ref を持ち越すと再 Init 後に別 state へ
        // luaL_unref してしまうため、未保持状態にリセットする。
        m_Impl->m_LastStringRef = LUA_NOREF;
    }
}

acs::TResult<void> FLuaVm::LoadScript(const char* source, acs::u32 source_len,
                                      const char* chunk_name) noexcept {
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "Init() before LoadScript");
    }
    if (!source) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "source is null");
    }
    const usize len = (source_len > 0) ? static_cast<usize>(source_len)
                                       : std::strlen(source);
    const char* name = chunk_name ? chunk_name : "=(load)";
    // luaL_loadbuffer (parse) + lua_pcall (run)。
    if (luaL_loadbuffer(m_Impl->m_L, source, len, name) != LUA_OK) {
        const char* err = lua_tostring(m_Impl->m_L, -1);
        ACS_LOG_ERROR("Lua load error: %s", err ? err : "?");
        lua_pop(m_Impl->m_L, 1);
        return ACS_ERR(Generic, script_err::kSub_LoadFailed, "luaL_loadbuffer failed");
    }
    if (lua_pcall(m_Impl->m_L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(m_Impl->m_L, -1);
        ACS_LOG_ERROR("Lua run error: %s", err ? err : "?");
        lua_pop(m_Impl->m_L, 1);
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "lua_pcall (chunk) failed");
    }
    return acs::Ok();
}

acs::TResult<void> FLuaVm::CallFunction(const char* function_name,
                                        const ScriptValue* args, acs::u32 arg_count,
                                        ScriptValue* ret_out) noexcept {
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "Init() before CallFunction");
    }
    if (!function_name || function_name[0] == 0) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "function_name null/empty");
    }
    // arg_count>0 で args が null だと PushScriptValue ループで null deref する。
    // 引数なしは (args==null, arg_count==0) が契約 (ScriptHost.h) なので防御する。
    if (arg_count > 0 && !args) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "arg_count>0 but args is null");
    }
    lua_State* L = m_Impl->m_L;

    // 前回の CallFunction が anchor した戻り文字列を解放してから今回に入る。
    // (戻り FString は VM 寿命管理 = 次の CallFunction まで有効、の契約)
    m_Impl->ReleaseLastString();

    lua_getglobal(L, function_name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "global is not a function");
    }
    for (u32 i = 0; i < arg_count; ++i) {
        PushScriptValue(L, args[i]);
    }
    const int nresults = ret_out ? 1 : 0;
    if (lua_pcall(L, static_cast<int>(arg_count), nresults, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        ACS_LOG_ERROR("Lua call '%s' error: %s", function_name, err ? err : "?");
        lua_pop(L, 1);
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "lua_pcall (function) failed");
    }
    if (ret_out) {
        *ret_out = LuaToScriptValue(L, -1);
        // 戻り値が文字列のとき、ret_out->v.str は Lua スタック上の文字列を指す。
        // 直後の lua_pop でスタックが縮むとその文字列は GC 対象 = dangling になる。
        // ScriptValue::FString は非所有 (ScriptHost.h) なので呼び出し側でコピー
        // 保持する義務はない契約。よって VM 側で registry に anchor して値を生かし、
        // pop 後も const char* を有効に保つ (次の CallFunction / Shutdown まで)。
        if (ret_out->kind == EScriptValueKind::FString) {
            // 値を複製して push し直し、複製を registry に anchor (元はそのまま pop)。
            // luaL_ref は top を pop しつつ ref を返すので、複製を積んで ref 化する。
            lua_pushvalue(L, -1);                                  // 戻り文字列を複製 push
            m_Impl->m_LastStringRef = luaL_ref(L, LUA_REGISTRYINDEX);  // 複製を anchor (pop 済)
            // anchor した本体から改めて安定した const char* を取り直す。
            lua_rawgeti(L, LUA_REGISTRYINDEX, m_Impl->m_LastStringRef);
            ret_out->v.str = lua_tostring(L, -1);                 // registry 内文字列を指す
            lua_pop(L, 1);                                        // rawgeti で積んだ複製を pop
        }
        lua_pop(L, 1);  // 元の戻り値を pop
    }
    return acs::Ok();
}

acs::TResult<void> FLuaVm::RegisterNativeFunction(const char* function_name,
                                                  NativeFunction fn, void* user) noexcept {
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "Init() before RegisterNativeFunction");
    }
    if (!function_name || function_name[0] == 0 || !fn) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "name/fn null");
    }
    lua_State* L = m_Impl->m_L;
    // registry に追加し、その index を closure upvalue に。
    const int reg_index = static_cast<int>(m_Impl->m_Natives.Size());
    Impl::NativeReg reg;
    reg.fn   = fn;
    reg.user = user;
    m_Impl->m_Natives.PushBack(reg);

    lua_pushinteger(L, reg_index);                 // upvalue 1: registry index
    lua_pushlightuserdata(L, m_Impl);              // upvalue 2: Impl*
    lua_pushcclosure(L, &Impl::NativeTrampoline, 2);
    lua_setglobal(L, function_name);
    return acs::Ok();
}

void FLuaVm::SetGlobalNumber(const char* name, acs::f64 value) noexcept {
    if (!m_Impl->m_L || !name) return;
    lua_pushnumber(m_Impl->m_L, static_cast<lua_Number>(value));
    lua_setglobal(m_Impl->m_L, name);
}

acs::f64 FLuaVm::GetGlobalNumber(const char* name, acs::f64 default_value) const noexcept {
    if (!m_Impl->m_L || !name) return default_value;
    lua_State* L = m_Impl->m_L;
    lua_getglobal(L, name);
    f64 result = default_value;
    if (lua_isnumber(L, -1)) {
        result = static_cast<f64>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return result;
}

void FLuaVm::CollectGarbage() noexcept {
    if (m_Impl->m_L) {
        lua_gc(m_Impl->m_L, LUA_GCCOLLECT, 0);
    }
}

acs::u64 FLuaVm::MemoryUsageBytes() const noexcept {
    if (!m_Impl->m_L) return 0;
    const int kb = lua_gc(m_Impl->m_L, LUA_GCCOUNT, 0);
    const int b  = lua_gc(m_Impl->m_L, LUA_GCCOUNTB, 0);
    return static_cast<u64>(kb) * 1024ULL + static_cast<u64>(b);
}

} // namespace acs::scripting
