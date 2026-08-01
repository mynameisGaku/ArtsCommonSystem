// SPDX-License-Identifier: Apache-2.0
// HelloScripting — Lua VM の使い方を 4 デモで示す。
//
// 各デモは IScriptVm 越しに動くので、real Lua (CLuaVm) でも stub でも
// 同じコードで走る。real のときだけ結果を厳密に検証する。
#pragma once

#include "gameframework/ScriptHost.h"

namespace helloscript {

class FScriptDemoApp {
public:
    // VM を Init → 4 デモ実行 → Shutdown。real backend で全デモ成功なら 0。
    int Run() noexcept;

private:
    // スクリプト内の関数を C++ から呼ぶ (add / 再帰 fib)。
    bool DemoCallFunction(acs::game::IScriptVm& vm) noexcept;

    // C++ 関数を登録し、スクリプト側から呼び返してもらう。
    bool DemoNativeCallback(acs::game::IScriptVm& vm) noexcept;

    // グローバル変数を C++ ↔ スクリプトで共有する。
    bool DemoGlobals(acs::game::IScriptVm& vm) noexcept;

    // テーブルを大量確保 → 破棄 → GC でメモリが戻ることを見る。
    bool DemoGarbageCollection(acs::game::IScriptVm& vm) noexcept;

    // real backend (Lua) を使えているか。stub のときは検証を緩める。
    bool m_bRealBackend = false;
};

} // namespace helloscript
