// SPDX-License-Identifier: Apache-2.0
// HelloImGui — エントリポイント。
//
// 構成:
//   HelloImGuiApp.{h,cpp} - FApplication 派生クラス (FImGuiCtx + demo UI)
//
// 操作:
//   ・ウィンドウ内で ImGui demo / FPS / 背景色スライダーを操作
//   ・Esc で終了
//
// ACS_DEFINE_MAIN は HelloImGuiApp を main エントリに登録
// (FApplication 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloImGuiApp.h"

ACS_DEFINE_MAIN(helloimgui::HelloImGuiApp)
