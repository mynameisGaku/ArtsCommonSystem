// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — エントリポイント。
//
// 動作:
//   ・GameFramework Pillar A の Scene 上に CParticleEffectSystem (Pillar I Phase 2)
//     を 1 個立て、`fxedit::AParticleEditorPanel` がそのパラメータを ImGui で編集、
//     `fxedit::CParticleEditorPreview` がプレビュー描画 + Burst/Restart ボタン
//     を提供する。
//   ・main menu bar "File > Save .fxedit / Load .fxedit" で `CFxeditSerializer`
//     経由の永続化を行う (保存先は実行ディレクトリの "preset.fxedit")。
//   ・Esc で終了。
//
// 構成:
//   main.cpp                      - ACS_GAME_MAIN(ParticleEditorApp) のみ
//   ParticleEditorScene.{h,cpp}   - Particle 編集対象 Scene (emitter + Panel + Preview)
//   ParticleEditorApp.{h,cpp}     - CGame 派生クラス (ImGui lifecycle ラッパ)
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui と同じ理由で、
// ImGuiContext が DX12 raw backend 経由のため)。
#include "ParticleEditorApp.h"

ACS_GAME_MAIN(helloparticleed::CParticleEditorApp)
