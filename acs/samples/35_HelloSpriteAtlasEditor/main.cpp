// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — エントリポイント。
//
// 構成:
//   SpriteAtlasApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
//   SpriteAtlasScene.{h,cpp} - Workspace + SpriteAtlasEditorPanel + dummy SpritePack
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace を 1 個立てて、Phase 22 で
//     並列実装中の `spriteatlas::SpriteAtlasEditorPanel` を register する。
//   ・シーン内に 256x256 の dummy atlas (= 実 texture は無し、panel 側 grid
//     placeholder で代用) を `SpritePack` に初期登録し、3 frame
//     (Idle / Walk / Jump 各 32x32) を AddFrame で先に入れておく。
//   ・MainMenuBar > File に Save / Load ".acsatlas" を stub callback で配線。
//     Phase 22 範囲外なので serializer 本体は未配線、ACS_LOG_INFO で発火を
//     ログするだけ。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// ACS_GAME_MAIN は SpriteAtlasApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "SpriteAtlasApp.h"

ACS_GAME_MAIN(hellosa::SpriteAtlasApp)
