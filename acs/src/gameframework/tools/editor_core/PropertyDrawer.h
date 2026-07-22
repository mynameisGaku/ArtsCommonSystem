// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / PropertyDrawer
//
// **カスタム field drawer の登録レジストリ**。FInspectorPanel は EFieldKind
// 9 種を hardcode の `switch` で扱っているが、それを超える「ゲーム固有 /
// エディタ拡張型」の field 表示 (`Curve`, `Gradient`, `AssetPath`,
// `NodeIdSelector`, `KeyCombo`, ...) を後付けで追加できるようにするための拡張点。
//
// 使い方 (典型):
//   // 起動時にカスタム drawer を登録する:
//   static void DrawHealth(const FPropertyContext& ctx) noexcept {
//       auto* hp = static_cast<FHealth*>(ctx.data_ptr);
//       ImGui::ProgressBar(hp->Ratio(), ImVec2(-1, 0));
//       if (ctx.out_changed) *ctx.out_changed = false;
//   }
//   FPropertyDrawerRegistry reg;
//   reg.Init();                                   // bundled drawer も含めて初期化
//   reg.RegisterDrawer("Health", &DrawHealth);
//
//   // FInspectorPanel や任意の editor panel から:
//   FPropertyContext ctx { /* data_ptr / label / tooltip / min/max / ... */ };
//   if (!reg.DrawProperty(field_type_name, ctx)) {
//       // 未登録 type → 既存 EFieldKind switch のフォールバックへ
//   }
//
// 設計選択:
//   ・**type_name は const char* literal 前提**: drawer name は登録元が永続所有する
//     リテラル文字列を想定。本 registry はコピー所有しない (= STL `std::string` 不使用)。
//     比較は per-byte ループ (FSettings / FEntitlementRegistry と同じ StrEq pattern)。
//   ・**`DrawerFn` は raw 関数ポインタ + `FPropertyContext`**: ACS は std::function 禁止。
//     `FPropertyContext` は POD 構造体で、必要な情報 (data ポインタ / 表示名 / tooltip /
//     min/max / enum labels / out_changed) を 1 つにまとめて渡す。引数増減で
//     `DrawerFn` シグネチャが変わらないように構造体束ねを採用。
//   ・**bundled drawer 群を `Init()` で自動登録**: "F32Slider" / "Vec2Drag" /
//     "Vec3Drag" / "Vec4Drag" / "ColorRGB" / "ColorRGBA" / "AssetPath" /
//     "EnumCombo" / "TextInput" の 9 種。FInspectorPanel の hardcode switch と
//     重複するが、こちらは registry 経由で書き換え / 拡張可能。
//   ・**`AssetPath` の drag-drop payload id は "ASSET_PATH"** (リテラル定数)。
//     FAssetBrowser panel が drag-source 側で同 id の payload を SetDragDrop
//     することで、textbox に drop すると path が書き戻される。
//   ・**非コピー / 非ムーブ**: 内部 `TArray<FEntry>` の所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept / STL 不使用 / ImGui include 可**: ACS 規約に準拠。
//     ヘッダから ImGui は include しない (.cpp 内で <imgui.h> を読む)。
//
// 将来拡張余地:
//   ・**field metadata 拡張**: per-property tooltip / validation rule / step /
//     readonly / hide 属性などを `FPropertyContext` に積み上げる (今は最低限)。
//   ・**composite drawer (struct 再帰)**: drawer 内から `DrawProperty()` を再帰呼出して
//     ネスト struct (例: `FTransform2D` = FVec2 + f32 + FVec2) を 1 個の drawer として
//     扱う。本 registry はそのまま使える。
//   ・**per-game カスタム drawer**: ゲーム固有型 (`class FHealth`, `class FWeaponSlot`,
//     `class FStatBlock`) を inspector 上で美麗表示する目的。ゲーム側コードが
//     `RegisterDrawer("Health", ...)` を起動時に呼ぶだけで反映される。
//   ・**`NodeIdSelector`**: FHierarchyPanel と連動して "現在の選択を取得" or
//     "Selectable な node 一覧から Combo で選択" する drawer。FSelectionService
//     を参照するため drawer 側 closure (= `FPropertyContext` に user_data を
//     拡張) が必要になる予定。
//
// 範囲外:
//   ・FInspectorPanel との実統合 (= EFieldKind hardcode switch の置き換え)。
//   ・drawer の優先度 / 上書きルール (現状は **後勝ち**: 同 name を Register
//     したら旧 fn を置き換える)。
//   ・drawer 描画失敗時の例外伝播 (ACS は no-exception、drawer 内で完結)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"  // m_Entries が TArray<FEntry> なので header include 必須

namespace acs::game::editor_core {

/**
 * drawer に渡される全パラメータをまとめた POD。
 *
 * @details
 * フィールドはどれも省略可能 (drawer 側が必要なものだけ参照する)。data_ptr は型に
 * 応じて drawer がキャストする (例: F32Slider なら f32*、AssetPath なら char[])。
 * out_changed は drawer が「値を書き換えたか」を呼び出し側に伝える出口で、nullptr
 * なら drawer は書き戻さなくてよい。
 */
struct FPropertyContext {
    /** 編集対象データへのポインタ (drawer が型に応じてキャストする)。 */
    void*        data_ptr     = nullptr;

    /** ImGui widget の表示ラベル (nullptr 時は "##unnamed" を使う)。 */
    const char*  label        = nullptr;

    /** hover tooltip (nullptr 時は表示しない)。 */
    const char*  tooltip      = nullptr;

    /** F32Slider 等で使う最小値。 */
    f32          min_value    = 0.0f;

    /** F32Slider 等で使う最大値。 */
    f32          max_value    = 1.0f;

    /** EnumCombo の項目列 ("Item0\0Item1\0...\0" 終端の ImGui::Combo 形式)。 */
    const char*  enum_values  = nullptr;

    /** EnumCombo の有効ラベル数 (popup_max_height_in_items のヒント、0 で ImGui 既定)。 */
    u32          enum_count   = 0;

    /** drawer が編集発生時に *out_changed = true を書く出口 (nullptr 可)。 */
    bool*        out_changed  = nullptr;
};

/**
 * 1 つの property 描画関数の型。
 *
 * @details
 * 関数ポインタで実装し std::function は使わない (ACS no-exception のため noexcept
 * 必須)。drawer 内から ImGui を直接叩いてよい。
 */
using DrawerFn = void (*)(const FPropertyContext& ctx) noexcept;

/**
 * type_name (const char* literal) → DrawerFn のカスタム field drawer 登録レジストリ。
 *
 * @details
 * FInspectorPanel の hardcode switch を超える「ゲーム固有 / エディタ拡張型」の field
 * 表示を後付けで追加できる拡張点。type_name は登録元が永続所有するリテラル文字列を
 * 想定し本 registry はコピー所有しない (比較は per-byte ループ)。Init() で bundled
 * drawer 9 種 ("F32Slider" / "Vec2Drag" / "Vec3Drag" / "Vec4Drag" / "ColorRGB" /
 * "ColorRGBA" / "AssetPath" / "EnumCombo" / "TextInput") を自動登録する。内部
 * TArray<FEntry> の所有を曖昧にしないため非コピー・非ムーブ。
 */
class FPropertyDrawerRegistry {
public:
    /** 空状態で構築する (bundled drawer の登録は Init で行う)。 */
    FPropertyDrawerRegistry() noexcept = default;

    /** 破棄する (drawer は関数ポインタ参照のみで所有しない)。 */
    ~FPropertyDrawerRegistry() noexcept = default;

    /** コピー禁止 (内部 TArray<FEntry> の所有を曖昧にしないため)。 */
    FPropertyDrawerRegistry(const FPropertyDrawerRegistry&)            = delete;

    /** コピー代入も禁止。 */
    FPropertyDrawerRegistry& operator=(const FPropertyDrawerRegistry&) = delete;

    /** ムーブ禁止。 */
    FPropertyDrawerRegistry(FPropertyDrawerRegistry&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FPropertyDrawerRegistry& operator=(FPropertyDrawerRegistry&&)      = delete;

    /**
     * 既存登録を全て破棄したうえで bundled drawer 9 種を自動登録する。
     *
     * @details 多重呼び出し可 (2 回目以降は state を完全に再構築する)。
     */
    void Init() noexcept;

    /** 全 drawer 登録を破棄する (多重呼び出し可)。 */
    void Shutdown() noexcept;

    /**
     * 任意の type_name + DrawerFn を登録 / 上書きする。
     *
     * @details
     * type_name が nullptr / 空文字、または fn が nullptr の場合は no-op。同 type_name が
     * 既に登録済みなら後勝ちで fn を置き換える。type_name は呼び出し側が永続所有する
     * 文字列 (リテラル想定) で、本 registry はコピー所有しない。
     * @param type_name 登録する型名 (永続所有のリテラル想定)。
     * @param fn 描画関数ポインタ。
     */
    void RegisterDrawer(const char* type_name, DrawerFn fn) noexcept;

    /**
     * type_name 一致の登録 1 件を解除する。
     *
     * @details 内部で末尾 swap 削除するため解除後の順序は保持しない。未登録 / nullptr は no-op。
     * @param type_name 解除する型名。
     */
    void UnregisterDrawer(const char* type_name) noexcept;

    /**
     * type_name に対応する drawer が登録済みかを返す。
     *
     * @param type_name 探す型名 (nullptr / 空文字は false)。
     * @return 登録済みなら true。
     */
    bool HasDrawer(const char* type_name) const noexcept;

    /**
     * type_name の drawer を呼んで ctx で描画する。
     *
     * @details
     * 呼び出し側は false 時に既存 EFieldKind switch にフォールバックする想定。
     * @param type_name 描画に使う drawer の型名 (nullptr は false)。
     * @param ctx drawer に渡す描画パラメータ。
     * @return 該当 drawer を呼んだら true、未登録 / type_name == nullptr なら false。
     */
    bool DrawProperty(const char* type_name, const FPropertyContext& ctx) const noexcept;

    /**
     * 登録済み drawer 数を返す。
     *
     * @return bundled を含む登録 drawer 数。
     */
    u32 DrawerCount() const noexcept;

    /**
     * index 番目の drawer name を返す (デバッグ表示 / イントロスペクション用)。
     *
     * @param index 取得する drawer のインデックス。
     * @return drawer name (範囲外は nullptr)。
     */
    const char* DrawerName(u32 index) const noexcept;

    /**
     * 全 drawer 登録を破棄して空に戻す (bundled も含む)。
     *
     * @details bundled を再注入したい場合は Init() を呼び直す。
     */
    void ClearAll() noexcept;

    /** AssetPath drawer が AcceptDragDropPayload で受け取る payload id。 */
    static constexpr const char* kAssetPathPayloadId = "ASSET_PATH";

    /** TextInput / AssetPath drawer が使う固定長バッファサイズ。 */
    static constexpr u32 kTextInputBufferSize = 256u;

private:
    /**
     * 1 件の登録エントリ (type_name → DrawerFn)。
     *
     * @details name は登録者所有の永続文字列 (リテラル想定)。
     */
    struct FEntry {
        /** 登録された型名 (登録者所有の永続文字列)。 */
        const char* name = nullptr;

        /** 対応する描画関数ポインタ。 */
        DrawerFn    fn   = nullptr;
    };

    /**
     * name 一致のエントリを線形探索する (.cpp 側で per-byte StrEq を使う)。
     *
     * @param type_name 探す型名 (nullptr / 空文字は -1)。
     * @return 一致したインデックス、なければ -1。
     */
    isize FindIndex(const char* type_name) const noexcept;

    /** 登録エントリ群 (bundled 9 種 + ゲーム拡張分、少数想定で線形探索)。 */
    TArray<FEntry> m_Entries;
};

} // namespace acs::game::editor_core
