// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — エントリポイント。
//
// 構成:
//   FontEditorApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
//   FontEditorScene.{h,cpp} - Workspace + FFontEditorPanel + 初期 3 face 登録
//
// 動作:
//   ・editor_core の FEditorWorkspace を 1 個立てて、fontedit::FFontEditorPanel を
//     register する。
//   ・起動時に 3 face を fallback chain に初期登録:
//       [00] "Noto Sans JP"   : 日本語/英数共通プライマリ (path は stub)
//       [01] "Noto Sans Mono" : 等幅 ASCII / 記号 (path は stub)
//       [02] "fallback emoji" : Emoji / Symbol 拡張 (path は stub)
//   ・preview text には "ACS Font Editor サンプル 123 αβγ ★★★" を入れる。
//   ・MainMenuBar > File に Save / Load ".acsfont" を stub callback で配線
//     (serializer 本体は未配線、ACS_LOG_INFO で発火をログするだけ)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (ImGuiCtx が DX12 raw backend 経由のため)。
//
// ACS_GAME_MAIN は FontEditorApp を main エントリに登録 (FApplication 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "FontEditorApp.h"

ACS_GAME_MAIN(hellofont::FFontEditorApp)
