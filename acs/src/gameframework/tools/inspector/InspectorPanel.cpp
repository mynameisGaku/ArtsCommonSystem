// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FInspectorPanel 実装 (Phase 20 editor 第二弾)
//
// 仕様の意図は FInspectorPanel.h を参照。本ファイルでは:
//   ・FSelectionService からの FNodeId 取得 (forward-decl 経由)
//   ・`FInspectorSeam::GetProvider(FNodeId)` を介した Provider 取得
//   ・`InspectableObject::fields` の 1 件ずつを EFieldKind に応じた
//     ImGui widget に変換 (Bool / I32 / U32 / F32 / FVec2 / FVec3 / FVec4 /
//     FColor / FString / Enum)
//   ・編集発生時に dirty flag を立て + FieldChangeCallback を発火
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/inspector/InspectorPanel.h"

#include "gameframework/InspectorSeam.h"
#include "gameframework/tools/inspector/SelectionService.h"  // 別エージェントが提供する SelectionService の本物 API
#include "math/Vec.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (label 整形)
#include <cstring>  // std::strncpy / std::strlen (String 編集バッファ)

namespace acs::game::inspector {

// FSelectionService は同名前空間にあり、ヘッダで forward-decl 済み。
// 実 API (`CurrentSelection()` 等) はこの .cpp から直接呼ぶ。

// =============================================================================
// ローカルヘルパ
// =============================================================================

// 選択 FNodeId から、その node に紐付いた Provider を seam 経由で逆引きする。
//
// 【修正の背景】以前はここで `seam.GetProvider(id.Index())` を呼んでいたが、
// これは誤り。FNodeId::Index() は「シーングラフ上の slot index」であり、
// seam の GetProvider(u32) が取る「provider レジストリ上の登録順 index」とは
// 別の index 空間。両者は一般に一致しないため (例: node index=1 の provider が
// 登録順 0 番目、等)、別 provider または範囲外 nullptr を引く偽実装だった。
//
// 正しい解決は node identity (FNodeId 完全一致) での逆引き。seam に追加した
// node-keyed lookup `GetProviderForNode(FNodeId)` を使う。これは
// RegisterProviderForNode で記録した node_id に完全一致する provider を返す。
static IInspectableProvider* ResolveProvider(FInspectorSeam& seam, FNodeId id) noexcept {
    if (!id.IsValid()) {
        return nullptr;
    }
    return seam.GetProviderForNode(id);
}

// 1 InspectableField を ImGui 上に描画 / 編集する。値が書き換わったら
// `true` を返す。`changed_field_name` には書き換わったフィールドの name を
// 出力 (callback 呼び出し用)。
//
// FString 編集は「Provider 所有の固定領域に書き戻す」と挙動が壊れる (リテラル
// に書く危険) ため、Phase 20 では **read + 編集はパネル内バッファに対してのみ**
// 行い、Provider 側への反映は callback 経由で外部に任せる暫定運用にする。
// (= ImGui::InputText は ReadOnly フラグなしで動かすが、本 panel は data
//  ポインタを const char* として読み、書き戻しは行わない)。
//
// kind 分岐は switch にまとめてレジスタ局所化を狙う。各 case は ImGui の
// 該当 widget を呼び、戻り値が true なら m_Dirty を立てる。
static bool DrawField(InspectableField& field) noexcept {
    if (field.name == nullptr || field.data == nullptr) {
        // データ未設定の field は描画しない (no-op + change=false)。
        ImGui::TextDisabled("(invalid field)");
        return false;
    }

    bool changed = false;

    // PushID で同名 field の ID 衝突を避ける (= field.name を ID 種に使う)。
    ImGui::PushID(field.name);

    switch (field.kind) {
    case EFieldKind::Bool: {
        bool* p = static_cast<bool*>(field.data);
        changed = ImGui::Checkbox(field.name, p);
        break;
    }

    case EFieldKind::I32: {
        i32* p = static_cast<i32*>(field.data);
        // ImGui::InputInt は int* を要求するが i32 = int32_t == int (Windows MSVC) で
        // 一致する前提。アライメントも同じ。
        changed = ImGui::InputInt(field.name, reinterpret_cast<int*>(p));
        break;
    }

    case EFieldKind::U32: {
        u32* p = static_cast<u32*>(field.data);
        // InputScalar は型タグで分岐するため、UI 表記は %u になる。
        changed = ImGui::InputScalar(field.name, ImGuiDataType_U32, p);
        break;
    }

    case EFieldKind::F32: {
        f32* p = static_cast<f32*>(field.data);
        // 現状の InspectableField には min/max metadata が無いため、暫定値
        // [kDefaultSliderMin, kDefaultSliderMax] を使う。
        // metadata 拡張後はここで field の min/max を参照する。
        changed = ImGui::SliderFloat(field.name,
                                      p,
                                      FInspectorPanel::kDefaultSliderMin,
                                      FInspectorPanel::kDefaultSliderMax,
                                      "%.3f");
        break;
    }

    case EFieldKind::FVec2: {
        // acs::FVec2 = { f32 x, f32 y } で連続レイアウト。f32[2] として直接渡す。
        FVec2* p = static_cast<FVec2*>(field.data);
        f32 tmp[2] = { p->x, p->y };
        if (ImGui::DragFloat2(field.name, tmp, 0.1f)) {
            p->x = tmp[0];
            p->y = tmp[1];
            changed = true;
        }
        break;
    }

    case EFieldKind::FVec3: {
        // acs::FVec3 は alignas(16) で内部に m_Pad を含む。x/y/z は連続するが
        // ImGui に &p->x を直接渡すと m_Pad を踏まずに 3 要素読むので安全。
        // ただし可読性のため一時配列に詰める。
        FVec3* p = static_cast<FVec3*>(field.data);
        f32 tmp[3] = { p->x, p->y, p->z };
        if (ImGui::DragFloat3(field.name, tmp, 0.1f)) {
            p->x = tmp[0];
            p->y = tmp[1];
            p->z = tmp[2];
            changed = true;
        }
        break;
    }

    case EFieldKind::FVec4: {
        // 仕様で要求されていないが対称性のため対応 (EFieldKind に値が存在
        // するので未対応放置は警告源)。acs::FVec4 は連続 f32[4]。
        f32* p = static_cast<f32*>(field.data);
        changed = ImGui::DragFloat4(field.name, p, 0.1f);
        break;
    }

    case EFieldKind::FString: {
        // FString は read-only 想定 (FInspectorSeam.h の Phase 2 仕様)。
        // バッファコピー後 InputText を出すが、書き戻しは行わない
        // (= panel 内のローカル編集のみ、永続化は callback 経由で外部担当)。
        //
        // 【修正】EFieldKind::FString の data は `const char**` (= 文字列ポインタ
        // への間接) が仕様 (FInspectorSeam.h enum コメント参照)。以前は data を
        // 直接 `const char*` とキャストしており、ポインタのポインタを文字列
        // 本体ポインタとして誤読していた (= 文字データではなくアドレス値の
        // バイト列を文字列として読む不正アクセス)。正しくは一段 deref する。
        const char* const* pp = static_cast<const char* const*>(field.data);
        const char* src = (pp != nullptr) ? *pp : nullptr;
        if (src == nullptr) src = "";
        char buf[FInspectorPanel::kStringBufferSize] = {};
        // strncpy_s 系は Windows 限定なので std::strncpy + 末尾 NUL 確実化で。
        std::strncpy(buf, src, FInspectorPanel::kStringBufferSize - 1);
        buf[FInspectorPanel::kStringBufferSize - 1] = '\0';
        if (ImGui::InputText(field.name, buf, FInspectorPanel::kStringBufferSize)) {
            // 書き戻しは現状しない (read-only)。callback 経由で外部が処理する。
            changed = true;
        }
        break;
    }

    case EFieldKind::Enum: {
        // i32* + enum_values[0..enum_value_count) の文字列配列。
        // ImGui::Combo は const char* const items[] を要求するので、
        // enum_values を直接渡せる (キャストで合わせる)。
        i32* p = static_cast<i32*>(field.data);
        if (field.enum_values == nullptr || field.enum_value_count == 0) {
            // ラベル未設定の Enum は壊れていると見なし、disabled 表記で描画。
            ImGui::TextDisabled("%s: (enum has no labels)", field.name);
        } else {
            // Combo の preview は現在値の label を見せる。範囲外なら clamp。
            i32 idx = *p;
            if (idx < 0) idx = 0;
            if (static_cast<u32>(idx) >= field.enum_value_count) {
                idx = static_cast<i32>(field.enum_value_count) - 1;
            }
            int int_idx = static_cast<int>(idx);
            if (ImGui::Combo(field.name,
                             &int_idx,
                             field.enum_values,
                             static_cast<int>(field.enum_value_count))) {
                *p     = static_cast<i32>(int_idx);
                changed = true;
            }
        }
        break;
    }

    default: {
        // 未知の kind は警告表示のみ。EFieldKind は u8 enum なので想定外値も
        // 起こりうる (バイナリ互換時の古い data 等)。
        ImGui::TextDisabled("%s: (unsupported kind)", field.name);
        break;
    }
    }

    ImGui::PopID();
    return changed;
}

// =============================================================================
// Init / Shutdown
// =============================================================================

void FInspectorPanel::Init() noexcept {
    // 完全リセット。多重 Init を許容するため、ここでは selection / dirty を
    // クリアするだけで callback は触らない (Init は state リセット、callback
    // のクリアは Shutdown / Set*() の責務)。
    m_CurrentSelection = FNodeId {};
    m_Dirty             = false;
    // m_SelectionService / callback は維持 (= 再 Init で外部設定を壊さない)。
}

void FInspectorPanel::Shutdown() noexcept {
    // 全状態を初期に戻す。外部所有の FSelectionService / callback ターゲットを
    // 破棄するわけではないが、本パネルからの参照は外す。
    m_CurrentSelection  = FNodeId {};
    m_SelectionService  = nullptr;
    m_Dirty              = false;
    m_OnChangeCb       = nullptr;
    m_OnChangeUser     = nullptr;
}

// =============================================================================
// SetSelectionService / SetOnFieldChangeCallback
// =============================================================================

void FInspectorPanel::SetSelectionService(FSelectionService* svc) noexcept {
    // non-owning 保存。nullptr で解除可能。
    m_SelectionService = svc;
}

void FInspectorPanel::SetOnFieldChangeCallback(FieldChangeCallback cb, void* user) noexcept {
    m_OnChangeCb   = cb;
    m_OnChangeUser = user;
}

// =============================================================================
// DrawUI — メイン ImGui window
// =============================================================================
// ・FSelectionService が居れば Current() を優先採用、なければ引数 selected_id。
// ・Provider が無効なら "(No provider)" を表示して return。
// ・Provider の各オブジェクトを type/instance ヘッダ + field 配列で描画。
// ・field が変わったら m_Dirty を立て + callback を発火。
// =============================================================================
void FInspectorPanel::DrawUI() noexcept {
    // Phase 24: FEditorPanel 継承で no-param 化。
    // - FInspectorSeam は SetInspectorSeam で事前 set
    // - FNodeId は FSelectionService::CurrentSelection() で取得 (なければ invalid)
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    if (m_InspectorSeam == nullptr) {
        ImGui::TextUnformatted("(no FInspectorSeam attached; call SetInspectorSeam)");
        ImGui::End();
        return;
    }
    FInspectorSeam& seam = *m_InspectorSeam;

    // ----- 選択 FNodeId の解決 -----
    FNodeId effective {};
    if (m_SelectionService != nullptr) {
        FNodeId from_svc = m_SelectionService->CurrentSelection();
        if (from_svc.IsValid()) {
            effective = from_svc;
        }
    }
    m_CurrentSelection = effective;

    // ヘッダ: 現在の選択 FNodeId を表示 (デバッグ + 視認性)。
    if (!effective.IsValid()) {
        ImGui::TextDisabled("(Nothing selected)");
        ImGui::End();
        return;
    }
    ImGui::Text("FNodeId: index=%u gen=%u",
                static_cast<unsigned>(effective.Index()),
                static_cast<unsigned>(effective.Generation()));
    ImGui::Separator();

    // ----- Provider 解決 -----
    IInspectableProvider* prov = ResolveProvider(seam, effective);
    if (prov == nullptr) {
        ImGui::TextDisabled("(No provider registered for this FNodeId)");
        ImGui::End();
        return;
    }

    // ----- 各オブジェクトの描画 -----
    const u32 obj_count = prov->ObjectCount();
    if (obj_count == 0) {
        ImGui::TextDisabled("(Provider exposes 0 objects)");
        ImGui::End();
        return;
    }

    for (u32 obj_index = 0; obj_index < obj_count; ++obj_index) {
        InspectableObject obj = prov->GetObject(obj_index);

        // オブジェクトヘッダ "[TypeName] InstanceName" を CollapsingHeader で。
        // PushID で obj_index 単位の ID 名前空間を作る (= 同 type が複数並んでも
        // ImGui ID が衝突しない)。
        ImGui::PushID(static_cast<int>(obj_index));

        char header_label[160] = {};
        const char* type_name = (obj.type_name     != nullptr) ? obj.type_name     : "(type?)";
        const char* inst_name = (obj.instance_name != nullptr) ? obj.instance_name : "";
        std::snprintf(header_label, sizeof(header_label), "[%s] %s", type_name, inst_name);

        // 既定で開いておく (= 1 オブジェクトしか無いケースで折り畳まれていない
        // 方が UX として親切)。
        if (ImGui::CollapsingHeader(header_label, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (obj.fields == nullptr || obj.field_count == 0) {
                ImGui::TextDisabled("  (no fields)");
            } else {
                for (u32 f = 0; f < obj.field_count; ++f) {
                    InspectableField& field = obj.fields[f];
                    if (DrawField(field)) {
                        // field 編集発生: dirty 立て、Provider 通知、callback 発火。
                        m_Dirty = true;
                        // Provider への通知 (clamp / 派生値再計算 hook)。
                        prov->OnFieldChanged(obj_index, f);
                        // 外部 callback (永続化 / undo) への通知。
                        if (m_OnChangeCb != nullptr) {
                            m_OnChangeCb(m_OnChangeUser,
                                          effective,
                                          field.name,
                                          field.kind);
                        }
                    }
                }
            }
        }

        ImGui::PopID();
    }

    // ----- フッタ: dirty 状態のインジケータ -----
    ImGui::Separator();
    if (m_Dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "* modified");
    } else {
        ImGui::TextDisabled("(no changes)");
    }

    ImGui::End();
}

} // namespace acs::game::inspector
