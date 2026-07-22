// SPDX-License-Identifier: Apache-2.0
// HelloSteamworks — エントリポイント。
//
// ISteamworksBridge (実績 / stat / leaderboard / cloud save) の使い方デモ。
// stub mode でビルド可能 (SDK 不要)、ACS_BUILD_STEAMWORKS=ON で real backend。
//
// 構成:
//   main.cpp                  - ACS_DEFINE_MAIN(SteamworksDemoApp) のみ
//   SteamworksDemoApp.{h,cpp} - Application 派生クラス本体
#include "SteamworksDemoApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellosteam::FSteamworksDemoApp)
