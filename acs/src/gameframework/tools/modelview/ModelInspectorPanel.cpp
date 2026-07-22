// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / FModelInspectorPanel 実装
//
// 仕様の意図は FModelInspectorPanel.h を参照。本ファイルでは:
//   ・UpdateFromModel: summary + 3 配列の値コピーと has_model フラグ更新
//   ・Clear / Init / Shutdown: 内部状態の初期化
//   ・DrawUI: ImGui で 4 セクションを描画
//       - Summary           : Text 一覧 (vertex / triangle / submesh / material /
//                               bone / clip 数 + bounding sphere)
//       - Submeshes         : CollapsingHeader + Table (Name / Start / Count /
//                               Material Slot)
//       - Bones             : CollapsingHeader + TreeNode (parent_index < 0 を
//                               root として再帰展開)
//       - Animation Clips    : CollapsingHeader + Table (名前 / 長さ / サンプル数
//                               / Looping)
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/modelview/ModelInspectorPanel.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (label / 数値整形)

namespace acs::game::modelview {

/**
 * bone 階層描画の再帰深度上限 (不正な parent_index による無限ループ防衛)。
 *
 * @details 実 skeleton では 32 階層を超えることはまず無いが、64 に余裕を取る。
 */
static constexpr u32 kBoneRecursionLimit = 64u;

/**
 * caller が nullptr を渡した場合に "(unnamed)" を返す安全ラベル。
 *
 * @param name 元の名前 (nullptr 可)。
 * @return name が非 null ならそのまま、nullptr なら "(unnamed)"。
 */
static const char* SafeName(const char* name) noexcept {
    return (name != nullptr) ? name : "(unnamed)";
}

/** 内部状態を空にする (summary はゼロ初期化、3 配列を Clear、has_model = false)。 */
void FModelInspectorPanel::Init() noexcept {
    // m_Summary は値型 = ゼロ初期化に戻す。
    m_Summary    = FMeshSummary{};
    m_Submeshes.Clear();
    m_Bones.Clear();
    m_Clips.Clear();
    m_HasModel  = false;
}

/** Init と同じ深さで内部状態を全 reset する (容量は destructor が解放)。 */
void FModelInspectorPanel::Shutdown() noexcept {
    // TArray は Clear で要素数 0。容量解放は panel 自身の destructor に任せ、
    // ここでは要素破棄のみで十分。
    m_Summary    = FMeshSummary{};
    m_Submeshes.Clear();
    m_Bones.Clear();
    m_Clips.Clear();
    m_HasModel  = false;
}

/** caller から渡された summary + 3 配列を値コピーで全置換し has_model = true にする。 */
void FModelInspectorPanel::UpdateFromModel(const FMeshSummary&        summary,
                                          const FSubmeshInfo*        submeshes,
                                          u32                       submesh_count,
                                          const FBoneInfo*           bones,
                                          u32                       bone_count,
                                          const FAnimationClipInfo*  clips,
                                          u32                       clip_count) noexcept {
    m_Summary = summary;

    // Submeshes。
    m_Submeshes.Clear();
    if (submeshes != nullptr && submesh_count > 0) {
        m_Submeshes.Reserve(submesh_count);
        for (u32 i = 0; i < submesh_count; ++i) {
            // SubmeshInfo は POD (const char* + u32 三つ) なので値コピーで OK。
            // TArray::PushBack は perfect forward され T(...) で構築する。
            m_Submeshes.PushBack(submeshes[i]);
        }
    }

    // Bones。
    m_Bones.Clear();
    if (bones != nullptr && bone_count > 0) {
        m_Bones.Reserve(bone_count);
        for (u32 i = 0; i < bone_count; ++i) {
            m_Bones.PushBack(bones[i]);
        }
    }

    // Animation Clips。
    m_Clips.Clear();
    if (clips != nullptr && clip_count > 0) {
        m_Clips.Reserve(clip_count);
        for (u32 i = 0; i < clip_count; ++i) {
            m_Clips.PushBack(clips[i]);
        }
    }

    m_HasModel = true;
}

/** 表示内容を空に戻し "No model loaded" 状態にする。 */
void FModelInspectorPanel::Clear() noexcept {
    m_Summary    = FMeshSummary{};
    m_Submeshes.Clear();
    m_Bones.Clear();
    m_Clips.Clear();
    m_HasModel  = false;
}

/** bone_index を root とする部分木を TreeNode で再帰描画する (depth でガード)。 */
void FModelInspectorPanel::DrawBoneRecursive(i32 bone_index, u32 depth) noexcept {
    if (depth >= kBoneRecursionLimit) {
        ImGui::TextDisabled("  (bone depth limit reached)");
        return;
    }
    if (bone_index < 0 || static_cast<u32>(bone_index) >= m_Bones.Size()) {
        // 範囲外 = データ破損 / 不正 parent。安全に no-op。
        return;
    }

    const FBoneInfo& bone = m_Bones[static_cast<u32>(bone_index)];

    // この bone を親とする子 bone があるかを線形走査で先に確認。
    // (TreeNode の Leaf flag を正確に立てるため = 矢印表示の精度が上がる)。
    bool has_child = false;
    for (u32 i = 0; i < m_Bones.Size(); ++i) {
        if (m_Bones[i].parent_index == bone_index) {
            has_child = true;
            break;
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!has_child) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    // 同名の bone が複数あっても ID が衝突しないよう bone_index を PushID。
    ImGui::PushID(bone_index);

    char label[128];
    std::snprintf(label, sizeof(label),
                  "%s  [%d]",
                  SafeName(bone.name),
                  static_cast<int>(bone_index));

    const bool open = ImGui::TreeNodeEx("##bone_tn", flags, "%s", label);

    if (open && has_child) {
        // 子 bone を線形走査して再帰描画。元配列を走査するため index は安定。
        for (u32 i = 0; i < m_Bones.Size(); ++i) {
            if (m_Bones[i].parent_index == bone_index) {
                DrawBoneRecursive(static_cast<i32>(i), depth + 1u);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

/** ImGui::Begin("Model Info") + Summary / Submeshes / Bones / Animation Clips を描画する。 */
void FModelInspectorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        // 折りたたまれている等、描画が無効化されている場合は End だけ呼んで return。
        ImGui::End();
        return;
    }

    if (!m_HasModel) {
        ImGui::TextDisabled("(No model loaded)");
        ImGui::TextDisabled("Drop a model from the Asset Browser into the Model Viewer.");
        ImGui::End();
        return;
    }

    // 1) Summary セクション — 常時表示、CollapsingHeader 外で簡潔に出す。
    // CollapsingHeader にせず素の Text 群にする理由: summary は本 panel で最も
    // 高頻度に確認される情報のため、開閉を許さず常に視認できる位置に置く
    // (Unity の Mesh Inspector もヘッダー直下に常時表示する設計)。
    ImGui::SeparatorText("Summary");
    ImGui::Text("Vertices       : %u", static_cast<unsigned>(m_Summary.vertex_count));
    ImGui::Text("Triangles      : %u", static_cast<unsigned>(m_Summary.triangle_count));
    ImGui::Text("Submeshes      : %u", static_cast<unsigned>(m_Summary.submesh_count));
    ImGui::Text("Material slots : %u", static_cast<unsigned>(m_Summary.material_slot_count));
    ImGui::Text("Bones          : %u", static_cast<unsigned>(m_Summary.bone_count));
    ImGui::Text("Animation clips: %u", static_cast<unsigned>(m_Summary.animation_clip_count));
    ImGui::Text("Bounding center: (%.3f, %.3f, %.3f)",
                static_cast<double>(m_Summary.bounding_center.x),
                static_cast<double>(m_Summary.bounding_center.y),
                static_cast<double>(m_Summary.bounding_center.z));
    ImGui::Text("Bounding radius: %.3f",
                static_cast<double>(m_Summary.bounding_radius));

    ImGui::Spacing();

    // 2) Submeshes セクション — CollapsingHeader + Table。
    // 大きな model では数十 submesh あり得るので、開閉できる方が UX 上望ましい。
    // Table は ImGuiTableFlags_Borders + RowBg で見やすく整形する。
    if (ImGui::CollapsingHeader("Submeshes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_Submeshes.IsEmpty()) {
            ImGui::TextDisabled("  (no submeshes)");
        } else {
            const ImGuiTableFlags table_flags =
                ImGuiTableFlags_Borders
              | ImGuiTableFlags_RowBg
              | ImGuiTableFlags_SizingStretchProp
              | ImGuiTableFlags_ScrollY;

            // 高さは概ね 8 行分にクランプ (= 大きな mesh で window を占有しすぎない)。
            const float row_h    = ImGui::GetTextLineHeightWithSpacing();
            const float table_h  = row_h * 9.0f;  // header 1 + body 8

            if (ImGui::BeginTable("##submesh_table", 4, table_flags, ImVec2(0.0f, table_h))) {
                ImGui::TableSetupScrollFreeze(0, 1);  // ヘッダー固定
                ImGui::TableSetupColumn("Name",          ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Index Start",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Index Count",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Material Slot", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();

                for (u32 i = 0; i < m_Submeshes.Size(); ++i) {
                    const FSubmeshInfo& sm = m_Submeshes[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(SafeName(sm.name));

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", static_cast<unsigned>(sm.index_start));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", static_cast<unsigned>(sm.index_count));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", static_cast<unsigned>(sm.material_slot_index));
                }

                ImGui::EndTable();
            }
        }
    }

    // 3) Bones セクション — CollapsingHeader + TreeNode 再帰。
    // root bone (= parent_index < 0) を全て列挙し、各 root から DrawBoneRecursive。
    // 一般のキャラクタは root が 1 つだが、ACS は複数 root を許容する (=
    // 武器 attach 用の補助 skeleton root 等)。
    if (ImGui::CollapsingHeader("Bones", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_Bones.IsEmpty()) {
            ImGui::TextDisabled("  (no skeleton)");
        } else {
            // root bone を線形走査で全て描画。
            // parent_index < 0 もしくは「parent_index が m_Bones 範囲外」を root とみなす
            // (= 不正データに対する fail-safe で、孤立した bone も表示できるように)。
            bool drew_any_root = false;
            for (u32 i = 0; i < m_Bones.Size(); ++i) {
                const i32 pi = m_Bones[i].parent_index;
                const bool is_root =
                    (pi < 0) ||
                    (static_cast<u32>(pi) >= m_Bones.Size());
                if (is_root) {
                    DrawBoneRecursive(static_cast<i32>(i), 0u);
                    drew_any_root = true;
                }
            }
            if (!drew_any_root) {
                // 全ての bone が「親あり」 = 循環している = データ破損。
                // 何も描画しないとユーザが「panel が壊れている?」と誤解するので
                // メッセージを出す。
                ImGui::TextDisabled("  (no root bone — possible cycle in skeleton)");
            }
        }
    }

    // 4) Animation Clips セクション — CollapsingHeader + Table。
    if (ImGui::CollapsingHeader("Animation Clips", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_Clips.IsEmpty()) {
            ImGui::TextDisabled("  (no animation clips)");
        } else {
            const ImGuiTableFlags table_flags =
                ImGuiTableFlags_Borders
              | ImGuiTableFlags_RowBg
              | ImGuiTableFlags_SizingStretchProp
              | ImGuiTableFlags_ScrollY;

            const float row_h   = ImGui::GetTextLineHeightWithSpacing();
            const float table_h = row_h * 9.0f;

            if (ImGui::BeginTable("##anim_clip_table", 4, table_flags, ImVec2(0.0f, table_h))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Samples",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Looping",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();

                for (u32 i = 0; i < m_Clips.Size(); ++i) {
                    const FAnimationClipInfo& clip = m_Clips[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(SafeName(clip.name));

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f s", static_cast<double>(clip.duration_sec));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", static_cast<unsigned>(clip.sample_count));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(clip.is_looping ? "yes" : "no");
                }

                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
}

} // namespace acs::game::modelview
