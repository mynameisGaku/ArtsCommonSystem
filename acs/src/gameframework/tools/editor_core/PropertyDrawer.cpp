// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FPropertyDrawer 実装
//
// 仕様の意図は FPropertyDrawer.h を参照。本ファイルでは:
//   ・per-byte StrEq による const char* 比較ループ (FSettings / FEntitlement と同形)
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

namespace {

/**
 * const char* の per-byte 比較で 2 文字列が一致するかを返す。
 *
 * @details nullptr 安全 (一方でも nullptr なら不一致)。FSettings / FEntitlement と同形。
 * @param a 比較する文字列その 1。
 * @param b 比較する文字列その 2。
 * @return 両者が非 null で内容一致なら true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * label を ImGui へ安全に渡せる文字列へ正規化する。
 *
 * @details
 * label が nullptr / 空文字なら "##unnamed" を返す。ImGui は label に nullptr を
 * 渡せない (internal SetID で deref する箇所がある) ため安全側に倒す。
 * @param label 正規化する元ラベル。
 * @return 非 null の表示ラベル。
 */
const char* SafeLabel(const char* label) noexcept {
    if (label == nullptr || label[0] == '\0') {
        return "##unnamed";
    }
    return label;
}

/**
 * 直前の item に hover tooltip を出す共通処理。
 *
 * @details tooltip が nullptr / 空文字、または item が hover されていなければ no-op。
 * @param tooltip 表示する tooltip 文字列。
 */
void DrawTooltip(const char* tooltip) noexcept {
    if (tooltip != nullptr && tooltip[0] != '\0' && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

/**
 * F32Slider drawer: ctx.min_value / ctx.max_value 範囲の SliderFloat を描画する。
 *
 * @details data_ptr は f32* を想定する。
 * @param ctx 描画パラメータ (data_ptr / label / tooltip / min/max / out_changed)。
 */
void Drawer_F32Slider(const FPropertyContext& ctx) noexcept {
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

/**
 * Vec2Drag drawer: DragFloat2 (speed=0.1) を描画する。
 *
 * @details data_ptr は連続 x/y を持つ f32[2] を想定する。
 * @param ctx 描画パラメータ。
 */
void Drawer_Vec2Drag(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat2(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * Vec3Drag drawer: DragFloat3 (speed=0.1) を描画する。
 *
 * @details
 * data_ptr は連続 x/y/z を持つ f32[3] を想定する (FVec3 は alignas(16) で padding を
 * 含むため、呼び出し側で 3 要素にコピーして渡すパターン)。
 * @param ctx 描画パラメータ。
 */
void Drawer_Vec3Drag(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat3(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * Vec4Drag drawer: DragFloat4 (speed=0.1) を描画する。
 *
 * @details data_ptr は連続 4 要素を持つ f32[4] を想定する。
 * @param ctx 描画パラメータ。
 */
void Drawer_Vec4Drag(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::DragFloat4(SafeLabel(ctx.label), p, 0.1f);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * ColorRGB drawer: ColorEdit3 (0..1) を描画する。
 *
 * @details data_ptr は RGB 各成分 0..1 の f32[3] を想定する。
 * @param ctx 描画パラメータ。
 */
void Drawer_ColorRGB(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::ColorEdit3(SafeLabel(ctx.label), p);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * ColorRGBA drawer: ColorEdit4 (0..1) を描画する。
 *
 * @details data_ptr は RGBA 各成分 0..1 の f32[4] を想定する。
 * @param ctx 描画パラメータ。
 */
void Drawer_ColorRGBA(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    f32* p = static_cast<f32*>(ctx.data_ptr);
    const bool changed = ImGui::ColorEdit4(SafeLabel(ctx.label), p);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * AssetPath drawer: char[] バッファに対する InputText + DragDrop 受け口を描画する。
 *
 * @details
 * data_ptr は null 終端 char[] (容量 kTextInputBufferSize) を想定。ImGui 側で path を
 * 直接編集できるほか、別 panel (CAssetBrowser) から "ASSET_PATH" payload で drag-drop
 * されるとバッファに strncpy で書き戻す。
 * @param ctx 描画パラメータ。
 */
void Drawer_AssetPath(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    char* buf = static_cast<char*>(ctx.data_ptr);
    // InputText は size 引数が必須。本 drawer は ctx.data_ptr が
    // PropertyDrawerRegistry::kTextInputBufferSize 長を持つ前提で固定値を渡す。
    const bool changed = ImGui::InputText(SafeLabel(ctx.label),
                                          buf,
                                          CPropertyDrawerRegistry::kTextInputBufferSize);
    DrawTooltip(ctx.tooltip);

    // Drag-drop 受け口: CAssetBrowser 側が "ASSET_PATH" payload (= 文字列バイト列)
    // を SetDragDropPayload した場合、その文字列をバッファに書き戻す。
    // payload size は null 終端を含むかは sender 側次第。安全のため strncpy で
    // 末尾 null を保証する。
    bool dropped = false;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(CPropertyDrawerRegistry::kAssetPathPayloadId);
        if (payload != nullptr && payload->Data != nullptr && payload->DataSize > 0) {
            const char* src = static_cast<const char*>(payload->Data);
            // strncpy で書き戻し + 末尾 null 確実化。
            std::strncpy(buf, src, CPropertyDrawerRegistry::kTextInputBufferSize - 1);
            buf[CPropertyDrawerRegistry::kTextInputBufferSize - 1] = '\0';
            dropped = true;
        }
        ImGui::EndDragDropTarget();
    }

    if ((changed || dropped) && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

/**
 * EnumCombo drawer: ImGui::Combo を描画する。
 *
 * @details
 * data_ptr は選択中 index を持つ int* を想定し、ctx.enum_values は
 * "Item0\0Item1\0...\0" 形式の items_separated_by_zeros 文字列。ctx.enum_count を
 * popup_max_height_in_items として渡す (0 なら ImGui 既定の -1)。data_ptr または
 * enum_values が nullptr なら no-op。
 * @param ctx 描画パラメータ (data_ptr / enum_values / enum_count を使う)。
 */
void Drawer_EnumCombo(const FPropertyContext& ctx) noexcept {
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

/**
 * TextInput drawer: char[] バッファに対する read-write InputText を描画する。
 *
 * @details data_ptr は null 終端 char[] (容量 kTextInputBufferSize) を想定する。
 * @param ctx 描画パラメータ。
 */
void Drawer_TextInput(const FPropertyContext& ctx) noexcept {
    if (ctx.data_ptr == nullptr) return;
    char* buf = static_cast<char*>(ctx.data_ptr);
    const bool changed = ImGui::InputText(SafeLabel(ctx.label),
                                          buf,
                                          CPropertyDrawerRegistry::kTextInputBufferSize);
    DrawTooltip(ctx.tooltip);
    if (changed && ctx.out_changed != nullptr) {
        *ctx.out_changed = true;
    }
}

} // namespace

/** 既存登録を破棄して bundled drawer 9 種を自動登録する。 */
void CPropertyDrawerRegistry::Init() noexcept {
    // 既存登録を全て破棄 (多重 Init を許容)。
    m_Entries.Clear();

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

/** 全 drawer 登録を破棄する。 */
void CPropertyDrawerRegistry::Shutdown() noexcept {
    // 全 drawer 登録を破棄。Init での bundled 再注入は呼び出し側で `Init()` を
    // 呼び直す責務 (= Shutdown は state を空に倒すだけ)。
    m_Entries.Clear();
}

/** 全 drawer 登録を破棄して空に戻す (Shutdown と同義)。 */
void CPropertyDrawerRegistry::ClearAll() noexcept {
    // Shutdown と同義 (= ImGui 等のグローバル状態は触らないため等価)。
    // 別名 API として残しているのは Init/Shutdown/ClearAll の対称性を取るため。
    m_Entries.Clear();
}

/** name 一致のエントリを線形探索する (未ヒットは -1)。 */
isize CPropertyDrawerRegistry::FindIndex(const char* type_name) const noexcept {
    if (type_name == nullptr || type_name[0] == '\0') return -1;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Entries[i].name, type_name)) {
            return static_cast<isize>(i);
        }
    }
    return -1;
}

/** type_name + DrawerFn を後勝ちで登録 / 上書きする (不正引数は no-op)。 */
void CPropertyDrawerRegistry::RegisterDrawer(const char* type_name, DrawerFn fn) noexcept {
    // null / 空文字 / null fn は全部 no-op (= 呼び出し側ミスを silent に弾く)。
    if (type_name == nullptr || type_name[0] == '\0' || fn == nullptr) return;

    // 同 name が既に登録済みなら **後勝ち** で fn を置き換える。
    const isize idx = FindIndex(type_name);
    if (idx >= 0) {
        m_Entries[static_cast<usize>(idx)].fn = fn;
        // name は同じ文字列リテラルが来る前提だが、念のため上書きしておく
        // (= 違うアドレスの同内容文字列が来た場合に古いポインタを保持しないため)。
        m_Entries[static_cast<usize>(idx)].name = type_name;
        return;
    }

    // 新規登録: 末尾に追加。
    FEntry e;
    e.name = type_name;
    e.fn   = fn;
    m_Entries.PushBack(e);
}

/** type_name 一致の登録 1 件を末尾 swap で解除する (順序非保持)。 */
void CPropertyDrawerRegistry::UnregisterDrawer(const char* type_name) noexcept {
    const isize idx = FindIndex(type_name);
    if (idx < 0) return;
    // 末尾 swap で O(1) 削除 (順序非保持)。RegisterDrawer で再登録すれば
    // 末尾に戻るので影響は少ない。
    m_Entries.RemoveAtSwap(static_cast<usize>(idx));
}

/** type_name に対応する drawer が登録済みかを返す。 */
bool CPropertyDrawerRegistry::HasDrawer(const char* type_name) const noexcept {
    return FindIndex(type_name) >= 0;
}

/** type_name の drawer を ctx で呼ぶ (該当ありで true、未登録で false)。 */
bool CPropertyDrawerRegistry::DrawProperty(const char* type_name,
                                          const FPropertyContext& ctx) const noexcept {
    const isize idx = FindIndex(type_name);
    if (idx < 0) return false;

    const DrawerFn fn = m_Entries[static_cast<usize>(idx)].fn;
    if (fn == nullptr) {
        // 防御的: Register 時に null fn は弾いているはずだが、念のため。
        return false;
    }
    fn(ctx);
    return true;
}

/** 登録済み drawer 数を返す。 */
u32 CPropertyDrawerRegistry::DrawerCount() const noexcept {
    return static_cast<u32>(m_Entries.Size());
}

/** index 番目の drawer name を返す (範囲外は nullptr)。 */
const char* CPropertyDrawerRegistry::DrawerName(u32 index) const noexcept {
    if (static_cast<usize>(index) >= m_Entries.Size()) return nullptr;
    return m_Entries[static_cast<usize>(index)].name;
}

} // namespace acs::game::editor_core
