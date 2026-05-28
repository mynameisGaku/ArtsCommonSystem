// SPDX-License-Identifier: Apache-2.0
// HelloScripting — ScriptDemoApp 実装。
#include "ScriptDemoApp.h"

#include "foundation/Types.h"

#if WITH_ACS_SCRIPTING
#include "scripting/LuaVm.h"
#endif

#include <cstdio>

namespace helloscript {

using namespace acs;
using acs::game::IScriptVm;
using acs::game::ScriptValue;
using acs::game::ScriptCallFrame;
using acs::game::EScriptValueKind;

namespace {

// ScriptValue を組み立てる小ヘルパ (読みやすさのため)。
ScriptValue Number(f64 n) noexcept {
    ScriptValue v{};
    v.kind  = EScriptValueKind::Number;
    v.v.num = n;
    return v;
}

// f64 のほぼ等価判定。
bool NearlyEqual(f64 a, f64 b) noexcept {
    const f64 d = a - b;
    return (d < 0.0 ? -d : d) < 1e-9;
}

// スクリプトから呼ばれる C++ 関数: host_add(a, b) -> a + b。
void HostAdd(IScriptVm&, ScriptCallFrame& frame, void*) noexcept {
    f64 a = (frame.arg_count > 0 && frame.args[0].kind == EScriptValueKind::Number)
            ? frame.args[0].v.num : 0.0;
    f64 b = (frame.arg_count > 1 && frame.args[1].kind == EScriptValueKind::Number)
            ? frame.args[1].v.num : 0.0;
    if (frame.ret) {
        frame.ret->kind  = EScriptValueKind::Number;
        frame.ret->v.num = a + b;
    }
}

void ReportLoad(const char* demo, const TResult<void>& r) noexcept {
    if (r.IsErr()) {
        std::printf("    (LoadScript failed: subcode=%u)\n",
                    static_cast<unsigned>(r.Error().subcode));
    }
    (void)demo;
}

} // namespace

bool ScriptDemoApp::DemoCallFunction(IScriptVm& vm) noexcept {
    const char* src =
        "function add(a, b) return a + b end\n"
        "function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end\n";
    ReportLoad("CallFunction", vm.LoadScript(src, 0, "demo_callfn"));

    ScriptValue args[2] = { Number(2.0), Number(3.0) };
    ScriptValue ret{};
    const bool ok_add = vm.CallFunction("add", args, 2, &ret).IsOk()
                        && ret.kind == EScriptValueKind::Number;

    ScriptValue fib_arg[1] = { Number(10.0) };
    ScriptValue fib_ret{};
    const bool ok_fib = vm.CallFunction("fib", fib_arg, 1, &fib_ret).IsOk()
                        && fib_ret.kind == EScriptValueKind::Number;

    std::printf("  CallFunction : add(2,3)=%.0f  fib(10)=%.0f\n",
                ok_add ? ret.v.num : -1.0, ok_fib ? fib_ret.v.num : -1.0);

    if (!m_bRealBackend) return true;  // stub は実値を返さない
    return ok_add && NearlyEqual(ret.v.num, 5.0)
        && ok_fib && NearlyEqual(fib_ret.v.num, 55.0);
}

bool ScriptDemoApp::DemoNativeCallback(IScriptVm& vm) noexcept {
    const bool reg_ok = vm.RegisterNativeFunction("host_add", &HostAdd, nullptr).IsOk();

    // スクリプト側から host_add を呼び、結果をグローバル result に保存。
    ReportLoad("NativeCallback", vm.LoadScript("result = host_add(40, 2)", 0, "demo_native"));
    const f64 result = vm.GetGlobalNumber("result", -1.0);

    std::printf("  Native call  : register=%s  result=host_add(40,2)=%.0f\n",
                reg_ok ? "ok" : "fail", result);

    if (!m_bRealBackend) return true;
    return reg_ok && NearlyEqual(result, 42.0);
}

bool ScriptDemoApp::DemoGlobals(IScriptVm& vm) noexcept {
    vm.SetGlobalNumber("hp", 100.0);
    ReportLoad("Globals", vm.LoadScript("hp = hp - 30", 0, "demo_globals"));
    const f64 hp = vm.GetGlobalNumber("hp", -1.0);

    std::printf("  Globals      : set hp=100, script hp-=30 -> hp=%.0f\n", hp);

    if (!m_bRealBackend) return true;
    return NearlyEqual(hp, 70.0);
}

bool ScriptDemoApp::DemoGarbageCollection(IScriptVm& vm) noexcept {
    ReportLoad("GC", vm.LoadScript(
        "big = {} for i = 1, 20000 do big[i] = i * i end", 0, "demo_gc_alloc"));
    const u64 mem_before = vm.MemoryUsageBytes();

    ReportLoad("GC", vm.LoadScript("big = nil", 0, "demo_gc_free"));
    vm.CollectGarbage();
    const u64 mem_after = vm.MemoryUsageBytes();

    std::printf("  GC           : before=%llu B  after collect=%llu B\n",
                static_cast<unsigned long long>(mem_before),
                static_cast<unsigned long long>(mem_after));

    if (!m_bRealBackend) return true;
    // 確保した分が GC で戻る (= after < before)。
    return mem_after < mem_before;
}

int ScriptDemoApp::Run() noexcept {
#if WITH_ACS_SCRIPTING
    acs::scripting::FLuaVm lua;
    IScriptVm& vm = lua;
    m_bRealBackend = true;
#else
    IScriptVm& vm = acs::game::GetVmStub();
    m_bRealBackend = false;
#endif

    std::printf("=== ACS HelloScripting ===\n");
    std::printf("backend: %s\n\n", m_bRealBackend ? "REAL Lua 5.4 (FLuaVm)"
                                                  : "Stub (build with -DACS_BUILD_SCRIPTING=ON)");

    if (vm.Init().IsErr()) {
        std::printf("VM Init failed.\n");
        return m_bRealBackend ? 1 : 0;
    }

    int failures = 0;
    if (!DemoCallFunction(vm))       ++failures;
    if (!DemoNativeCallback(vm))     ++failures;
    if (!DemoGlobals(vm))            ++failures;
    if (!DemoGarbageCollection(vm))  ++failures;

    vm.Shutdown();

    std::printf("\nresult: %s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    // stub のときは「未実装」が正常なので 0 を返す。
    return (m_bRealBackend && failures > 0) ? 1 : 0;
}

} // namespace helloscript
