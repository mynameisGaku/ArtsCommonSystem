// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / AModelMaterialPanel 実装
//
// 仕様の意図は AModelMaterialPanel.h を参照。本ファイルでは:
//   ・slot list (TArray<MaterialOverride>) の resize / reset / accessor
//   ・ImGui (CollapsingHeader + ColorEdit / SliderFloat / Button) によるレンダ
//   ・field 変更検知時の callback 発火
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/modelview/ModelMaterialPanel.h"

#include <imgui.h>

#include <cstdio>  // std::snprintf (slot label 整形)

namespace acs::game::modelview {

/**
 * slot 1 件の表示ラベル "Slot NN" を整形する。
 *
 * @details 将来 SetSlotName API が入ったら、ここでアセット由来の material 名を返すように
 * 差し替える予定。
 * @param buf 書き込み先バッファ (nullptr は no-op)。
 * @param buf_size buf の容量 (0 は no-op)。
 * @param slot_index 整形する slot index。
 */
static void FormatSlotLabel(char* buf, usize buf_size, u32 slot_index) noexcept {
    if (buf == nullptr || buf_size == 0) return;
    std::snprintf(buf, buf_size, "Slot %u", static_cast<unsigned>(slot_index));
}

/**
 * slot index の境界チェックを行う。
 *
 * @param slot_index 検査する slot index。
 * @param count slot 総数。
 * @return slot_index < count なら true。
 */
static inline bool IsValidSlot(u32 slot_index, u32 count) noexcept {
    return slot_index < count;
}

/**
 * MaterialOverride を物理的に neutral な default 値に戻す。
 *
 * @details
 * 「Reset = is_overridden=false かつ各値を default に戻す」という規約をここに集約する
 * (仕様変更時の修正箇所を 1 つに)。slot_index は触らず呼び出し側で再設定する。
 * @param o reset 対象の MaterialOverride。
 */
static inline void ResetOverrideToDefault(FMaterialOverride& o) noexcept {
    // slot_index は呼び出し側責務 (= Reset 後も slot_index は保持したいため
    // ここでは触らない)。
    o.base_color        = FVec4{1.0f, 1.0f, 1.0f, 1.0f};
    o.metallic          = 0.0f;
    o.roughness         = 0.5f;
    o.normal_strength   = 1.0f;
    o.ao_strength       = 1.0f;
    o.emissive          = FVec3{0.0f, 0.0f, 0.0f};
    o.emissive_intensity = 1.0f;
    o.is_overridden     = false;
}

/** slot list を空に戻す (callback は維持。状態リセット責務)。 */
void AModelMaterialPanel::Init() noexcept {
    // slot list を空に戻す (= 多重 Init 時の状態リセット)。
    // 容量は破棄せず再利用 (= 再 Init / model 再 load のアロケーション節約)。
    m_Overrides.Clear();
    // callback は維持 (= Init は state リセット責務、callback 解除は Shutdown 責務)。
}

/** slot list を空にし callback も解除する (全状態リセット)。 */
void AModelMaterialPanel::Shutdown() noexcept {
    // TArray はデストラクタで自然解放されるが、明示 Clear することで
    // 「多重 Shutdown 後の状態」を確定させる。
    m_Overrides.Clear();
    m_OnChangeCb   = nullptr;
    m_OnChangeUser = nullptr;
}

/** slot list を count 個 (kMaxSlots clamp) に resize し default 値 + slot_index を書く。 */
void AModelMaterialPanel::SetMaterialSlotCount(u32 count) noexcept {
    // 上限 clamp: 不正な model でも UI が無限長 list を作らないように。
    if (count > kMaxSlots) {
        count = kMaxSlots;
    }

    // TArray::Resize は新規領域を MemSet 0 (trivially constructible 経路) するが、
    // MaterialOverride の default ctor 値 (base_color = 1,1,1,1 等) は 0 初期化
    // では再現できない。よって、resize 後に明示的に default 値を書き直す必要が
    // ある。コストは slot 数線形だが、SetMaterialSlotCount は model load 時の
    // 単発呼び出しなので問題なし。
    m_Overrides.Resize(static_cast<usize>(count));
    for (u32 i = 0; i < count; ++i) {
        FMaterialOverride& o = m_Overrides[static_cast<usize>(i)];
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

/** 1 slot を default に戻し callback を発火する (範囲外は no-op)。 */
void AModelMaterialPanel::ResetSlot(u32 slot_index) noexcept {
    const u32 count = static_cast<u32>(m_Overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        // 範囲外は no-op (= 仕様)。
        return;
    }
    FMaterialOverride& o = m_Overrides[static_cast<usize>(slot_index)];
    ResetOverrideToDefault(o);
    // slot_index フィールドは保守的に再設定 (= 万一外部が壊していても回復)。
    o.slot_index = slot_index;
    // 外部 (CUndoStack / shader CB) に「default に戻した」イベントを通知する。
    FireChangeCallback(slot_index);
}

/** 全 slot を ResetSlot で 1 件ずつ default に戻す (callback も slot 数だけ発火)。 */
void AModelMaterialPanel::ResetAll() noexcept {
    const u32 count = static_cast<u32>(m_Overrides.Size());
    for (u32 i = 0; i < count; ++i) {
        // ResetSlot を直接呼ぶと callback が slot 数だけ発火する仕様
        // (= ヘッダコメント参照)。1 件ずつ undo できる粒度を保つため意図的。
        ResetSlot(i);
    }
}

/** 指定 slot が override 中かを返す (範囲外は false)。 */
bool AModelMaterialPanel::IsSlotOverridden(u32 slot_index) const noexcept {
    const u32 count = static_cast<u32>(m_Overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return false;
    }
    return m_Overrides[static_cast<usize>(slot_index)].is_overridden;
}

/** 指定 slot の override を const ポインタで返す (範囲外は nullptr)。 */
const FMaterialOverride* AModelMaterialPanel::GetOverride(u32 slot_index) const noexcept {
    const u32 count = static_cast<u32>(m_Overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return nullptr;
    }
    return &m_Overrides[static_cast<usize>(slot_index)];
}

/** 指定 slot の override を可変ポインタで返す (範囲外は nullptr)。 */
FMaterialOverride* AModelMaterialPanel::GetOverrideMutable(u32 slot_index) noexcept {
    const u32 count = static_cast<u32>(m_Overrides.Size());
    if (!IsValidSlot(slot_index, count)) {
        return nullptr;
    }
    return &m_Overrides[static_cast<usize>(slot_index)];
}

/** 現在の slot 数を返す。 */
u32 AModelMaterialPanel::SlotCount() const noexcept {
    return static_cast<u32>(m_Overrides.Size());
}

/** material 変更通知 callback と user ポインタを設定する。 */
void AModelMaterialPanel::SetOnMaterialChangeCallback(MaterialChangeCallback cb,
                                                     void* user) noexcept {
    m_OnChangeCb   = cb;
    m_OnChangeUser = user;
}

/** 指定 slot の現在値で変更 callback を発火する (callback 未設定 / 範囲外は no-op)。 */
void AModelMaterialPanel::FireChangeCallback(u32 slot_index) noexcept {
    if (m_OnChangeCb == nullptr) return;
    const u32 count = static_cast<u32>(m_Overrides.Size());
    if (!IsValidSlot(slot_index, count)) return;
    m_OnChangeCb(m_OnChangeUser,
                  slot_index,
                  m_Overrides[static_cast<usize>(slot_index)]);
}

/** ImGui window (slot 数 + Reset All + 各 slot の override 編集 UI) を描画する。 */
void AModelMaterialPanel::DrawUI() noexcept {
    // AEditorPanel 規約: !IsVisible() なら早期 return (描画スキップ)。
    if (!IsVisible()) {
        return;
    }

    // ImGui::Begin の戻り値が false でも End は必須 (= ImGui 規約)。
    // `&m_Visible` を渡すことで close ボタンが基底の m_Visible を直接 toggle。
    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    const u32 count = static_cast<u32>(m_Overrides.Size());

    // ヘッダ (slot 数 + Reset All)。
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

    // 各 slot の編集 UI。
    for (u32 i = 0; i < count; ++i) {
        FMaterialOverride& o = m_Overrides[static_cast<usize>(i)];

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

            // Base Color (RGBA) を編集する。
            // ColorEdit4 は f32[4] を直接読み書きする。FVec4 は alignas(16)、
            // 内部 f32 x,y,z,w が連続レイアウト (alignas は配置のみ、要素間 pad なし)
            // なので &o.base_color.x を直接渡せる。
            if (ImGui::ColorEdit4("Base Color", &o.base_color.x)) {
                o.is_overridden = true;
                changed         = true;
            }

            // Metallic。
            if (ImGui::SliderFloat("Metallic",
                                   &o.metallic,
                                   kMinMetallic,
                                   kMaxMetallic,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // Roughness。
            if (ImGui::SliderFloat("Roughness",
                                   &o.roughness,
                                   kMinRoughness,
                                   kMaxRoughness,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // Normal Strength。
            if (ImGui::SliderFloat("Normal Strength",
                                   &o.normal_strength,
                                   kMinNormalStrength,
                                   kMaxNormalStrength,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // AO Strength。
            if (ImGui::SliderFloat("AO Strength",
                                   &o.ao_strength,
                                   kMinAoStrength,
                                   kMaxAoStrength,
                                   "%.3f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // Emissive (RGB)。
            // ColorEdit3 は f32[3] を期待。FVec3 は alignas(16) + 末尾 m_Pad を持つ
            // が、x/y/z は連続 f32 なので &o.emissive.x を 3 要素として安全に渡せる
            // (AInspectorPanel と同パターン)。
            if (ImGui::ColorEdit3("Emissive", &o.emissive.x)) {
                o.is_overridden = true;
                changed         = true;
            }

            // Emissive Intensity。
            if (ImGui::SliderFloat("Emissive Intensity",
                                   &o.emissive_intensity,
                                   kMinEmissiveIntensity,
                                   kMaxEmissiveIntensity,
                                   "%.2f")) {
                o.is_overridden = true;
                changed         = true;
            }

            // Reset (per-slot)。
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
