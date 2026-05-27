// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FInspectorPanel (Phase 20 editor 第二弾)
//
// `FInspectorSeam` 経由で公開された `IInspectableProvider` の `InspectableObject`
// を ImGui ベースの field widget として描画 / 編集する UI パネル。シーンビュー
// (Phase 20-1 SceneHierarchyPanel) で選択された Node に対して、紐づく provider
// の field 配列を表示する。
//
// 役割分担:
//   ・本パネルは **描画と編集 widget の構築** のみを担当。Provider 自体の登録 /
//     破棄 / 値の永続化は呼び出し側 (FInspectorSeam owner) の責務。値の変更
//     通知は `FieldChangeCallback` で外部 (undo / dirty tracker / 永続化) に
//     委譲する。
//   ・`FSelectionService` (別エージェントで実装中) に依存する。本パネルは
//     forward-decl のみで受け、ポインタ経由で「現在の選択 FNodeId」を取得する。
//     FSelectionService 未設定時は `DrawUI` の引数 `selected_id` を採用する。
//
// 設計選択:
//   ・**非コピー / 非ムーブ**: 内部 dirty / selected_id / callback 状態を持つ
//     ため、所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept**: ACS 規約。エラーは無効 FNodeId / nullptr provider を
//     no-op (描画スキップ) で扱う。
//   ・**STL 不使用**: 文字列バッファは `char[256]` などスタック領域のみ。
//     コンテナは持たない (Provider 自身が `InspectableField[]` を所有)。
//   ・**ImGui ヘッダは .cpp に閉じ込め**: header からは imgui 依存を漏らさず、
//     gameframework 上位レイヤから include しても include order が壊れない
//     ようにする (FParticleEditorPanel と同じパターン)。
//   ・**FieldChangeCallback は raw 関数ポインタ + void***: ACS は STL の
//     std::function を使えないため、C スタイルの callback 規約に揃える
//     (FParticleEditorPanel / Input.h と同形)。
//   ・**`FInspectorSeam::GetProvider(FNodeId)` を仮定**: FNodeId → Provider の
//     resolve は seam 側 API を前提に呼び出す。実 seam の `GetProvider(u32)`
//     との overload 共存を想定。
//
// EFieldKind → ImGui widget マッピング (FInspectorSeam.h 9 種):
//   Bool   → Checkbox
//   I32    → InputInt
//   U32    → InputScalar(U32)
//   F32    → SliderFloat (現状の InspectableField には min/max metadata が無い
//             ため、defaults `kDefaultSliderMin` / `kDefaultSliderMax` を使う。
//             metadata 拡張が入ったらここで参照する)
//   FVec2   → DragFloat2
//   FVec3   → DragFloat3
//   FVec4   → DragFloat4 (仕様外だが対称性のため対応)
//   FColor  → ColorEdit3 (data を FVec3* として扱う)
//   FString → InputText (`char[256]` バッファ経由)
//   Enum   → Combo (enum_values 文字列配列を直接 ImGui に渡す)
//
// 範囲外 (将来):
//   ・field の hide / readonly 属性 (現状は全 field 編集可能扱い)
//   ・複合型 (struct of struct / array of T) の自動展開
//   ・undo / redo 統合
//   ・multi-select (現状は単一 FNodeId のみ)
#pragma once

#include "foundation/Types.h"
#include "gameframework/InspectorSeam.h"  // EFieldKind / InspectableField / InspectableObject
#include "gameframework/NodeId.h"

namespace acs::game {
class FInspectorSeam;       // 前方宣言は本パネル内では不要 (header include 済み) だが
                           // 仕様一貫性のため宣言を残す
}

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

// 別エージェントが作成中の FSelectionService。本パネルは forward-decl で受け、
// .cpp 側で実装ヘッダ (FSelectionService.h) を include して扱う。
// 本パネルが使う最小 API は `FNodeId CurrentSelection() const noexcept` 1 つのみ。
class FSelectionService;

// ---------------------------------------------------------------------------
// FInspectorPanel — ImGui ベースの field property editor
// (Phase 24: editor_core::FEditorPanel 継承)
// ---------------------------------------------------------------------------
class FInspectorPanel : public ::acs::game::editor_core::FEditorPanel {
public:
    // field 変更通知 callback。`user` は SetOnFieldChangeCallback の第二引数で
    // 渡したポインタがそのまま戻る (closure 代替)。`field_name` は Provider が
    // 所有するリテラル文字列 (InspectableField::name)。
    using FieldChangeCallback = void (*)(void* user,
                                         FNodeId target,
                                         const char* field_name,
                                         EFieldKind kind) noexcept;

    FInspectorPanel() noexcept = default;
    ~FInspectorPanel() noexcept = default;

    // 非コピー・非ムーブ: 内部 dirty / selection / callback 状態の所有を曖昧にしない。
    FInspectorPanel(const FInspectorPanel&)            = delete;
    FInspectorPanel& operator=(const FInspectorPanel&) = delete;
    FInspectorPanel(FInspectorPanel&&)                 = delete;
    FInspectorPanel& operator=(FInspectorPanel&&)      = delete;

    // 初期化: 内部状態を初期値に戻す。多重 Init 可。
    void Init() noexcept;

    // 後片付け: dirty / callback / selection を解除。多重 Shutdown 可。
    void Shutdown() noexcept;

    // メイン ImGui window 描画。`Begin("Inspector")` で 1 つの window を出す。
    //
    // 選択 FNodeId の解決順:
    //   1. SetSelectionService() で FSelectionService が設定済みなら、その
    //      `Current()` を優先採用。
    //   2. それが無効 (= !IsValid()) なら、引数 `selected_id` を使う。
    //   3. どちらも無効なら "(Nothing selected)" を表示して return。
    //
    // Provider 解決は `seam.GetProvider(node_id)` を呼ぶ (FNodeId 受けの API を
    // 想定)。nullptr が返ったら "(No provider)" を表示。
    // Phase 24: FEditorPanel 継承で no-param DrawUI 化。FInspectorSeam は
    // SetInspectorSeam で事前 set、selection は FSelectionService 経由で取得。
    void SetInspectorSeam(class FInspectorSeam* seam) noexcept { m_InspectorSeam = seam; }
    class FInspectorSeam* InspectorSeamPtr() const noexcept { return m_InspectorSeam; }

    const char* Title() const noexcept override { return "Inspector"; }
    void DrawUI() noexcept override;

    // FSelectionService を登録。nullptr で解除。FSelectionService の保持期間は
    // 呼び出し側責務 (non-owning)。
    void SetSelectionService(FSelectionService* svc) noexcept;

    // 直近の DrawUI 内で何らかの field が編集されたか。
    bool IsAnyFieldDirty() const noexcept { return m_Dirty; }

    // dirty フラグをクリア。外部の永続化 / undo 処理完了後に呼ぶ想定。
    void ClearDirtyFlag() noexcept { m_Dirty = false; }

    // field 変更通知 callback を登録。nullptr で解除。
    void SetOnFieldChangeCallback(FieldChangeCallback cb, void* user) noexcept;

    // ---- 公開定数 -------------------------------------------------------
    // F32 SliderFloat の既定 min/max。InspectableField に min/max metadata が
    // 追加されるまでの暫定値。
    static constexpr f32 kDefaultSliderMin = -1000.0f;
    static constexpr f32 kDefaultSliderMax =  1000.0f;

    // FString 編集用スタックバッファ長 (= ImGui::InputText 制限)。
    static constexpr u32 kStringBufferSize = 256u;

private:
    // 現在の選択 FNodeId キャッシュ。FSelectionService から取得した値、または
    // DrawUI の引数で渡された値を反映する (DrawUI 末尾で更新)。デバッグ表示用。
    FNodeId               m_CurrentSelection {};

    // FSelectionService (non-owning)。nullptr ならば DrawUI 引数を採用。
    FSelectionService*    m_SelectionService = nullptr;

    // Phase 24: 事前 set される FInspectorSeam (DrawUI で provider lookup に使う)
    class FInspectorSeam* m_InspectorSeam    = nullptr;

    // 直近フレームで field 変更が起きたか。外部が IsAnyFieldDirty() で読み、
    // ClearDirtyFlag() でクリアする。
    bool                 m_Dirty             = false;

    // field 変更通知 callback。
    FieldChangeCallback  m_OnChangeCb      = nullptr;
    void*                m_OnChangeUser    = nullptr;
};

} // namespace acs::game::inspector
