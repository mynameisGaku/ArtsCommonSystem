// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorPanel 実装 (Phase 21a)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・本基底は「Workspace ポインタを保存する」最低限の OnInit と、
//     `_visible` / `_docked_target` のアクセサだけを実装する。
//   ・残りの hook (OnFrameBegin / OnSelectionChanged / OnAssetSelected /
//     OnSaveLayout / OnLoadLayout / OnShutdown / WantsFocus) は全て header 側で
//     inline no-op default を持つため、本 .cpp には実装が無い。
//   ・本ファイルは「将来の hook 実装が増えたとき / vtable を出すための翻訳単位
//     を持っておく」目的で配置する (= 純粋仮想なし基底でも vtable anchor 用に
//     1 つ非 inline 関数を持つ ACS 規約)。Title() / DrawUI() が純粋仮想のため
//     基底自体は instantiate されないが、`virtual ~FEditorPanel()` のために
//     vtable は出る。`OnInit` を非 inline で持つことで、各派生 .cpp 側に vtable
//     コピーが量産されるのを防ぐ (= 翻訳単位 anchor の役割)。

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::editor_core {

// ============================================================================
// OnInit — Workspace 参照を内部に保存
// ============================================================================
// 派生クラスが override する際は冒頭で `FEditorPanel::OnInit(workspace);` を
// 呼ぶこと (= ヘッダのコメント参照)。基底は他に副作用を持たないため、
// 「派生で呼び忘れた場合」は Workspace() が nullptr のまま動作する
// (= 派生側で workspace を使わないなら無害、使うなら呼び忘れ自体がバグ)。
//
// Workspace ポインタの解除は基底側では行わない (OnShutdown の default は no-op)。
// 派生クラスが「Shutdown 後に Workspace() が nullptr を返してほしい」場合は
// override で明示的に基底の `_workspace = nullptr;` 相当を呼ぶ必要があるが、
// _workspace は private のため将来 protected に変更するか、
// `FEditorPanel::OnShutdown()` を明示的に呼べる形にするかは Phase 21b で
// 利用パターンが揃ってから判断する (= YAGNI、現状は OnInit 1 回 + 終生有効
// の典型ケースのみ想定)。
// ============================================================================
void FEditorPanel::OnInit(FEditorWorkspace& workspace) noexcept {
    _workspace = &workspace;
}

} // namespace acs::game::editor_core
