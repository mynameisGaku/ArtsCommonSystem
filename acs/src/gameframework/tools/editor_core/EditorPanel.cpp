// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorPanel 実装
//
// 設計のポイント (詳細はヘッダ参照):
//   ・本基底は「Workspace ポインタを保存する」最低限の OnInit と、
//     `m_Visible` / `m_DockedTarget` のアクセサだけを実装する。
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

/**
 * Workspace 参照を内部に保存する基底初期化フック。
 *
 * @details
 * 派生クラスが override する際は冒頭で FEditorPanel::OnInit(workspace) を呼ぶこと。
 * 基底は他に副作用を持たず、Workspace ポインタの解除は行わない (OnShutdown の
 * default は no-op)。非 inline で持つことで翻訳単位 anchor (vtable 出力先) を兼ねる。
 * @param workspace 登録先の editor workspace (参照を non-owning で保持)。
 */
void FEditorPanel::OnInit(FEditorWorkspace& workspace) noexcept {
    m_Workspace = &workspace;
}

} // namespace acs::game::editor_core
