// SPDX-License-Identifier: Apache-2.0
// Lua VM を既定のスクリプト実行先へ接続する。
#include "scripting/LuaVm.h"
#include "gameframework/ScriptHost.h"
#include "memory/SystemAllocator.h"

namespace acs::scripting {

/** 共有するLua VMを初期化して返す。 */
acs::game::IScriptVm& GetDefaultLuaVm() noexcept {
    /** 共有VMとその確保元を同じ寿命で保持する。 */
    struct FDefaultLuaState {
        /** VMに確保元を渡して構築する。 */
        FDefaultLuaState() noexcept : vm(allocator) {}

        // VM を先に破棄してから allocator を破棄する宣言順にする。
        /** VMの保存領域を確保する。 */
        acs::CSystemAllocator allocator;
        /** 既定として共有するLua VM。 */
        CLuaVm vm;
    };

    // DefaultAllocator はアプリ終了時に失効し得るため、process lifetime の allocator
    // を singleton 自身に所有させる。Init はべき等なので、失敗後や明示 Shutdown 後も
    // 次の取得で再試行できる。
    static FDefaultLuaState s_state;
    (void)s_state.vm.Init();
    return s_state.vm;
}

/** Lua VMを既定のスクリプト実行先に設定する。 */
void InstallLuaAsDefault() noexcept {
    acs::game::SetScriptVmProvider(&GetDefaultLuaVm);
}

} // namespace acs::scripting
