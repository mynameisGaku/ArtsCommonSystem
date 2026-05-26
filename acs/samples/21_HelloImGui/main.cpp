// SPDX-License-Identifier: Apache-2.0
// HelloImGui — エントリポイント。
//
// 構成:
//   HelloImGuiApp.{h,cpp} - Application 派生クラス (ImGuiCtx + demo UI)
//
// 動作:
//   ・ウィンドウを開いて ImGui のデモウィンドウを表示
//   ・自前ウィンドウで FPS 表示 + スライダーで背景色変更
//   ・Esc で終了
//
// ACS_DEFINE_MAIN は HelloImGuiApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloImGuiApp.h"

ACS_DEFINE_MAIN(helloimgui::HelloImGuiApp)
