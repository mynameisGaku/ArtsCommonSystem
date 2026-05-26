// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — エントリポイント。
//
// 構成:
//   GameTypes.h        - HighScore POD + 共通定数 (constexpr / inline)
//   FullGameApp.{h,cpp}- FGame 派生クラス (FAudioDirector / FFont / FSaveSlot 等)
//   TitleScene.{h,cpp} - Title 画面 (背景色 Tween + Press Space 待ち)
//   GameplayScene.{h,cpp} - 本編 (WASD / Mouse / Fire / wave / HUD / FPS)
//   GameOverScene.{h,cpp} - 結果画面 (VICTORY / GAME OVER + best 表示)
//
// 操作:
//   WASD       : 移動
//   マウス     : 照準
//   左クリック : 発射
//   Esc        : 終了
//   R          : Title 戻し (GameOver)
//
// ACS_GAME_MAIN は FullGameApp を main エントリに登録 (FApplication 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "FullGameApp.h"

ACS_GAME_MAIN(hellofg::FullGameApp)
