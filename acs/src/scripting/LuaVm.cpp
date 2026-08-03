// SPDX-License-Identifier: Apache-2.0
// Lua 5.4 backend 実装 (acs::game::IScriptVm)。
#include "scripting/LuaVmImpl.h"
#include "scripting/LuaVmValueConversion.h"

#include "foundation/Error.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs::scripting {

using acs::game::FScriptValue;
using acs::game::EScriptValueKind;
using acs::game::NativeFunction;
using lua_vm_detail::LuaToScriptValue;
using lua_vm_detail::PushScriptValue;
namespace script_err = acs::game::script_err;

namespace {

/** Luaグローバル公開に必要な関数名、登録位置、中継先をまとめる。 */
struct FNativeRegistrationOperation {
    /** closureから参照するLua実装本体。 */
    void* implementation = nullptr;

    /** Luaグローバルへ公開する関数名。 */
    const char* function_name = nullptr;

    /** closureから呼び出す中継関数。 */
    lua_CFunction trampoline = nullptr;

    /** native登録簿内の関数位置。 */
    int registration_index = 0;

    /** 失敗した登録のclosureを後続登録から区別する番号。 */
    lua_Integer registration_id = 0;

    /** 公開失敗時に戻す同名グローバルのLua登録番号。 */
    int previous_global_ref = LUA_NOREF;
};

/** 同一VMのnative登録中フラグを寿命で設定し、破棄時に解除する。 */
class FNativeRegistrationGuard {
public:
    /**
     * 再入拒否状態を開始する。
     *
     * @param registration_in_progress 登録中を示す状態。
     */
    explicit FNativeRegistrationGuard(bool& registration_in_progress) noexcept : m_RegistrationInProgress(registration_in_progress) {
        m_RegistrationInProgress = true;
    }

    /** 再入拒否状態を解除する。 */
    ~FNativeRegistrationGuard() noexcept {
        m_RegistrationInProgress = false;
    }

    /** コピーによる二重解除を禁止する。 */
    FNativeRegistrationGuard(const FNativeRegistrationGuard&) = delete;

    /** コピー代入による二重解除を禁止する。 */
    FNativeRegistrationGuard& operator=(const FNativeRegistrationGuard&) = delete;

private:
    /** 登録中を示す状態。 */
    bool& m_RegistrationInProgress;
};

/** Lua stackの開始位置を保存し、破棄時にその位置へ戻す。 */
class FLuaStackTopGuard {
public:
    /**
     * 現在のstack位置を保存する。
     *
     * @param state 復元対象のLua状態。
     */
    explicit FLuaStackTopGuard(lua_State* state) noexcept : m_State(state), m_InitialTop(lua_gettop(state)) {}

    /** 保存したstack位置へ戻す。 */
    ~FLuaStackTopGuard() noexcept {
        lua_settop(m_State, m_InitialTop);
    }

    /** コピーによる二重復元を禁止する。 */
    FLuaStackTopGuard(const FLuaStackTopGuard&) = delete;

    /** コピー代入による二重復元を禁止する。 */
    FLuaStackTopGuard& operator=(const FLuaStackTopGuard&) = delete;

    /** 保存したstack位置を返す。 */
    int InitialTop() const noexcept {
        return m_InitialTop;
    }

private:
    /** 復元対象のLua状態。 */
    lua_State* m_State = nullptr;

    /** 登録開始時のstack位置。 */
    int m_InitialTop = 0;
};

/**
 * native関数のclosureを作り、Luaグローバルへ公開する。
 *
 * @param state 保護呼び出し中のLua状態。第1引数にFNativeRegistrationOperationを受け取る。
 * @return Luaへ返す値の個数。
 */
int PublishNativeRegistration(lua_State* state) noexcept {
    /** 公開するnative関数の入力。 */
    auto* const operation = static_cast<FNativeRegistrationOperation*>(lua_touserdata(state, 1));
    if (operation == nullptr || operation->implementation == nullptr || operation->function_name == nullptr || operation->trampoline == nullptr) {
        return luaL_error(state, "invalid native registration operation");
    }

    lua_pushglobaltable(state);
    lua_pushstring(state, operation->function_name);
    lua_rawget(state, -2);
    operation->previous_global_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1);

    lua_pushinteger(state, operation->registration_index);
    lua_pushlightuserdata(state, operation->implementation);
    lua_pushinteger(state, operation->registration_id);
    lua_pushcclosure(state, operation->trampoline, 3);
    lua_pushvalue(state, -1);
    lua_setglobal(state, operation->function_name);
    lua_pushglobaltable(state);
    lua_pushstring(state, operation->function_name);
    lua_rawget(state, -2);
    /** 大域表へ実際に保存された値と生成直後のLua関数が同じかを示す。 */
    const bool is_published = lua_rawequal(state, -1, -3) != 0;
    lua_pop(state, 3);
    if (!is_published) {
        return luaL_error(state, "native registration global was not published");
    }
    return 0;
}

/**
 * Lua公開失敗前の同名グローバルを直接書き戻す。
 *
 * @param state 保護呼び出し中のLua状態。第1引数にFNativeRegistrationOperationを受け取る。
 * @return Luaへ返す値の個数。
 */
int RestoreNativeRegistrationGlobal(lua_State* state) noexcept {
    /** 復元するnative関数の入力。 */
    auto* const operation = static_cast<FNativeRegistrationOperation*>(lua_touserdata(state, 1));
    if (operation == nullptr || operation->function_name == nullptr || operation->previous_global_ref == LUA_NOREF) {
        return luaL_error(state, "invalid native registration rollback operation");
    }

    lua_pushglobaltable(state);
    lua_pushstring(state, operation->function_name);
    if (operation->previous_global_ref == LUA_REFNIL) {
        lua_pushnil(state);
    } else {
        lua_rawgeti(state, LUA_REGISTRYINDEX, operation->previous_global_ref);
    }
    lua_rawset(state, -3);
    lua_pop(state, 1);
    return 0;
}

} // namespace

/** Pimpl を確保し、所有者ポインタを自身に設定する。 */
CLuaVm::CLuaVm() noexcept : CLuaVm(DefaultAllocator()) {}

/** 指定 allocator で Pimpl 内の native function 登録簿を構築する。 */
/**
 * @param allocator 登録情報の保存領域を確保する。
 */
CLuaVm::CLuaVm(acs::IAllocator& allocator) noexcept {
    m_Impl = new FLuaVmImpl(allocator);
    m_Impl->m_Owner = this;
}

#if defined(ACS_SCRIPTING_TEST_HOOKS)
/** 次回のnative関数登録へ最大の識別番号を割り当てる。 */
void CLuaVm::SetNextNativeRegistrationIdToMaximumForTest() noexcept {
    m_Impl->m_NextNativeRegistrationId = lua_vm_detail::kNativeRegistrationIdMaximum;
}
#endif

/** Shutdown で lua_State を解放してから Pimpl を delete する。 */
CLuaVm::~CLuaVm() noexcept {
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

/** lua_State を生成し標準ライブラリを open する。 */
acs::TResult<void> CLuaVm::Init() noexcept {
    if (m_Impl->m_L) return acs::Ok();  // 既に初期化済み
    m_Impl->m_L = luaL_newstate();
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "luaL_newstate failed (out of memory)");
    }
    luaL_openlibs(m_Impl->m_L);  // base / string / table / math / os / io 等
    ACS_LOG_INFO("Lua 5.4 VM initialized (%s)", LUA_RELEASE);
    return acs::Ok();
}

/** lua_State を lua_close で破棄し、native registry と戻り文字列 anchor をリセットする。 */
void CLuaVm::Shutdown() noexcept {
    if (m_Impl == nullptr) return;
    if (m_Impl->m_L) {
        lua_close(m_Impl->m_L);              // registry ごと破棄されるので ref は無効化
        m_Impl->m_L = nullptr;
    }
    // process singleton でも shutdown 時点で容量を返し、再 Init は同じ allocator を使う。
    m_Impl->m_Natives.Empty();
    // 旧 state の registry ref を持ち越すと再 Init 後に別 state へ
    // luaL_unref してしまうため、未保持状態にリセットする。
    m_Impl->m_LastStringRef = LUA_NOREF;
    m_Impl->m_NativeRegistrationInProgress = false;
    // 番号枯渇後は再初期化しても古い番号空間を再利用しない。
    if (m_Impl->m_NextNativeRegistrationId != 0) {
        m_Impl->m_NextNativeRegistrationId = 1;
    }
}

/** ソースを luaL_loadbuffer で parse し lua_pcall で即時実行する。 */
acs::TResult<void> CLuaVm::LoadScript(const char* source, acs::u32 source_len, const char* chunk_name) noexcept {
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "Init() before LoadScript");
    }
    if (!source) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "source is null");
    }
    const usize len = (source_len > 0) ? static_cast<usize>(source_len) : std::strlen(source);
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

/** グローバル関数を lua_pcall し、戻り文字列は registry に anchor して延命する。 */
acs::TResult<void> CLuaVm::CallFunction(const char* function_name, const FScriptValue* args, acs::u32 arg_count, FScriptValue* ret_out) noexcept {
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
        // FScriptValue::FString は非所有 (ScriptHost.h) なので呼び出し側でコピー
        // 保持する義務はない契約。よって VM 側で registry に anchor して値を生かし、
        // pop 後も const char* を有効に保つ (次の CallFunction / Shutdown まで)。
        if (ret_out->kind == EScriptValueKind::String) {
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

/** 登録番号と内部データを付けたLua関数を公開する。 */
acs::TResult<void> CLuaVm::RegisterNativeFunction(const char* function_name, NativeFunction fn, void* user) noexcept {
    if (!m_Impl->m_L) {
        return ACS_ERR(Generic, script_err::kSub_NotInitialized, "Init() before RegisterNativeFunction");
    }
    if (!function_name || function_name[0] == 0 || !fn) {
        return ACS_ERR(Generic, script_err::kSub_InvalidArg, "name/fn null");
    }
    if (m_Impl->m_NativeRegistrationInProgress) {
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "CLuaVm::RegisterNativeFunction: reentrant registration rejected");
    }
    if (m_Impl->m_NextNativeRegistrationId == 0) {
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "CLuaVm::RegisterNativeFunction: registration identifiers exhausted");
    }
    /** 登録先のLua状態。 */
    lua_State* L = m_Impl->m_L;
    /** 注入allocatorとLuaメタ関数からの再入を登録終了まで拒否する。 */
    FNativeRegistrationGuard registration_guard(m_Impl->m_NativeRegistrationInProgress);
    /** 成否にかかわらず呼び出し前のstack位置へ戻す。 */
    FLuaStackTopGuard stack_guard(L);
    if (lua_checkstack(L, 2) == 0) {
        return ACS_ERR(Memory, script_err::kSub_AllocationFailed, "CLuaVm::RegisterNativeFunction: Lua stack allocation failed");
    }
    /** 新しい登録情報の番号。 */
    const int reg_index = static_cast<int>(m_Impl->m_Natives.Num());
    /** 登録する関数と利用者情報。 */
    FLuaVmImpl::FNativeReg reg;
    reg.fn   = fn;
    reg.user = user;
    reg.registration_id = m_Impl->m_NextNativeRegistrationId;
    /** Lua公開処理へ渡す登録情報。 */
    FNativeRegistrationOperation operation{m_Impl, function_name, &FLuaVmImpl::NativeTrampoline, reg_index, reg.registration_id, LUA_NOREF};
    // 登録簿を変える前に、確保を伴わないC関数と軽量利用者ポインタを保護呼び出し用の2枠へ積む。
    lua_pushcfunction(L, &PublishNativeRegistration);
    lua_pushlightuserdata(L, &operation);
    if (!m_Impl->m_Natives.TryAdd(reg)) {
        lua_pop(L, 2);
        return ACS_ERR(Memory, script_err::kSub_AllocationFailed, "CLuaVm::RegisterNativeFunction: registry allocation failed");
    }
    // Lua側へ関数が退避される可能性が生じるため、公開結果にかかわらず識別番号を消費する。
    m_Impl->m_NextNativeRegistrationId = lua_vm_detail::AdvanceNativeRegistrationId(reg.registration_id);

    /** Lua公開処理の終了状態。 */
    const int publish_status = lua_pcall(L, 1, 0, 0);
    if (publish_status != LUA_OK) {
        lua_settop(L, stack_guard.InitialTop());
        m_Impl->m_Natives.Pop();
        /** 同名グローバルを書き戻した結果。 */
        int rollback_status = LUA_OK;
        if (operation.previous_global_ref != LUA_NOREF) {
            lua_pushcfunction(L, &RestoreNativeRegistrationGlobal);
            lua_pushlightuserdata(L, &operation);
            rollback_status = lua_pcall(L, 1, 0, 0);
            if (rollback_status != LUA_OK) {
                lua_settop(L, stack_guard.InitialTop());
            }
        }
        if (operation.previous_global_ref != LUA_NOREF && operation.previous_global_ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, operation.previous_global_ref);
        }
        if (publish_status == LUA_ERRMEM || rollback_status == LUA_ERRMEM) {
            return ACS_ERR(Memory, script_err::kSub_AllocationFailed, "CLuaVm::RegisterNativeFunction: Lua closure allocation failed");
        }
        return ACS_ERR(Generic, script_err::kSub_CallFailed, "CLuaVm::RegisterNativeFunction: Lua global publication failed");
    }
    if (operation.previous_global_ref != LUA_NOREF && operation.previous_global_ref != LUA_REFNIL) {
        luaL_unref(L, LUA_REGISTRYINDEX, operation.previous_global_ref);
    }
    return acs::Ok();
}

/** 数値を push して name のグローバルに setglobal する (未初期化 / name null なら no-op)。 */
void CLuaVm::SetGlobalNumber(const char* name, acs::f64 value) noexcept {
    if (!m_Impl->m_L || !name) return;
    lua_pushnumber(m_Impl->m_L, static_cast<lua_Number>(value));
    lua_setglobal(m_Impl->m_L, name);
}

/** name のグローバルを getglobal し、数値なら返し、それ以外は default_value を返す。 */
acs::f64 CLuaVm::GetGlobalNumber(const char* name, acs::f64 default_value) const noexcept {
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

/** lua_gc(LUA_GCCOLLECT) で強制 GC を 1 サイクル走らせる (未初期化なら no-op)。 */
void CLuaVm::CollectGarbage() noexcept {
    if (m_Impl->m_L) {
        lua_gc(m_Impl->m_L, LUA_GCCOLLECT, 0);
    }
}

/** lua_gc の GCCOUNT (KiB) と GCCOUNTB (端数 byte) を合算してメモリ使用量を返す。 */
acs::u64 CLuaVm::MemoryUsageBytes() const noexcept {
    if (!m_Impl->m_L) return 0;
    const int kb = lua_gc(m_Impl->m_L, LUA_GCCOUNT, 0);
    const int b  = lua_gc(m_Impl->m_L, LUA_GCCOUNTB, 0);
    return static_cast<u64>(kb) * 1024ULL + static_cast<u64>(b);
}

} // namespace acs::scripting
