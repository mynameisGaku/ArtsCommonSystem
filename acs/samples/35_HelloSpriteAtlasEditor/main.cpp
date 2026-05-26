// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — エントリポイント。
//
// 動作:
//   ・editor_core::FEditorWorkspace を 1 個立てて、spriteatlas::FSpriteAtlasEditorPanel
//     を register する。
//   ・256x256 の dummy atlas (実 texture は無し、panel 側 grid placeholder で代用)
//     を FSpritePack に初期登録し、3 frame (Idle / Walk / Jump 各 32x32) を AddFrame で
//     入れておく。
//   ・MainMenuBar > File に Save / Load ".acsatlas" を stub callback で配線。
//     serializer 本体は未配線で、ACS_LOG_INFO で発火をログするだけ。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (FImGuiCtx が DX12 raw backend 経由のため)。
//
// ACS_GAME_MAIN は SpriteAtlasApp を main エントリに登録 (FApplication 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "SpriteAtlasApp.h"

ACS_GAME_MAIN(hellosa::SpriteAtlasApp)
