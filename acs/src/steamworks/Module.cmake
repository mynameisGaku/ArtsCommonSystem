# SPDX-License-Identifier: Apache-2.0
# Steamworks module — `ISteamworksBridge` の real 実装 (Phase 26 実バックエンド)。
#
# 有効化: cmake -DACS_BUILD_STEAMWORKS=ON -DACS_STEAMWORKS_SDK_DIR=<sdk path>
#
# OFF (default) の場合は本 module 自体がビルドされず、依然として
# `acs::game::SteamworksBridgeStub` (no-op) のみ利用可能。
#
# 開発時は steam_appid.txt = 480 (Spacewar) を実行ディレクトリに置く。
# samples/00_HelloEasy/assets/steam_appid.txt として同梱予定。

if(NOT ACS_BUILD_STEAMWORKS)
    return()
endif()

# 3rd party SDK target を生成 (acs_third_party::steamworks)
include(ACSThirdParty)
acs_third_party_steamworks()

acs_module(
    NAME    Steamworks
    TYPE    Runtime
    SOURCES
        SteamworksBridgeImpl.cpp
        SteamworksDefault.cpp
    HEADERS
        SteamworksBridgeImpl.h
    PUBLIC_DEPS
        Foundation
        GameFramework
    LINK_PUBLIC
        acs_third_party_steamworks
)
