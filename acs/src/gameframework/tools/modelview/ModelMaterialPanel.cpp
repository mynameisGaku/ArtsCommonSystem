// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / ModelMaterialPanel 実装 (Phase 21b)
//
// 仕様の意図は ModelMaterialPanel.h を参照。本ファイルでは:
//   ・slot list (Array<MaterialOverride>) の resize / reset / accessor
//   ・ImGui (CollapsingHeader + ColorEdit / SliderFloat / Button) によるレンダ
//   ・field 変更検知時の callback 発火
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/modelview/ModelMaterialPanel.h"

#include <imgui.h>

#include <cstdio>  // std::snprintf (slot label 整形)

namespace acs::game::modelview {

// =============================================================================
// ローカルヘルパ
// =============================================================================

// slot 1 件の表示ラベル "Slot NN" を整形する。将来 SetSlotName API が入ったら
// ここでアセット由来の material 名を返すように差し替える予定。
static void FormatSlotLabel(char* buf, usize buf_size, u32 slot_index) noexcept {
    if (buf == nullptr || buf_size == 0) return;
    std::snprintf(buf, buf_size, "Slot %u", static_cast<unsigned>(slot_index));
}

// slot index の境界チェック。
static inline bool IsValidSlot(u32 slot_index, u32 count) noexcept {
    return slot_index < count;
}

// MaterialOverride を default 値に戻す (slot_index は呼び出し側で再設定する)。
// 「Reset = is_overridden=false かつ各値を物理的に neutral な default に戻す」
// という規約をここに集約する (= 仕様変更時の修正箇所を 1 つに)。
static inline void ResetOverrideToDefault(MaterialOverride& o) noexcept {
    // slot_index は呼び出し側責務 (= Reset 後も slot_index は保持したいため
    // ここでは触らない)。
    o.base_color        = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    o.metallic          = 0.0f;
    o.roughness         = 0.5f;
    o.normal_strength   = 1.0f;
    o.ao_strength       = 1.0f;
    o.emissive          = Vec3{0.0f, 0.0f, 0.0f};
    o.emissive_intensity = 1.0f;
    o.is_overridden     = false;
}

// =============================================================================
// Init / Shutdown
// =============================================================================

void ModelMaterialPanel::Init() noexcept {
    // slot list を空に戻す (= 多重 Init 時の状態リセット)。
    // 容量は破棄せず再利用 (= 再 Init / model 再 load のアロケーション節約)。
    _overrides.Clear();
    // callback は維持 (= Init は state リセット責務、callback 解除は Shutdown 責務)。
}

void ModelMaterialPanel::Shutdown() noexcept {
    // 全状態を初期に戻す。Array はデストラクタで自然解放されるが、明示 Clear
    // することで「多重 Shutdown 後の状態」を確定させる。
    _overrides.Clear();
    _on_change_cb   = nullptr;
    _on_change_user = nullptr;
}

// =============================================================================
// SetMaterialSlotCount — model load 時に呼ばれる、Array resize + slot_index 設定
// =============================================================================
void ModelMaterialPanel::SetMaterialSlotCount(u32 count) noexcept {
    // 上限 clamp: 不正な model でも UI が無限長 list を作らないように。
    if (count > kMaxSlots) {
        count = kMaxSlots;
    }

    // Array::Resize は新規領域を MemSet 0 (trivially constructible 経路) するが、
    // MaterialOverride の default ctor 値 (base_color = 1,1,1,1 等) は 0 初期化
    // では再現できない。よって、resize 後に明示的に default 値を書き直す必要が
    // ある。コストは slot 数線形だが、SetMaterialSlotCount は model load 時の
    // 単発呼び出しなので問題なし。
    _overrides.Resize(static_cast<usize>(count));
    for (u32 i = 0; i < count; ++i) {
        MaterialOverride& o = _overrides[static_cast<usize>(i)];
        ResetOverrideToDefault(o);
        // slot_index は ResetOverrideToDefault では触らない (= 既存仕様の通り)
        // ので、ここで明示的に割り当てる。
        o.slot_index = i;
    }
    // 既存 callback はそのまま (= model 切替時に外部が「全 slot 0..count-1 の
    // override が初期値」と認識できれば足りる)。callback は ResetAll とは違って
    // ここでは発火しない (= model load 自体が大イベントで、外部側 (ModelViewer
    // 本体) が一括で shader CB を初期化する想定)。
}

// =============================================================================
// ResetSlot / ResetAll
// =============================================================================
void ModelMaterialPanel::ResetSlot(u32 slot_index) noexcept {
    const u32 count = static_cast<u32>(_overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        // 範囲外は no-op (= 仕様)。
        return;
    }
    MaterialOverride& o = _overrides[static_cast<usize>(slot_index)];
    ResetOverrideToDefault(o);
    // slot_index フィールドは保守的に再設定 (= 万一外部が壊していても回復)。
    o.slot_index = slot_index;
    // 外部 (UndoStack / shader CB) に「default に戻した」イベントを通知する。
    FireChangeCallback(slot_index);
}

void ModelMaterialPanel::ResetAll() noexcept {
    const u32 count = static_cast<u32>(_overrides.Size());
    for (u32 i = 0; i < count; ++i) {
        // ResetSlot を直接呼ぶと callback が slot 数だけ発火する仕様
        // (= ヘッダコメント参照)。1 件ずつ undo できる粒度を保つため意図的。
        ResetSlot(i);
    }
}

// =============================================================================
// アクセサ群
// =============================================================================
bool ModelMaterialPanel::IsSlotOverridden(u32 slot_index) const noexcept {
    const u32 count = static_cast<u32>(_overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return false;
    }
    return _overrides[static_cast<usize>(slot_index)].is_overridden;
}

const MaterialOverride* ModelMaterialPanel::GetOverride(u32 slot_index) const noexcept {
    const u32 count = static_cast<u32>(_overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return nullptr;
    }
    return &_overrides[static_cast<usize>(slot_index)];
}

MaterialOverride* ModelMaterialPanel::GetOverrideMutable(u32 slot_index) noexcept {
    const u32 count = static_cast<u32>(_overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return nullptr;
    }
    return &_overrides[static_cast<usize>(slot_index)];
}

u32 ModelMaterialPanel::SlotCount() const noexcept {
    return static_cast<u32>(_overrides.Size());
}

void ModelMaterialPanel::SetOnMaterialChangeCallback(MaterialChangeCallback cb,
                                                     void* user) noexcept {
    _on_change_cb   = cb;
    _on_change_user = user;
}

// =============================================================================
// 内部ヘルパ: callback 発火
// =============================================================================
void ModelMaterialPanel::FireChangeCallback(u32 slot_index) noexcept {
    if (_on_change_cb == nullptr) return;
    const u32 count = static_cast<u32>(_overrides.Size());
    if (!IsValidSlot(slot_index, count)) return;
    _on_change_cb(_on_change_user,
                  slot_index,
                  _overrides[static_cast<usize>(slot_index)]);
}

// =============================================================================
// DrawUI — メイン ImGui window
// =============================================================================
// レイアウト (ヘッダ参照):
//   ┌─ "Material Override" window
//   │ Slot count: N
//   │ [Reset All]
//   │ CollapsingHeader "Slot 0"
//   │   [x] Override enabled
//   │   Base Color   [ColorEdit4]
//   │   Metallic     [SliderFloat]
//   │   ...
//   │   [Reset]
//   │ CollapsingHeader "Slot 1"
//   │   ...
// =============================================================================
void ModelMaterialPanel::DrawUI() noexcept {
    // EditorPanel 規約: !IsVisible() なら早期 return (描画スキップ)。
    if (!IsVisible()) {
        return;
    }

    // ImGui::Begin の戻り値が false でも End は必須 (= ImGui 規約)。
    // `&_visible` を渡すことで close ボタンが基底の _visible を直接 toggle。
    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    const u32 count = static_cast<u32>(_overrides.Size());

    // ----- ヘッダ (slot 数 + Reset All) -----
    ImGui::Text("Slot count: %u", static_cast<unsigned>(count));
    ImGui::SameLine();
    // Reset All は危険操作 (全 slot のユーザー編集を一掃) なので、ホバーで
    // 注意を促すヒント。
    if (ImGui::SmallButton("Reset All")) {
        ResetAll();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reset every slot to default (clears all overrides).");
    }
    ImGui::Separator();

    if (count == 0) {
        // モデル未 load の状態を視認できるように disabled 文言を出す
        // (= 「壊れてる」と誤解されない UX)。
        ImGui::TextDisabled("(No material slots — load a model first)");
        ImGui::End();
        return;
    }

    // ----- 各 slot の編集 UI -----
    for (u32 i = 0; i < count; ++i) {
        MaterialOverride& o = _overrides[static_cast<usize>(i)];

        // 各 slot を独立 ID namespace に: 同名 field が複数 slot で並んでも
        // ImGui の widget ID が衝突しないように PushID(i) で囲む。
        ImGui::PushID(static_cast<int>(i));

        // slot ラベル整形 (= "Slot 0" 等)。将来アセット由来の material 名に差替予定。
        char slot_label[64] = {};
        FormatSlotLabel(slot_label, sizeof(slot_label), i);

        // override 中の slot は CollapsingHeader を default open にすることで、
        // ユーザーが触った slot をすぐ確認できる UX。
        ImGuiTreeNodeFlags header_flags = 0;
        if (o.is_overridden) {
            header_flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        if (ImGui::CollapsingHeader(slot_label, header_flags)) {
            bool changed = false;

            // override toggle: 直接 is_overridden を ImGui::Checkbox で出す
            // (= 明示的に override を on/off する高速経路)。
            // ColorEdit / SliderFloat の編集時には自動で is_overridden = true に
            // セットするが、ユーザー側で「override を解除して元の material に戻したい」
            // 場合の明示操作としても残す。
            if (ImGui::Checkbox("Override enabled", &o.is_overridden)) {
                changed = true;
            }

            // ----- Base Color (RGBA) -----
            // ColorEdit4 は f32[4] を直接読み書きする。Vec4 は alignas(16)、
            // 内部 f32 x,y,z,w が連続レイアウト (alignas は配置のみ、要素間 pad なし)
            // なので &o.base_color.x を直接渡せる。
            if (ImGui::ColorEdit4("Base Color", &o.base_color.x)) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Metallic -----
            if (ImGui::SliderFloat("Metallic",
                                   &o.metallic,
                                   kMinMetallic,
                                   kMaxMetallic,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Roughness -----
            if (ImGui::SliderFloat("Roughness",
                                   &o.roughness,
                                   kMinRoughness,
                                   kMaxRoughness,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Normal Strength -----
            if (ImGui::SliderFloat("Normal Strength",
                                   &o.normal_strength,
                                   kMinNormalStrength,
                                   kMaxNormalStrength,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- AO Strength -----
            if (ImGui::SliderFloat("AO Strength",
                                   &o.ao_strength,
                                   kMinAoStrength,
                                   kMaxAoStrength,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Emissive (RGB) -----
            // ColorEdit3 は f32[3] を期待。Vec3 は alignas(16) + 末尾 _pad を持つ
            // が、x/y/z は連続 f32 なので &o.emissive.x を 3 要素として安全に渡せる
            // (InspectorPanel と同パターン)。
            if (ImGui::ColorEdit3("Emissive", &o.emissive.x)) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Emissive Intensity -----
            if (ImGui::SliderFloat("Emissive Intensity",
                                   &o.emissive_intensity,
                                   kMinEmissiveIntensity,
                                   kMaxEmissiveIntensity,
                                   "%.2f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // ----- Reset (per-slot) -----
            // 個別 slot だけを default に戻す。ResetSlot 内で callback も発火する
            // ので、ここでは追加の changed = true は不要 (= 二重発火回避)。
            if (ImGui::Button("Reset")) {
                ResetSlot(i);
                // ResetSlot 内で callback 済 → 下の changed 分岐をスキップ。
                changed = false;
            }

            // 何らかの field が編集されたら callback を 1 回発火。
            // (Reset ボタン経由は ResetSlot 内で発火済なので、ここには来ない。)
            if (changed) {
                FireChangeCallback(i);
            }
        }

        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace acs::game::modelview
