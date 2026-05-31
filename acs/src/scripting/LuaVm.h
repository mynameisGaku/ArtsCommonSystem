// SPDX-License-Identifier: Apache-2.0
// Lua 5.4 backend (acs::game::IScriptVm の real 実装、Phase N-3)。
//
// Lua は MIT / FetchContent 取得可能 / gating 無しなので、stub と違い
// 実ビルド + 実動作まで完全検証できる real backend。
//
// 利用例:
//   acs::scripting::FLuaVm vm;
//   if (vm.Init().IsOk()) {
//       vm.LoadScript("function add(a,b) return a+b end", 0, "inline");
//       acs::game::ScriptValue args[2];
//       args[0].kind = acs::game::EScriptValueKind::Number; args[0].v.num = 2;
//       args[1].kind = acs::game::EScriptValueKind::Number; args[1].v.num = 3;
//       acs::game::ScriptValue ret;
//       vm.CallFunction("add", args, 2, &ret);  // ret.v.num == 5
//       vm.Shutdown();
//   }
//
// 設計:
//   ・Pimpl で lua.h を public header から隠す (lua_State* を漏らさない)。
//   ・NativeFunction の bridge は lua_pushcclosure の upvalue に
//     (registry index, this) を載せる trampoline 方式。
//   ・ScriptValue <-> Lua stack 変換は Nil/Bool/Number/FString/Handle の 5 種。
//   ・全 noexcept、STL/<string> 不使用、TResult でエラー伝搬。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/ScriptHost.h"

namespace acs::scripting {

class FLuaVm final : public acs::game::IScriptVm {
public:
    FLuaVm() noexcept;
    ~FLuaVm() noexcept override;

    FLuaVm(const FLuaVm&)            = delete;
    FLuaVm& operator=(const FLuaVm&) = delete;
    FLuaVm(FLuaVm&&)                 = delete;
    FLuaVm& operator=(FLuaVm&&)      = delete;

    acs::TResult<void>       Init()                                              noexcept override;
    void                     Shutdown()                                          noexcept override;
    acs::game::EScriptLanguage Language()                                  const noexcept override {
        return acs::game::EScriptLanguage::Lua54;
    }
    acs::TResult<void>       LoadScript(const char* source, acs::u32 source_len,
                                        const char* chunk_name)                  noexcept override;
    acs::TResult<void>       CallFunction(const char* function_name,
                                          const acs::game::ScriptValue* args,
                                          acs::u32 arg_count,
                                          acs::game::ScriptValue* ret_out)       noexcept override;
    acs::TResult<void>       RegisterNativeFunction(const char* function_name,
                                                    acs::game::NativeFunction fn,
                                                    void* user)                  noexcept override;
    void                     SetGlobalNumber(const char* name, acs::f64 value)   noexcept override;
    acs::f64                 GetGlobalNumber(const char* name,
                                             acs::f64 default_value)       const noexcept override;
    void                     CollectGarbage()                                    noexcept override;
    acs::u64                 MemoryUsageBytes()                            const noexcept override;

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

// =============================================================================
// 既定 ScriptVm 結線ヘルパ (gameframework の provider へ FLuaVm を登録)
// -----------------------------------------------------------------------------
// アプリ起動時に一度だけ `InstallLuaAsDefault()` を呼ぶと、以降
// `acs::game::GetDefaultScriptVm()` が本物の Lua 5.4 VM (プロセス共有 singleton)
// を返すようになる。これにより上位コードは backend 非依存に
// `GetDefaultScriptVm()` だけで実 VM を取得できる (`#if WITH_ACS_SCRIPTING` は
// この Install 呼び出し 1 箇所だけで済む)。
// =============================================================================

// プロセス共有の既定 FLuaVm singleton を返す (provider の実体)。
acs::game::IScriptVm& GetDefaultLuaVm() noexcept;

// gameframework の ScriptVm provider に GetDefaultLuaVm を登録する。
void InstallLuaAsDefault() noexcept;

} // namespace acs::scripting
