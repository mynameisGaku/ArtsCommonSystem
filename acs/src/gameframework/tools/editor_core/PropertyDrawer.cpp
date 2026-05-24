// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / PropertyDrawer 実装 (Phase 21a)
//
// 仕様の意図は PropertyDrawer.h を参照。本ファイルでは:
//   ・per-byte StrEq による const char* 比較ループ (Settings / Entitlement と同形)
//   ・Init で bundled drawer 9 種 ("F32Slider" / "Vec2Drag" / "Vec3Drag" /
//     "Vec4Drag" / "ColorRGB" / "ColorRGBA" / "AssetPath" / "EnumCombo" /
//     "TextInput") を自動登録
//   ・各 bundled drawer の ImGui 実装
//   ・Register/Unregister/HasDrawer/DrawProperty/DrawerCount/DrawerName/ClearAll
//     の単純実装
// を提供する。すべて noexcept、STL 不使用、ImGui 依存は本 .cpp に閉じる。
#include "gameframework/tools/editor_core/PropertyDrawer.h"

#include <imgui.h>

#include <cstring>  // std::strncpy (TextInput / AssetPath 編集バッファ用)

namespace acs::game::editor_core {

// =============================================================================
// 内部ヘルパ
// =============================================================================
namespace {

// const char* の per-byte 比較。nullptr 安全。
// Settings.cpp / Entitlement.cpp と同形 (= ACS 内 StrEq pattern)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// label が nullptr / 空文字なら "##unnamed" を返す。ImGui は label に nullptr を
// 渡せないため (= internal SetID で deref する箇所がある) 安全側に倒す。
const char* SafeLabel(const char* label) noexcept {
    if (label == nullptr || label[0] == '\0') {
        return "##unnamed";
    }
    return label;
}

// hover tooltip を出す共通処理。tooltip == nullptr なら no-op。
void DrawTooltip(const char* tooltip) noexcept {
    if (tooltip != nullptr && tooltip[0] != '\0' && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

// =============================================================================
// Bundled drawer 群
// =============================================================================
// すべて DrawerFn シグネチャ (= `void (const PropertyContext&) noexcept`) で
// 統一。data_ptr の型は drawer 名で暗黙に決まる:
//   "F32Slider"  → f32*
//   "Vec2Drag"   → f32[2] (= acs::Vec2 連続 x/y を想定)
//   "Vec3Drag"   → f32[3] (= acs::Vec3 連続 x/y/z、ただし Vec3 は alignas(16) で
//                          padding を含むため呼び出し側で 3 要素にコピーして渡す
//                          パターン。本 drawer は f32* を素直に 3 要素読む)
//   "Vec4Drag"   → f32[4]
//   "ColorRGB"   → f32[3] (RGB 0..1)
//   "ColorRGBA"  → f32[4] (RGBA 0..1)
//   "AssetPath"  → char[]  (kTextInputBufferSize 長を想定、null 終端)
//   "EnumCombo"  → i32* (= int*; 選択中 index)
//   "TextInput"  → char[]  (kTextInputBufferSize 長を想定、null 終端)
// =============================================================================

// F32Slider: ctx.min_value / ctx.max_value で SliderFloat。
void Drawer_F32Slider(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::SliderFloat(SafeLabel(ctx.label),
                                            p,
                                            ctx.min_value,
                                            ctx.max_value,
                                            "%.3f");
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// Vec2Drag: DragFloat2 (speed=0.1)。
void Drawer_Vec2Drag(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat2(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// Vec3Drag: DragFloat3 (speed=0.1)。
void Drawer_Vec3Drag(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat3(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// Vec4Drag: DragFloat4 (speed=0.1)。
void Drawer_Vec4Drag(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat4(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// ColorRGB: ColorEdit3 (0..1)。
void Drawer_ColorRGB(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::ColorEdit3(SafeLabel(ctx.label), p);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// ColorRGBA: ColorEdit4 (0..1)。
void Drawer_ColorRGBA(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::ColorEdit4(SafeLabel(ctx.label), p);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// AssetPath: char[] バッファに対する InputText + DragDrop 受け口。
//   ・data_ptr は null 終端 char[]、容量は kTextInputBufferSize と想定。
//   ・ImGui 側で path を直接編集可能 (= テキスト入力)。
//   ・別 panel (AssetBrowser) から "ASSET_PATH" payload で drag-drop されると
//     バッファに strncpy で書き戻す。
void Drawer_AssetPath(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    char* buf = static_cast<char*>(ctx.data_ptr);
    // InputText は size 引数が必須。本 drawer は ctx.data_ptr が
    // PropertyDrawerRegistry::kTextInputBufferSize 長を持つ前提で固定値を渡す。
    const bool changed = ImGui::InputText(SafeLabel(ctx.label),
                                          buf,
                                          PropertyDrawerRegistry::kTextInputBufferSize);
    DrawTooltip(ctx.tooltip);

    // Drag-drop 受け口: AssetBrowser 側が "ASSET_PATH" payload (= 文字列バイト列)
    // を SetDragDropPayload した場合、その文字列をバッファに書き戻す。
    // payload size は null 終端を含むかは sender 側次第。安全のため strncpy で
    // 末尾 null を保証する。
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(PropertyDrawerRegistry::kAssetPathPayloadId);
        if (payload != nullptr && payload->Data != nullptr && payload->DataSize > 0) {
            const char* src = static_cast<const char*>(payload->Data);
            // strncpy で書き戻し + 末尾 null 確実化。
            std::strncpy(buf, src, PropertyDrawerRegistry::kTextInputBufferSize - 1);
            buf[PropertyDrawerRegistry::kTextInputBufferSize - 1] = '\0';
            dropped = true;
        }
        ImGui::EndDragDropTarget();
    }

    if ((changed || dropped) && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// EnumCombo: ImGui::Combo (enum_values は "Item0\0Item1\0...\0" 形式 OR
// const char* const items[]; 形式の両方を支援。ImGui::Combo の overload は
// `const char* items_separated_by_zeros` を取るものと `const char* const items[]`
// を取るものがある。本 drawer は呼び出し側の自由度を上げるため、
//   ・ctx.enum_count > 0  → "Item0\0Item1\0..." と解釈せず、items_separated_by_zeros
//     形式の文字列をそのまま渡す + items_count を ctx.enum_count に合わせる
//   ・ImGui::Combo(label, int*, items_separated_by_zeros, popup_max_height_in_items=-1)
//     を使う方が API として素直 (ImGui 側で '\0' 終端を数える)
// → 結論: items_separated_by_zeros の 3 引数 overload を使う (= enum_count 不要)
//   が、互換性のため `popup_max_height_in_items=ctx.enum_count` を渡せるよう
//   4 引数 overload を選ぶ。enum_count が 0 なら -1 (= ImGui 既定)。
void Drawer_EnumCombo(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr || ctx.enum_values == nullptr) return;
    int* p = static_cast<int*>(ctx.data_ptr);
    // 4 引数 overload: 末尾 popup_max_height_in_items は項目数のヒント。0 → -1。
    const int popup_max = (ctx.enum_count > 0)
                              ? static_cast<int>(ctx.enum_count)
                              : -1;
    const bool changed = ImGui::Combo(SafeLabel(ctx.label),
                                      p,
                                      ctx.enum_values,
                                      popup_max);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

// TextInput: char[] バッファに対する read-write InputText。
//   ・data_ptr は null 終端 char[]、容量は kTextInputBufferSize と想定。
//   ・ImGui 側で文字列を直接編集 (read-only にしたい場合は将来 ctx に flag 追加)。
void Drawer_TextInput(const PropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    char* buf = static_cast<char*>(ctx.data_ptr);
    const bool changed = ImGui::InputText(SafeLabel(ctx.label),
                                          buf,
                                          PropertyDrawerRegistry::kTextInputBufferSize);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

} // namespace

// =============================================================================
// Init / Shutdown / ClearAll
// =============================================================================

void PropertyDrawerRegistry::Init() noexcept {
    // 既存登録を全て破棄 (多重 Init を許容)。
    _entries.Clear();

    // bundled drawer 9 種を順に自動登録。
    //   ・名前順ではなく「カテゴリ順 (scalar → vector → color → asset → enum → text)」
    //     で並べておく (DrawerName(index) の出力デバッグ視認性のため)。
    //   ・name はリテラル文字列なので、ポインタを保存するだけで安全。
    RegisterDrawer("F32Slider",  &Drawer_F32Slider);
    RegisterDrawer("Vec2Drag",   &Drawer_Vec2Drag);
    RegisterDrawer("Vec3Drag",   &Drawer_Vec3Drag);
    RegisterDrawer("Vec4Drag",   &Drawer_Vec4Drag);
    RegisterDrawer("ColorRGB",   &Drawer_ColorRGB);
    RegisterDrawer("ColorRGBA",  &Drawer_ColorRGBA);
    RegisterDrawer("AssetPath",  &Drawer_AssetPath);
    RegisterDrawer("EnumCombo",  &Drawer_EnumCombo);
    RegisterDrawer("TextInput",  &Drawer_TextInput);
}

void PropertyDrawerRegistry::Shutdown() noexcept {
    // 全 drawer 登録を破棄。Init での bundled 再注入は呼び出し側で `Init()` を
    // 呼び直す責務 (= Shutdown は state を空に倒すだけ)。
    _entries.Clear();
}

void PropertyDrawerRegistry::ClearAll() noexcept {
    // Shutdown と同義 (= ImGui 等のグローバル状態は触らないため等価)。
    // 別名 API として残しているのは Init/Shutdown/ClearAll の対称性を取るため。
    _entries.Clear();
}

// =============================================================================
// FindIndex (内部線形探索)
// =============================================================================

isize PropertyDrawerRegistry::FindIndex(const char* type_name) const noexcept {
    if (type_name == nullptr || type_name[0] == '\0') return -1;
    const usize n = _entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_entries[i].name, type_name)) {
            return static_cast<isize>(i);
        }
    }
    return -1;
}

// =============================================================================
// Register / Unregister / Has / Draw
// =============================================================================

void PropertyDrawerRegistry::RegisterDrawer(const char* type_name, DrawerFn fn) noexcept {
    // null / 空文字 / null fn は全部 no-op (= 呼び出し側ミスを silent に弾く)。
    if (type_name == nullptr || type_name[0] == '\0' || fn == nullptr) return;

    // 同 name が既に登録済みなら **後勝ち** で fn を置き換える。
    const isize idx = FindIndex(type_name);
    if (idx >= 0) {
        _entries[static_cast<usize>(idx)].fn = fn;
        // name は同じ文字列リテラルが来る前提だが、念のため上書きしておく
        // (= 違うアドレスの同内容文字列が来た場合に古いポインタを保持しないため)。
        _entries[static_cast<usize>(idx)].name = type_name;
        return;
    }

    // 新規登録: 末尾に追加。
    Entry e;
    e.name = type_name;
    e.fn   = fn;
    _entries.PushBack(e);
}

void PropertyDrawerRegistry::UnregisterDrawer(const char* type_name) noexcept {
    const isize idx = FindIndex(type_name);
    if (idx < 0) return;
    // 末尾 swap で O(1) 削除 (順序非保持)。RegisterDrawer で再登録すれば
    // 末尾に戻るので影響は少ない。
    _entries.RemoveAtSwap(static_cast<usize>(idx));
}

bool PropertyDrawerRegistry::HasDrawer(const char* type_name) const noexcept {
    return FindIndex(type_name) >= 0;
}

bool PropertyDrawerRegistry::DrawProperty(const char* type_name,
                                          const PropertyContext& ctx) const noexcept {
    const isize idx = FindIndex(type_name);
    if (idx < 0) return false;

    DrawerFn fn = _entries[static_cast<usize>(idx)].fn;
    if (fn == nullptr) {
        // 防御的: Register 時に null fn は弾いているはずだが、念のため。
        return false;
    }
    fn(ctx);
    return true;
}

// =============================================================================
// イントロスペクション (DrawerCount / DrawerName)
// =============================================================================

u32 PropertyDrawerRegistry::DrawerCount() const noexcept {
    return static_cast<u32>(_entries.Size());
}

const char* PropertyDrawerRegistry::DrawerName(u32 index) const noexcept {
    if (static_cast<usize>(index) >= _entries.Size()) return nullptr;
    return _entries[static_cast<usize>(index)].name;
}

} // namespace acs::game::editor_core
