// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / AModelInspectorPanel
//
// ModelViewer ワークスペース内に配置される **読み取り専用の mesh 情報 panel**。
// CAssetBrowser から選択 / DragDrop された model (.mdl / .fbx / .gltf 等) を
// AModelViewerPanel (別エージェント) が load した結果を、本 panel に
// `UpdateFromModel(...)` でプッシュしてもらい、内部にスナップショットとして
// 保持して ImGui で表示する。Unity の Mesh Inspector / Godot の "Import"
// 出力タブ / UE の Static Mesh Editor の "Mesh Details" 相当。
//
// 表示内容:
//   1) **Summary**     : vertex / triangle / submesh / material slot / bone /
//                         animation clip 数 + bounding sphere (radius / center)
//   2) **Submeshes**   : CollapsingHeader + Table (name / start / count /
//                         material slot)
//   3) **Bones**       : TreeNode で parent_index から階層を再構築して再帰描画
//                         (root = parent_index < 0)
//   4) **Animations**  : Table (name / duration / sample count / looping)
//
// 役割分担:
//   ・本パネルは「描画 + 配列のスナップショット保持」だけを担当。実 model load /
//     parse は AModelViewerPanel (別エージェント) と Mesh / Skeleton / AnimationClip
//     モジュールの責任。caller (= AModelViewerPanel もしくは sample 31) が
//     load 完了時に本 panel の `UpdateFromModel` を呼んで情報を流し込む。
//   ・本 panel は callback / 編集 / GPU リソース確保を一切持たない (= 純粋な
//     read-only viewer)。
//
// 設計選択 (modelview):
//   ・**AEditorPanel 継承**: editor_core 基底に乗せる。
//     AModelViewerPanel / sample 31 が CEditorWorkspace 経由で本 panel を
//     register する。Title は "Model Info" (Unity / Godot の Inspector 表記寄り)。
//   ・**スナップショット方式**: 元の Mesh / Skeleton / AnimationClip オブジェクト
//     を ref で保持しない (= モデル再 load / 解放と本 panel の表示が race しない
//     ように、name / index 等を `acs::TArray<...>` に値コピーで持つ)。`const char*`
//     name は caller 側が「panel の寿命 ≧ 文字列の寿命」を保証する責務だが、
//     典型用途では Mesh モジュールの permanent storage を直接参照する想定なので
//     安全 (= ImGui draw までに開放されない)。
//   ・**3 つの可変長配列 + 1 つの fixed struct**: submeshes / bones / clips は
//     model ごとに件数が変わるため `acs::TArray<T>`。summary は単一 struct で
//     値保持。CAssetBrowser / AHierarchyPanel と同形の "TArray<T> を内部に持つ
//     panel" パターン。
//   ・**has_model flag**: load 前 (= UpdateFromModel が一度も呼ばれていない) /
//     Clear 直後の状態を識別するためのフラグ。DrawUI 冒頭で "(No model loaded)"
//     ガイダンスを出す目的。Summary の vertex_count==0 だけでは「空のメッシュ
//     を load した状態」と区別がつかないため、明示フラグを持つ。
//   ・**非コピー / 非ムーブ**: 内部 TArray<T> の所有を曖昧にしない (ACS 規約 +
//     他 panel 群と同形)。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。name は
//     `const char*` リテラル / caller 所有領域を想定。
//   ・**ImGui ヘッダは含めない**: 派生 .cpp で <imgui.h> を include する形
//     (AParticleEditorPanel / AInspectorPanel / AHierarchyPanel と同形)。
//   ・**bone hierarchy は描画時にオンザフライ走査**: 子リストを事前構築せず、
//     各 node 描画時に全 bone を線形走査して `parent_index == this_index` の
//     子を見つけて再帰する。bone 数 100k 級でなければ十分速く、メモリ追加なし。
//   ・**Submesh / AnimationClip は ImGui::BeginTable**: name / index / count を
//     等幅で見せたい用途に Table API が最適。CollapsingHeader でセクション
//     開閉、Table で行ごとの値表示、という 2 段構成。
//   ・**bounding_radius / bounding_center は FVec3 + f32**: Mesh モジュールの
//     既存 bounds API (= AABB or FSphere) のうち、最も典型的な「中心 + 半径」を
//     直接渡せる形にする。
//
// 範囲外:
//   ・vertex buffer の hex dump 表示
//   ・bone influence weight の matrix 表示
//   ・animation clip の preview
//   ・material slot の thumbnail / shader 名表示 (= MaterialEditor 連携)
//   ・LOD 階層情報
//   ・mesh / submesh の visibility toggle (= 編集機能、別 panel で)
//   ・export / re-import ボタン
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "math/Vec.h"

namespace acs::game::modelview {

/**
 * model 全体の集計情報 (単一インスタンス、値コピー保持)。
 *
 * @details
 * caller (AModelViewerPanel 等) が model load 後に集計して本 panel に渡す。全フィールド
 * 値型のため、Mesh / Skeleton / AnimationClip 側のリソース解放と本 panel の表示は完全に
 * decouple される。
 */
struct FMeshSummary {
    /** 全 vertex 数 (submesh 跨ぎ合算後)。 */
    u32        vertex_count          = 0;

    /** 全 triangle 数 (index_count / 3 合算)。 */
    u32        triangle_count        = 0;

    /** submesh セクション数。 */
    u32        submesh_count         = 0;

    /** material slot 数 (異なる material の数)。 */
    u32        material_slot_count   = 0;

    /** skeleton 内 bone 数 (skeleton 無しなら 0)。 */
    u32        bone_count            = 0;

    /** 関連付けられた animation clip 数 (0 可)。 */
    u32        animation_clip_count  = 0;

    /** model 全体の bounding sphere 半径。 */
    f32        bounding_radius       = 0.0f;

    /** 同 bounding sphere の中心 (object 空間)。 */
    acs::FVec3  bounding_center       = {};
};

/**
 * 1 submesh セクションの情報 (name + index 範囲 + material slot)。
 *
 * @details name は caller 所有のリテラル / permanent string を想定する (panel 寿命 ≦ name 寿命)。
 */
struct FSubmeshInfo {
    /** submesh 名 (例: "Body", "Hair", "Eyes")。 */
    const char* name                 = nullptr;

    /** 全 index buffer 内の開始 index。 */
    u32         index_start          = 0;

    /** この submesh の index 数 (3 の倍数想定)。 */
    u32         index_count          = 0;

    /** この submesh が使う material slot。 */
    u32         material_slot_index  = 0;
};

/**
 * 1 bone の情報 (name + 親 index)。
 *
 * @details
 * parent_index < 0 は root bone を意味する。本 panel は parent_index から子を線形走査で
 * 見つけて TreeNode 階層を再構築する (子リストは保持しない)。
 */
struct FBoneInfo {
    /** bone 名 (例: "Hips", "Spine_01")。 */
    const char* name         = nullptr;

    /** 同 bones 配列内の親 index (root は -1)。 */
    i32         parent_index = -1;
};

/** 1 animation clip の情報。 */
struct FAnimationClipInfo {
    /** clip 名 (例: "Idle", "Run", "Attack01")。 */
    const char* name         = nullptr;

    /** クリップの長さ (秒)。 */
    f32         duration_sec = 0.0f;

    /** サンプル数 (keyframe 数の合計など)。 */
    u32         sample_count = 0;

    /** ループ再生フラグ (clip metadata)。 */
    bool        is_looping   = false;
};

/**
 * read-only な mesh / skeleton / animation 情報ビューア panel。
 *
 * @details
 * editor_core::AEditorPanel を継承し、caller (AModelViewerPanel 等) が UpdateFromModel で
 * push した model 情報をスナップショットとして値コピー保持し、ImGui で Summary / Submeshes /
 * Bones / Animation Clips の 4 セクションを描画する。callback / 編集 / GPU リソース確保は
 * 一切持たない純粋な viewer。元の Mesh / Skeleton / AnimationClip オブジェクトは ref 保持
 * しないため、モデル再 load / 解放と本 panel の表示は race しない。
 */
class AModelInspectorPanel : public editor_core::AEditorPanel {
public:
    /** 空状態で構築する (model なし)。 */
    AModelInspectorPanel() noexcept = default;

    /** 破棄する (内部 TArray は ~TArray が解放)。 */
    ~AModelInspectorPanel() noexcept override = default;

    /** コピー禁止 (内部 TArray + has_model 状態の所有を曖昧にしないため)。 */
    AModelInspectorPanel(const AModelInspectorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AModelInspectorPanel& operator=(const AModelInspectorPanel&) = delete;

    /** ムーブ禁止。 */
    AModelInspectorPanel(AModelInspectorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AModelInspectorPanel& operator=(AModelInspectorPanel&&)      = delete;

    /** 内部状態を空にする (多重 Init 可)。 */
    void Init() noexcept;

    /** 全配列 + summary をクリアし has_model = false にする (多重 Shutdown 可)。 */
    void Shutdown() noexcept;

    /**
     * 現在表示する model のスナップショットを更新する。
     *
     * @details
     * summary は単一 struct を値コピー、submeshes / bones / clips は配列要素を 1 つずつ
     * PushBack で値コピーする (部分更新ではなく全置換)。各ポインタは nullptr 可で、その
     * 場合 count を 0 として扱う。呼び出し後 has_model = true になり DrawUI が情報を表示する。
     * @param summary model 全体の集計情報。
     * @param submeshes submesh 情報配列 (nullptr 可)。
     * @param submesh_count submeshes の要素数。
     * @param bones bone 情報配列 (nullptr 可)。
     * @param bone_count bones の要素数。
     * @param clips animation clip 情報配列 (nullptr 可)。
     * @param clip_count clips の要素数。
     */
    void UpdateFromModel(const FMeshSummary&        summary,
                         const FSubmeshInfo*        submeshes,
                         u32                       submesh_count,
                         const FBoneInfo*           bones,
                         u32                       bone_count,
                         const FAnimationClipInfo*  clips,
                         u32                       clip_count) noexcept;

    /**
     * 表示内容を空に戻す ("No model loaded" 表示に切り替わる)。
     *
     * @details model unload や workspace clear から呼ばれる想定 (多重 Clear 可)。
     */
    void Clear() noexcept;

    /**
     * model 全体の集計情報を返す。
     *
     * @return 保持中の FMeshSummary への const 参照。
     */
    const FMeshSummary& Summary() const noexcept { return m_Summary; }

    /**
     * 保持中の submesh 数を返す。
     *
     * @return submesh 数。
     */
    u32 SubmeshCount() const noexcept { return static_cast<u32>(m_Submeshes.Size()); }

    /**
     * i 番目の submesh 情報を返す。
     *
     * @param i submesh index。
     * @return i 番目の FSubmeshInfo (i >= SubmeshCount() なら nullptr)。
     */
    const FSubmeshInfo* Submesh(u32 i) const noexcept {
        return (i < m_Submeshes.Size()) ? &m_Submeshes[i] : nullptr;
    }

    /**
     * 保持中の bone 数を返す。
     *
     * @return bone 数。
     */
    u32 BoneCount() const noexcept { return static_cast<u32>(m_Bones.Size()); }

    /**
     * i 番目の bone 情報を返す。
     *
     * @param i bone index。
     * @return i 番目の FBoneInfo (範囲外なら nullptr)。
     */
    const FBoneInfo* Bone(u32 i) const noexcept {
        return (i < m_Bones.Size()) ? &m_Bones[i] : nullptr;
    }

    /**
     * 保持中の animation clip 数を返す。
     *
     * @return animation clip 数。
     */
    u32 AnimationClipCount() const noexcept {
        return static_cast<u32>(m_Clips.Size());
    }

    /**
     * i 番目の animation clip 情報を返す。
     *
     * @param i clip index。
     * @return i 番目の FAnimationClipInfo (範囲外なら nullptr)。
     */
    const FAnimationClipInfo* AnimationClip(u32 i) const noexcept {
        return (i < m_Clips.Size()) ? &m_Clips[i] : nullptr;
    }

    /**
     * 何らかの model が UpdateFromModel 経由で push されたかを返す。
     *
     * @return push 済みなら true。
     */
    bool HasModel() const noexcept { return m_HasModel; }

    /**
     * ImGui::Begin に渡すウインドウタイトルを返す。
     *
     * @return 固定リテラル "Model Info"。
     */
    const char* Title() const noexcept override { return "Model Info"; }

    /**
     * メインの ImGui 描画を行う。
     *
     * @details
     * ImGui::Begin("Model Info") + Summary (常時表示) / Submeshes (CollapsingHeader + Table) /
     * Bones (CollapsingHeader + TreeNode 再帰) / Animation Clips (CollapsingHeader + Table) の
     * 4 セクションを順に描画する。model 未 load (!m_HasModel) の場合は "(No model loaded)" を
     * 表示してセクションは描画しない。
     */
    void DrawUI() noexcept override;

private:
    /** model 全体の集計情報 (単一 summary)。 */
    FMeshSummary                m_Summary{};

    /** submesh 情報配列。 */
    acs::TArray<FSubmeshInfo>    m_Submeshes;

    /** bone 情報配列。 */
    acs::TArray<FBoneInfo>       m_Bones;

    /** animation clip 情報配列。 */
    acs::TArray<FAnimationClipInfo> m_Clips;

    /** UpdateFromModel 済みフラグ。 */
    bool                       m_HasModel      = false;

    /**
     * bone 部分木を TreeNode で再帰描画する内部ヘルパ。
     *
     * @details
     * bone_index を root とし、全 bone を線形走査して parent_index == bone_index の子を
     * 再帰描画する。depth は不正な parent_index による無限再帰を防ぐガード。
     * @param bone_index 描画する bone の index。
     * @param depth 現在の再帰深度 (kBoneRecursionLimit 到達で打ち切る)。
     */
    void DrawBoneRecursive(i32 bone_index, u32 depth) noexcept;
};

using FModelInspectorPanel = AModelInspectorPanel;

} // namespace acs::game::modelview
