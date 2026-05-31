// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// LuaDefault — FLuaVm を gameframework の既定 ScriptVm provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::Scripting に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetScriptVmProvider()` を呼んで行う。アプリは起動時
// に一度 `acs::scripting::InstallLuaAsDefault()` を呼ぶだけで、以降は backend 非
// 依存に `acs::game::GetDefaultScriptVm()` で実 Lua 5.4 VM を取得できる。
//
// provider が返す VM はプロセス共有 singleton。初回アクセス時に Init() を 1 回
// 走らせ、すぐ使える状態で返す (FScriptHost::Init(&vm) にそのまま差せる)。
// =============================================================================
#include "scripting/LuaVm.h"
#include "gameframework/ScriptHost.h"

namespace acs::scripting {

acs::game::IScriptVm& GetDefaultLuaVm() noexcept {
    // Meyers singleton。プロセス内 1 個の Lua VM を既定として共有する。
    static FLuaVm s_vm;
    // 初回のみ Init。ACS のこの層は単一スレッド初期化を前提とするため、素朴な
    // フラグで十分 (function-local static の構築自体は thread-safe)。
    static bool s_inited = false;
    if (!s_inited) {
        (void)s_vm.Init();   // 失敗しても VM 参照は返す (呼び出し側が IsAvailable 等で判断)
        s_inited = true;
    }
    return s_vm;
}

void InstallLuaAsDefault() noexcept {
    acs::game::SetScriptVmProvider(&GetDefaultLuaVm);
}

} // namespace acs::scripting
