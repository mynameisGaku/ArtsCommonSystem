// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — AInspectorPanel
//
// `CInspectorSeam` 経由で公開された `IInspectableProvider` の `FInspectableObject`
// を ImGui ベースの field widget として描画 / 編集する UI パネル。シーンビュー
// (AHierarchyPanel) で選択された ANode に対して、紐づく provider
// の field 配列を表示する。
//
// 役割分担:
//   ・本パネルは **描画と編集 widget の構築** のみを担当。Provider 自体の登録 /
//     破棄 / 値の永続化は呼び出し側 (CInspectorSeam owner) の責務。値の変更
//     通知は `FieldChangeCallback` で外部 (undo / dirty tracker / 永続化) に
//     委譲する。
//   ・`CSelectionService` (別エージェントで実装中) に依存する。本パネルは
//     forward-decl のみで受け、ポインタ経由で「現在の選択 FNodeId」を取得する。
//     CSelectionService 未設定時は `DrawUI` の引数 `selected_id` を採用する。
//
// 設計選択:
//   ・**非コピー / 非ムーブ**: 内部 dirty / selected_id / callback 状態を持つ
//     ため、所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept**: ACS 規約。エラーは無効 FNodeId / nullptr provider を
//     no-op (描画スキップ) で扱う。
//   ・**STL 不使用**: 文字列バッファは `char[256]` などスタック領域のみ。
//     コンテナは持たない (Provider 自身が `FInspectableField[]` を所有)。
//   ・**ImGui ヘッダは .cpp に閉じ込め**: header からは imgui 依存を漏らさず、
//     gameframework 上位レイヤから include しても include order が壊れない
//     ようにする (AParticleEditorPanel と同じパターン)。
//   ・**FieldChangeCallback は raw 関数ポインタ + void***: ACS は STL の
//     std::function を使えないため、C スタイルの callback 規約に揃える
//     (AParticleEditorPanel / Input.h と同形)。
//   ・**`CInspectorSeam::GetProvider(FNodeId)` を仮定**: FNodeId → Provider の
//     resolve は seam 側 API を前提に呼び出す。実 seam の `GetProvider(u32)`
//     との overload 共存を想定。
//
// EFieldKind → ImGui widget マッピング (InspectorSeam.h 9 種):
//   Bool   → Checkbox
//   I32    → InputInt
//   U32    → InputScalar(U32)
//   F32    → SliderFloat (現状の FInspectableField には min/max metadata が無い
//             ため、defaults `kDefaultSliderMin` / `kDefaultSliderMax` を使う。
//             metadata 拡張が入ったらここで参照する)
//   Vec2   → DragFloat2
//   Vec3   → DragFloat3
//   Vec4   → DragFloat4 (仕様外だが対称性のため対応)
//   String → InputText (`char[256]` バッファ経由)
//   Enum   → Combo (enum_values 文字列配列を直接 ImGui に渡す)
//
// 範囲外 (将来):
//   ・field の hide / readonly 属性 (現状は全 field 編集可能扱い)
//   ・複合型 (struct of struct / array of T) の自動展開
//   ・undo / redo 統合
//   ・multi-select (現状は単一 FNodeId のみ)
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "gameframework/InspectorSeam.h"  // EFieldKind / FInspectableField / FInspectableObject
#include "gameframework/NodeId.h"

#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::inspector {

/**
 * 現在の選択を共有する selection サービス (別レイヤで実装、forward-decl で受ける)。
 *
 * @details 本パネルが使う最小 API は `FNodeId CurrentSelection() const noexcept` のみで、.cpp 側で実装ヘッダを include する。
 */
/**
 * 選択ノードの FInspectableObject を ImGui field widget として描画・編集するパネル。
 *
 * @details
 * `CInspectorSeam` 経由で公開された `IInspectableProvider` を解決し、その field 配列を
 * EFieldKind に応じた ImGui widget (Checkbox / InputInt / SliderFloat / DragFloatN /
 * ColorEdit / InputText / Combo 等) で描画する。描画と編集 widget 構築のみを担当し、
 * 値の永続化や undo は FieldChangeCallback 経由で外部へ委譲する。選択 FNodeId は注入
 * された CSelectionService から取得する。editor_core::AEditorPanel を継承し、全 API は
 * noexcept・STL 不使用・ImGui 依存は .cpp に閉じる。
 */
class AInspectorPanel : public ::acs::game::editor_core::AEditorPanel {
public:
    /**
     * field 変更通知の関数ポインタ型。
     *
     * @param user SetOnFieldChangeCallback の第二引数で渡したユーザポインタ (closure 代替)。
     * @param target 変更が起きたノードの FNodeId。
     * @param field_name 変更された field 名 (Provider 所有のリテラル文字列)。
     * @param kind 変更された field の EFieldKind。
     */
    using FieldChangeCallback = void (*)(void* user,
                                         FNodeId target,
                                         const char* field_name,
                                         EFieldKind kind) noexcept;

    /** 空状態で構築する (seam / selection なし)。 */
    AInspectorPanel() noexcept = default;

    /** 破棄する (外部所有の seam / CSelectionService / callback は解放しない)。 */
    ~AInspectorPanel() noexcept = default;

    /** コピー禁止 (内部 dirty / selection / callback 状態の所有を曖昧にしないため)。 */
    AInspectorPanel(const AInspectorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AInspectorPanel& operator=(const AInspectorPanel&) = delete;

    /** ムーブ禁止 (同上の所有規約)。 */
    AInspectorPanel(AInspectorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AInspectorPanel& operator=(AInspectorPanel&&)      = delete;

    /** 内部状態 (selection / dirty) を初期値に戻して初期化する (多重 Init 可)。 */
    void Init() noexcept;

    /** dirty / callback / selection を解除して後始末する (多重 Shutdown 可)。 */
    void Shutdown() noexcept;

    /**
     * provider lookup に使う CInspectorSeam を設定する。
     *
     * @param seam field provider を引く seam (nullptr で未設定)。
     */
    void SetInspectorSeam(CInspectorSeam* seam) noexcept { m_InspectorSeam = seam; }

    /**
     * 設定済みの CInspectorSeam を返す。
     *
     * @return seam ポインタ (未設定なら nullptr)。
     */
    CInspectorSeam* InspectorSeamPtr() const noexcept { return m_InspectorSeam; }

    /**
     * このパネルのウィンドウタイトルを返す。
     *
     * @return "Inspector"。
     */
    const char* Title() const noexcept override { return "Inspector"; }

    /**
     * メイン ImGui window を描画する。
     *
     * @details
     * `Begin("Inspector")` で 1 window を出す。選択 FNodeId は CSelectionService の
     * CurrentSelection() を採用し、無効なら "(Nothing selected)" を表示して return する。
     * Provider は `seam.GetProviderForNode(node_id)` で解決し、nullptr なら案内を表示する。
     * 各 Provider オブジェクトを type/instance ヘッダ + field 配列で描画し、編集が起きたら
     * dirty を立て Provider 通知 + FieldChangeCallback を発火する。
     */
    void DrawUI() noexcept override;

    /**
     * CSelectionService を登録する。
     *
     * @details 保持期間は呼び出し側責務 (non-owning)。
     * @param svc 選択を取得する selection サービス (nullptr で解除)。
     */
    void SetSelectionService(CSelectionService* svc) noexcept;

    /**
     * 直近の DrawUI で何らかの field が編集されたかを返す。
     *
     * @return 編集があれば true。
     */
    bool IsAnyFieldDirty() const noexcept { return m_Dirty; }

    /** dirty フラグをクリアする (外部の永続化 / undo 完了後に呼ぶ想定)。 */
    void ClearDirtyFlag() noexcept { m_Dirty = false; }

    /**
     * field 変更通知 callback を登録する。
     *
     * @param cb 呼ぶ callback (nullptr で解除)。
     * @param user callback 第一引数として戻すユーザポインタ。
     */
    void SetOnFieldChangeCallback(FieldChangeCallback cb, void* user) noexcept;

    /** F32 SliderFloat の既定 min (FInspectableField に min/max metadata が入るまでの暫定値)。 */
    static constexpr f32 kDefaultSliderMin = -1000.0f;

    /** F32 SliderFloat の既定 max (同上の暫定値)。 */
    static constexpr f32 kDefaultSliderMax =  1000.0f;

    /** FString 編集用スタックバッファ長 (ImGui::InputText の上限)。 */
    static constexpr u32 kStringBufferSize = 256u;

private:
    /** 現在の選択 FNodeId キャッシュ (DrawUI で更新、デバッグ表示用)。 */
    FNodeId               m_CurrentSelection {};

    /** 選択取得用の CSelectionService (non-owning、未注入なら nullptr)。 */
    CSelectionService*    m_SelectionService = nullptr;

    /** provider lookup に使う CInspectorSeam (SetInspectorSeam で設定)。 */
    CInspectorSeam*       m_InspectorSeam    = nullptr;

    /** 直近フレームで field 変更が起きたかのフラグ (IsAnyFieldDirty / ClearDirtyFlag で読み書き)。 */
    bool                 m_Dirty             = false;

    /** field 変更通知 callback (未登録なら nullptr)。 */
    FieldChangeCallback  m_OnChangeCb      = nullptr;

    /** callback に渡すユーザポインタ。 */
    void*                m_OnChangeUser    = nullptr;
};

using FInspectorPanel = AInspectorPanel;

} // namespace acs::game::inspector
