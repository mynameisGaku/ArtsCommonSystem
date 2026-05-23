// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — InspectorPanel (Phase 20 editor 第二弾)
//
// `InspectorSeam` 経由で公開された `IInspectableProvider` の `InspectableObject`
// を ImGui ベースの field widget として描画 / 編集する UI パネル。シーンビュー
// (Phase 20-1 SceneHierarchyPanel) で選択された Node に対して、紐づく provider
// の field 配列を表示する。
//
// 役割分担:
//   ・本パネルは **描画と編集 widget の構築** のみを担当。Provider 自体の登録 /
//     破棄 / 値の永続化は呼び出し側 (InspectorSeam owner) の責務。値の変更
//     通知は `FieldChangeCallback` で外部 (undo / dirty tracker / 永続化) に
//     委譲する。
//   ・`SelectionService` (別エージェントで実装中) に依存する。本パネルは
//     forward-decl のみで受け、ポインタ経由で「現在の選択 NodeId」を取得する。
//     SelectionService 未設定時は `DrawUI` の引数 `selected_id` を採用する。
//
// 設計選択:
//   ・**非コピー / 非ムーブ**: 内部 dirty / selected_id / callback 状態を持つ
//     ため、所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept**: ACS 規約。エラーは無効 NodeId / nullptr provider を
//     no-op (描画スキップ) で扱う。
//   ・**STL 不使用**: 文字列バッファは `char[256]` などスタック領域のみ。
//     コンテナは持たない (Provider 自身が `InspectableField[]` を所有)。
//   ・**ImGui ヘッダは .cpp に閉じ込め**: header からは imgui 依存を漏らさず、
//     gameframework 上位レイヤから include しても include order が壊れない
//     ようにする (ParticleEditorPanel と同じパターン)。
//   ・**FieldChangeCallback は raw 関数ポインタ + void***: ACS は STL の
//     std::function を使えないため、C スタイルの callback 規約に揃える
//     (ParticleEditorPanel / Input.h と同形)。
//   ・**`InspectorSeam::GetProvider(NodeId)` を仮定**: NodeId → Provider の
//     resolve は seam 側 API を前提に呼び出す。実 seam の `GetProvider(u32)`
//     との overload 共存を想定。
//
// EFieldKind → ImGui widget マッピング (InspectorSeam.h 9 種):
//   Bool   → Checkbox
//   I32    → InputInt
//   U32    → InputScalar(U32)
//   F32    → SliderFloat (現状の InspectableField には min/max metadata が無い
//             ため、defaults `kDefaultSliderMin` / `kDefaultSliderMax` を使う。
//             metadata 拡張が入ったらここで参照する)
//   Vec2   → DragFloat2
//   Vec3   → DragFloat3
//   Vec4   → DragFloat4 (仕様外だが対称性のため対応)
//   Color  → ColorEdit3 (data を Vec3* として扱う)
//   String → InputText (`char[256]` バッファ経由)
//   Enum   → Combo (enum_values 文字列配列を直接 ImGui に渡す)
//
// 範囲外 (将来):
//   ・field の hide / readonly 属性 (現状は全 field 編集可能扱い)
//   ・複合型 (struct of struct / array of T) の自動展開
//   ・undo / redo 統合
//   ・multi-select (現状は単一 NodeId のみ)
#pragma once

#include "foundation/Types.h"
#include "gameframework/InspectorSeam.h"  // EFieldKind / InspectableField / InspectableObject
#include "gameframework/NodeId.h"

namespace acs::game {
class InspectorSeam;       // 前方宣言は本パネル内では不要 (header include 済み) だが
                           // 仕様一貫性のため宣言を残す
}

namespace acs::game::inspector {

// 別エージェントが作成中の SelectionService。本パネルは forward-decl で受け、
// .cpp 側で実装ヘッダ (SelectionService.h) を include して扱う。
// 本パネルが使う最小 API は `NodeId CurrentSelection() const noexcept` 1 つのみ。
class SelectionService;

// ---------------------------------------------------------------------------
// InspectorPanel — ImGui ベースの field property editor
// ---------------------------------------------------------------------------
class InspectorPanel {
public:
    // field 変更通知 callback。`user` は SetOnFieldChangeCallback の第二引数で
    // 渡したポインタがそのまま戻る (closure 代替)。`field_name` は Provider が
    // 所有するリテラル文字列 (InspectableField::name)。
    using FieldChangeCallback = void (*)(void* user,
                                         NodeId target,
                                         const char* field_name,
                                         EFieldKind kind) noexcept;

    InspectorPanel() noexcept = default;
    ~InspectorPanel() noexcept = default;

    // 非コピー・非ムーブ: 内部 dirty / selection / callback 状態の所有を曖昧にしない。
    InspectorPanel(const InspectorPanel&)            = delete;
    InspectorPanel& operator=(const InspectorPanel&) = delete;
    InspectorPanel(InspectorPanel&&)                 = delete;
    InspectorPanel& operator=(InspectorPanel&&)      = delete;

    // 初期化: 内部状態を初期値に戻す。多重 Init 可。
    void Init() noexcept;

    // 後片付け: dirty / callback / selection を解除。多重 Shutdown 可。
    void Shutdown() noexcept;

    // メイン ImGui window 描画。`Begin("Inspector")` で 1 つの window を出す。
    //
    // 選択 NodeId の解決順:
    //   1. SetSelectionService() で SelectionService が設定済みなら、その
    //      `Current()` を優先採用。
    //   2. それが無効 (= !IsValid()) なら、引数 `selected_id` を使う。
    //   3. どちらも無効なら "(Nothing selected)" を表示して return。
    //
    // Provider 解決は `seam.GetProvider(node_id)` を呼ぶ (NodeId 受けの API を
    // 想定)。nullptr が返ったら "(No provider)" を表示。
    void DrawUI(class InspectorSeam& seam, NodeId selected_id) noexcept;

    // SelectionService を登録。nullptr で解除。SelectionService の保持期間は
    // 呼び出し側責務 (non-owning)。
    void SetSelectionService(SelectionService* svc) noexcept;

    // 直近の DrawUI 内で何らかの field が編集されたか。
    bool IsAnyFieldDirty() const noexcept { return _dirty; }

    // dirty フラグをクリア。外部の永続化 / undo 処理完了後に呼ぶ想定。
    void ClearDirtyFlag() noexcept { _dirty = false; }

    // field 変更通知 callback を登録。nullptr で解除。
    void SetOnFieldChangeCallback(FieldChangeCallback cb, void* user) noexcept;

    // ---- 公開定数 -------------------------------------------------------
    // F32 SliderFloat の既定 min/max。InspectableField に min/max metadata が
    // 追加されるまでの暫定値。
    static constexpr f32 kDefaultSliderMin = -1000.0f;
    static constexpr f32 kDefaultSliderMax =  1000.0f;

    // String 編集用スタックバッファ長 (= ImGui::InputText 制限)。
    static constexpr u32 kStringBufferSize = 256u;

private:
    // 現在の選択 NodeId キャッシュ。SelectionService から取得した値、または
    // DrawUI の引数で渡された値を反映する (DrawUI 末尾で更新)。デバッグ表示用。
    NodeId               _current_selection {};

    // SelectionService (non-owning)。nullptr ならば DrawUI 引数を採用。
    SelectionService*    _selection_service = nullptr;

    // 直近フレームで field 変更が起きたか。外部が IsAnyFieldDirty() で読み、
    // ClearDirtyFlag() でクリアする。
    bool                 _dirty             = false;

    // field 変更通知 callback。
    FieldChangeCallback  _on_change_cb      = nullptr;
    void*                _on_change_user    = nullptr;
};

} // namespace acs::game::inspector
