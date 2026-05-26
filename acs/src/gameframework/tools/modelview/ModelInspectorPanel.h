// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / ModelInspectorPanel (Phase 21b ModelViewer 第二弾)
//
// ModelViewer ワークスペース内に配置される **読み取り専用の mesh 情報 panel**。
// AssetBrowser から選択 / DragDrop された model (.mdl / .fbx / .gltf 等) を
// ModelViewerPanel (別エージェント) が load した結果を、本 panel に
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
//     parse は ModelViewerPanel (別エージェント) と Mesh / Skeleton / AnimationClip
//     モジュールの責任。caller (= ModelViewerPanel もしくは sample 31) が
//     load 完了時に本 panel の `UpdateFromModel` を呼んで情報を流し込む。
//   ・本 panel は callback / 編集 / GPU リソース確保を一切持たない (= 純粋な
//     read-only viewer)。書き込みが必要になったら Phase 21c で別 panel を追加する。
//
// 設計選択 (Phase 21b modelview):
//   ・**EditorPanel 継承**: Phase 21a で確立した editor_core 基底に乗せる。
//     ModelViewerPanel / sample 31 が EditorWorkspace 経由で本 panel を
//     register する。Title は "Model Info" (Unity / Godot の Inspector 表記寄り)。
//   ・**スナップショット方式**: 元の Mesh / Skeleton / AnimationClip オブジェクト
//     を ref で保持しない (= モデル再 load / 解放と本 panel の表示が race しない
//     ように、name / index 等を `acs::TArray<...>` に値コピーで持つ)。`const char*`
//     name は caller 側が「panel の寿命 ≧ 文字列の寿命」を保証する責務だが、
//     典型用途では Mesh モジュールの permanent storage を直接参照する想定なので
//     安全 (= ImGui draw までに開放されない)。
//   ・**3 つの可変長配列 + 1 つの fixed struct**: submeshes / bones / clips は
//     model ごとに件数が変わるため `acs::TArray<T>`。summary は単一 struct で
//     値保持。AssetBrowser / HierarchyPanel と同形の "TArray<T> を内部に持つ
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
//     (ParticleEditorPanel / InspectorPanel / HierarchyPanel と同形)。
//   ・**bone hierarchy は描画時にオンザフライ走査**: 子リストを事前構築せず、
//     各 node 描画時に全 bone を線形走査して `parent_index == this_index` の
//     子を見つけて再帰する。bone 数 100k 級でなければ十分速く、メモリ追加なし。
//     大規模化したら parent → children インデックスの bucket 配列を Phase 21c で
//     追加。
//   ・**Submesh / AnimationClip は ImGui::BeginTable**: name / index / count を
//     等幅で見せたい用途に Table API が最適。CollapsingHeader でセクション
//     開閉、Table で行ごとの値表示、という 2 段構成。
//   ・**bounding_radius / bounding_center は FVec3 + f32**: Mesh モジュールの
//     既存 bounds API (= AABB or FSphere) のうち、最も典型的な「中心 + 半径」を
//     直接渡せる形にする。AABB が欲しい場合は Phase 21c で別フィールド追加。
//
// 範囲外 (Phase 21b 時点で持たない、将来追加候補):
//   ・vertex buffer の hex dump 表示
//   ・bone influence weight の matrix 表示
//   ・animation clip の preview (= 別 panel ModelAnimationPanel で Phase 21c 予定)
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

// ---------------------------------------------------------------------------
// MeshSummary — model 全体の集計情報 (単一インスタンス、値コピー保持)
// ---------------------------------------------------------------------------
// caller (ModelViewerPanel 等) が model load 後に集計して本 panel に渡す。
// 全フィールド値型のため、Mesh / Skeleton / AnimationClip 側のリソース解放と
// 本 panel の表示は完全に decouple される。
struct MeshSummary {
    u32        vertex_count          = 0;   // 全 vertex 数 (submesh 跨ぎ合算後)
    u32        triangle_count        = 0;   // 全 triangle 数 (index_count / 3 合算)
    u32        submesh_count         = 0;   // submesh セクション数
    u32        material_slot_count   = 0;   // material slot 数 (= 異なる material の数)
    u32        bone_count            = 0;   // skeleton 内 bone 数 (skeleton 無しなら 0)
    u32        animation_clip_count  = 0;   // 関連付けられた animation clip 数 (0 可)
    f32        bounding_radius       = 0.0f;// model 全体の bounding sphere 半径
    acs::FVec3  bounding_center       = {};  // 同 bounding sphere の中心 (object 空間)
};

// ---------------------------------------------------------------------------
// SubmeshInfo — 1 submesh セクションの情報 (name + index 範囲 + material slot)
// ---------------------------------------------------------------------------
// name は caller 所有のリテラル / permanent string を想定 (panel 寿命 ≦ name 寿命)。
struct SubmeshInfo {
    const char* name                 = nullptr; // 例: "Body", "Hair", "Eyes"
    u32         index_start          = 0;       // 全 index buffer 内の開始 index
    u32         index_count          = 0;       // この submesh の index 数 (3 の倍数想定)
    u32         material_slot_index  = 0;       // この submesh が使う material slot
};

// ---------------------------------------------------------------------------
// BoneInfo — 1 bone の情報 (name + 親 index)
// ---------------------------------------------------------------------------
// parent_index < 0 は root bone を意味する。本 panel は parent_index から子を
// 線形走査で見つけて TreeNode 階層を再構築する (= 子リストは保持しない)。
struct BoneInfo {
    const char* name         = nullptr; // 例: "Hips", "Spine_01"
    i32         parent_index = -1;      // 同 bones 配列内の親 index、root は -1
};

// ---------------------------------------------------------------------------
// AnimationClipInfo — 1 animation clip の情報
// ---------------------------------------------------------------------------
struct AnimationClipInfo {
    const char* name         = nullptr; // 例: "Idle", "Run", "Attack01"
    f32         duration_sec = 0.0f;    // クリップの長さ (秒)
    u32         sample_count = 0;       // サンプル数 (= keyframe 数の合計など)
    bool        is_looping   = false;   // ループ再生フラグ (= clip metadata)
};

// ---------------------------------------------------------------------------
// ModelInspectorPanel — read-only mesh / skeleton / anim 情報ビューア
// ---------------------------------------------------------------------------
class ModelInspectorPanel : public editor_core::EditorPanel {
public:
    ModelInspectorPanel() noexcept = default;
    ~ModelInspectorPanel() noexcept override = default;

    // 非コピー / 非ムーブ: 内部 TArray<...> + has_model 状態の所有を曖昧にしない
    // (EditorPanel 基底自体も非コピー / 非ムーブ宣言済 = 規約上の継承)。
    ModelInspectorPanel(const ModelInspectorPanel&)            = delete;
    ModelInspectorPanel& operator=(const ModelInspectorPanel&) = delete;
    ModelInspectorPanel(ModelInspectorPanel&&)                 = delete;
    ModelInspectorPanel& operator=(ModelInspectorPanel&&)      = delete;

    // ----- ライフサイクル ---------------------------------------------------

    // 初期化: 内部状態を空にする。多重 Init 可。
    void Init() noexcept;

    // 後片付け: 全配列 + summary をクリア、has_model = false。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- データ流し込み (caller = ModelViewerPanel 等) -------------------

    // 現在表示する model のスナップショットを更新する。本 panel は値コピーして
    // 内部に保持する (submeshes / bones / clips は配列要素を値コピー)。
    // 各ポインタは nullptr 可 (count == 0 と整合させること)。
    // ・summary: 単一 struct を値コピーで保存
    // ・submeshes[0..submesh_count): SubmeshInfo を TArray に PushBack コピー
    // ・bones[0..bone_count)        : BoneInfo を TArray に PushBack コピー
    // ・clips[0..clip_count)        : AnimationClipInfo を TArray に PushBack コピー
    // 呼び出し後、`has_model = true` になり DrawUI が情報を表示する。
    void UpdateFromModel(const MeshSummary&        summary,
                         const SubmeshInfo*        submeshes,
                         u32                       submesh_count,
                         const BoneInfo*           bones,
                         u32                       bone_count,
                         const AnimationClipInfo*  clips,
                         u32                       clip_count) noexcept;

    // 表示内容を空に戻す ("No model loaded" 表示に切り替わる)。
    // model unload や workspace clear から呼ばれる想定。多重 Clear 可。
    void Clear() noexcept;

    // ----- 読み出しアクセサ ------------------------------------------------

    const MeshSummary& Summary() const noexcept { return _summary; }

    u32 SubmeshCount() const noexcept { return static_cast<u32>(_submeshes.Size()); }
    // i >= SubmeshCount() のとき nullptr を返す (= 防衛的アクセス、UB なし)。
    const SubmeshInfo* Submesh(u32 i) const noexcept {
        return (i < _submeshes.Size()) ? &_submeshes[i] : nullptr;
    }

    u32 BoneCount() const noexcept { return static_cast<u32>(_bones.Size()); }
    const BoneInfo* Bone(u32 i) const noexcept {
        return (i < _bones.Size()) ? &_bones[i] : nullptr;
    }

    u32 AnimationClipCount() const noexcept {
        return static_cast<u32>(_clips.Size());
    }
    const AnimationClipInfo* AnimationClip(u32 i) const noexcept {
        return (i < _clips.Size()) ? &_clips[i] : nullptr;
    }

    // 何らかの model が UpdateFromModel 経由で push されたか。
    bool HasModel() const noexcept { return _has_model; }

    // ----- EditorPanel override --------------------------------------------

    // ImGui::Begin に渡すウインドウタイトル (リテラル文字列)。
    const char* Title() const noexcept override { return "Model Info"; }

    // メインの ImGui 描画: ImGui::Begin("Model Info") + 4 セクションを順に描画。
    //   - Summary           (常時表示、テキスト一覧)
    //   - Submeshes         (CollapsingHeader + Table)
    //   - Bones             (CollapsingHeader + TreeNode 再帰)
    //   - Animation Clips   (CollapsingHeader + Table)
    // model 未 load (`!_has_model`) の場合は "(No model loaded)" を表示して
    // セクションは描画しない。
    void DrawUI() noexcept override;

private:
    // ----- 内部状態 ---------------------------------------------------------
    MeshSummary                _summary{};                // 単一 summary
    acs::TArray<SubmeshInfo>    _submeshes;                // submesh 配列
    acs::TArray<BoneInfo>       _bones;                    // bone 配列
    acs::TArray<AnimationClipInfo> _clips;                 // animation clip 配列
    bool                       _has_model      = false;   // UpdateFromModel 済みフラグ

    // bone 階層描画用の内部ヘルパ (実装は .cpp 側)。
    // bone_index の子を全 bone から線形走査し、TreeNode で再帰描画する。
    // depth は無限再帰防止のためのガード (= 不正な parent_index による循環)。
    void DrawBoneRecursive(i32 bone_index, u32 depth) noexcept;
};

} // namespace acs::game::modelview
