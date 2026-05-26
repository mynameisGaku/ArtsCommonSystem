// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — エントリポイント。
//
// 動作:
//   ・GameFramework Pillar A の FScene 上に FParticleEffectSystem (Pillar I Phase 2)
//     を 1 個立て、`fxedit::FParticleEditorPanel` がそのパラメータを ImGui で編集、
//     `fxedit::FParticleEditorPreview` がプレビュー描画 + Burst/Restart ボタン
//     を提供する。
//   ・main menu bar "File > Save .fxedit / Load .fxedit" で `FFxeditSerializer`
//     経由の永続化を行う (保存先は実行ディレクトリの "preset.fxedit")。
//   ・Esc で終了。
//
// 構成:
//   main.cpp                      - ACS_GAME_MAIN(ParticleEditorApp) のみ
//   ParticleEditorScene.{h,cpp}   - FParticle 編集対象 FScene (emitter + Panel + Preview)
//   ParticleEditorApp.{h,cpp}     - FGame 派生クラス (ImGui lifecycle ラッパ)
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui と同じ理由で、
// ImGuiContext が DX12 raw backend 経由のため)。
#include "ParticleEditorApp.h"

ACS_GAME_MAIN(helloparticleed::ParticleEditorApp)
