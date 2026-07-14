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
#include "memory/SystemAllocator.h"

namespace acs::scripting {

acs::game::IScriptVm& GetDefaultLuaVm() noexcept {
    struct DefaultLuaState {
        DefaultLuaState() noexcept : vm(allocator)
        {
        }

        // VM を先に破棄してから allocator を破棄する宣言順にする。
        acs::FSystemAllocator allocator;
        FLuaVm vm;
    };

    // DefaultAllocator はアプリ終了時に失効し得るため、process lifetime の allocator
    // を singleton 自身に所有させる。Init はべき等なので、失敗後や明示 Shutdown 後も
    // 次の取得で再試行できる。
    static DefaultLuaState s_state;
    (void)s_state.vm.Init();
    return s_state.vm;
}

void InstallLuaAsDefault() noexcept {
    acs::game::SetScriptVmProvider(&GetDefaultLuaVm);
}

} // namespace acs::scripting
