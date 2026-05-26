// SPDX-License-Identifier: Apache-2.0
// HelloUI — エントリポイント。
//
// 構成:
//   PlayerVM.h           - Player ViewModel (hp/mana/invincible/name + hp_label)
//   HelloUIApp.{h,cpp}   - Application 派生クラス + Widget tree 構築
//
// 操作:
//   左クリック : ボタン / スライダー / チェックボックスを操作
//   Esc        : 終了
//
// ACS_DEFINE_MAIN は HelloUIApp を main エントリに登録
// (Application 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
//
// ACS 純正 UI フレームワーク (src/ui/) を使ってステータスパネルを構築し、
// PlayerVM の Observable を Slider/Checkbox/TextInput に MakeTwoWayBind で接続する。
// ImGui 不要、SpriteBatch + Font + ACS Widget のみで動作。
#include "HelloUIApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloui::HelloUIApp)
