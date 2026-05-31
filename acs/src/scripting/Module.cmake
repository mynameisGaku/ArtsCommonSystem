# SPDX-License-Identifier: Apache-2.0
# Scripting module — IScriptVm の real 実装 (Lua 5.4 backend、Phase N-3)。
#
# Lua は MIT で gating 無し → FetchContent で取得し常時 real ビルド可能。
# Steamworks (partner gating) と違い、CI でも完全に動作検証できる。
#
# 名前空間 acs::scripting、CMake target ACS::Scripting。
# 利用側は acs::game::IScriptVm 経由で使い、acs::scripting::FLuaVm を DI する。

# discover はすべての Module.cmake を include するので、OFF のときは
# Lua FetchContent を走らせないよう早期 return する。
if(NOT ACS_BUILD_SCRIPTING)
    return()
endif()

include(ACSThirdParty)
acs_third_party_lua()

acs_module(
    NAME    Scripting
    TYPE    Runtime
    SOURCES
        LuaVm.cpp
        LuaDefault.cpp
    HEADERS
        LuaVm.h
    PUBLIC_DEPS
        Foundation
        Container
        GameFramework
    LINK_PUBLIC
        acs_third_party_lua
)
