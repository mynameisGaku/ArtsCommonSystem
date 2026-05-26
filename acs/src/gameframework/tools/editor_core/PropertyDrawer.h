// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / PropertyDrawer (Phase 21a)
//
// **カスタム field drawer の登録レジストリ**。Phase 20 で実装された
// InspectorPanel は EFieldKind 9 種を hardcode の `switch` で扱っているが、
// それを超える「ゲーム固有 / エディタ拡張型」の field 表示 (`Curve`,
// `Gradient`, `AssetPath`, `NodeIdSelector`, `KeyCombo`, ...) を後付けで
// 追加できるようにするための拡張点。
//
// 使い方 (典型):
//   // 起動時にカスタム drawer を登録する:
//   static void DrawHealth(const PropertyContext& ctx) noexcept {
//       auto* hp = static_cast<Health*>(ctx.data_ptr);
//       ImGui::ProgressBar(hp->Ratio(), ImVec2(-1, 0));
//       if (ctx.out_changed) *ctx.out_changed = false;
//   }
//   PropertyDrawerRegistry reg;
//   reg.Init();                                   // bundled drawer も含めて初期化
//   reg.RegisterDrawer("Health", &DrawHealth);
//
//   // InspectorPanel (Phase 22+ refactor 後) や任意の editor panel から:
//   PropertyContext ctx { /* data_ptr / label / tooltip / min/max / ... */ };
//   if (!reg.DrawProperty(field_type_name, ctx)) {
//       // 未登録 type → 既存 EFieldKind switch のフォールバックへ
//   }
//
// 設計選択 (Phase 21a):
//   ・**type_name は const char* literal 前提**: drawer name は登録元が永続所有する
//     リテラル文字列を想定。本 registry はコピー所有しない (= STL `std::string` 不使用)。
//     比較は per-byte ループ (Settings / Entitlement と同じ StrEq pattern)。
//   ・**`DrawerFn` は raw 関数ポインタ + `PropertyContext`**: ACS は std::function 禁止。
//     `PropertyContext` は POD 構造体で、必要な情報 (data ポインタ / 表示名 / tooltip /
//     min/max / enum labels / out_changed) を 1 つにまとめて渡す。引数増減で
//     `DrawerFn` シグネチャが変わらないように構造体束ねを採用。
//   ・**bundled drawer 群を `Init()` で自動登録**: "F32Slider" / "Vec2Drag" /
//     "Vec3Drag" / "Vec4Drag" / "ColorRGB" / "ColorRGBA" / "AssetPath" /
//     "EnumCombo" / "TextInput" の 9 種。InspectorPanel の hardcode switch と
//     重複するが、こちらは registry 経由で書き換え / 拡張可能。Phase 22+ で
//     InspectorPanel が registry 経由で draw するように refactor 想定。
//   ・**`AssetPath` の drag-drop payload id は "ASSET_PATH"** (リテラル定数)。
//     将来 AssetBrowser panel が drag-source 側で同 id の payload を SetDragDrop
//     することで、textbox に drop すると path が書き戻される。
//   ・**非コピー / 非ムーブ**: 内部 `TArray<Entry>` の所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept / STL 不使用 / ImGui include 可**: ACS 規約に準拠。
//     ヘッダから ImGui は include しない (.cpp 内で <imgui.h> を読む)。
//
// 将来拡張余地 (Phase 21b 以降):
//   ・**field metadata 拡張**: per-property tooltip / validation rule / step /
//     readonly / hide 属性などを `PropertyContext` に積み上げる (今は最低限)。
//   ・**composite drawer (struct 再帰)**: drawer 内から `DrawProperty()` を再帰呼出して
//     ネスト struct (例: `Transform2D` = FVec2 + f32 + FVec2) を 1 個の drawer として
//     扱う。本 registry はそのまま使える。
//   ・**per-game カスタム drawer**: ゲーム固有型 (`class Health`, `class WeaponSlot`,
//     `class StatBlock`) を inspector 上で美麗表示する目的。ゲーム側コードが
//     `RegisterDrawer("Health", ...)` を起動時に呼ぶだけで反映される。
//   ・**`NodeIdSelector`**: HierarchyPanel と連動して "現在の選択を取得" or
//     "Selectable な node 一覧から Combo で選択" する drawer。SelectionService
//     を参照するため drawer 側 closure (= `PropertyContext` に user_data を
//     拡張) が必要になる予定。
//
// 範囲外 (本 Phase 21a では持たない):
//   ・InspectorPanel との実統合 (= EFieldKind hardcode switch の置き換え)。
//     Phase 22+ で InspectorPanel が registry を引くように refactor。
//   ・drawer の優先度 / 上書きルール (現状は **後勝ち**: 同 name を Register
//     したら旧 fn を置き換える)。
//   ・drawer 描画失敗時の例外伝播 (ACS は no-exception、drawer 内で完結)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"  // _entries が Array<Entry> なので header include 必須

namespace acs::game::editor_core {

// ---- PropertyContext ----------------------------------------------------
// drawer に渡される全パラメータをまとめた POD。フィールドはどれも省略可能
// (drawer 側が必要なものだけ参照する)。`data_ptr` は型に応じて drawer が
// キャストする (例: F32Slider なら `f32*` に、AssetPath なら `char[]` に)。
//
// `out_changed` は drawer が「値を書き換えたか」を呼び出し側に伝える出口。
// nullptr が来た場合は drawer は書き戻さなくてよい (= 呼び出し側が dirty
// 検出に使う気が無いケース)。
struct PropertyContext {
    void*        data_ptr     = nullptr;  // 編集対象データへのポインタ (drawer がキャスト)
    const char*  label        = nullptr;  // ImGui widget の表示ラベル (nullptr 時は "##unnamed")
    const char*  tooltip      = nullptr;  // hover tooltip (nullptr 時は表示しない)
    f32          min_value    = 0.0f;     // F32Slider 等で使う最小値
    f32          max_value    = 1.0f;     // F32Slider 等で使う最大値
    const char*  enum_values  = nullptr;  // EnumCombo: "Item0\0Item1\0...\0" 終端 (ImGui::Combo 形式)
    u32          enum_count   = 0;        // EnumCombo: 有効ラベル数
    bool*        out_changed  = nullptr;  // drawer が編集発生時に *out_changed = true を書く (nullptr 可)
};

// ---- DrawerFn -----------------------------------------------------------
// 1 つの property 描画関数。関数ポインタで実装し、`std::function` は使わない。
// `noexcept` 必須 (ACS no-exception)。drawer 内から ImGui を直接叩いてよい。
using DrawerFn = void (*)(const PropertyContext& ctx) noexcept;

// ---- PropertyDrawerRegistry ---------------------------------------------
// type_name (const char* literal) → DrawerFn の単純な map。コピー / ムーブ禁止。
// 9 種の bundled drawer は `Init()` 内で自動登録される。
class PropertyDrawerRegistry {
public:
    PropertyDrawerRegistry() noexcept = default;
    ~PropertyDrawerRegistry() noexcept = default;

    // 非コピー / 非ムーブ: 内部 `TArray<Entry>` の所有を曖昧にしない (ACS 規約)。
    PropertyDrawerRegistry(const PropertyDrawerRegistry&)            = delete;
    PropertyDrawerRegistry& operator=(const PropertyDrawerRegistry&) = delete;
    PropertyDrawerRegistry(PropertyDrawerRegistry&&)                 = delete;
    PropertyDrawerRegistry& operator=(PropertyDrawerRegistry&&)      = delete;

    // 初期化。既存登録を全て破棄したうえで bundled drawer 9 種を自動登録する。
    // 多重呼び出し可 (= 2 回目以降は state を完全に再構築)。
    void Init() noexcept;

    // 後片付け。全 drawer 登録を破棄。多重呼び出し可。
    void Shutdown() noexcept;

    // 任意 type_name + DrawerFn を登録 / 上書き。
    //   ・`type_name` が nullptr / 空文字、または `fn` が nullptr の場合は no-op。
    //   ・同 type_name が既に登録済みなら **後勝ち** で `fn` を置き換える。
    //   ・`type_name` は呼び出し側が永続所有する文字列 (リテラル想定)。本 registry
    //     はコピー所有しない。
    void RegisterDrawer(const char* type_name, DrawerFn fn) noexcept;

    // type_name 一致の登録 1 件を解除。未登録 / nullptr は no-op。
    // 解除後の順序は保持しない (内部で末尾 swap で削除)。
    void UnregisterDrawer(const char* type_name) noexcept;

    // type_name に対応する drawer が登録済みか。nullptr / 空文字は false。
    bool HasDrawer(const char* type_name) const noexcept;

    // type_name の drawer を呼んで `ctx` で描画。
    //   ・成功 (= 該当 drawer あり、呼び出した) なら true。
    //   ・未登録 (= 該当 drawer なし) or `type_name == nullptr` なら false。
    //     呼び出し側は false 時に既存 EFieldKind switch にフォールバックする想定。
    bool DrawProperty(const char* type_name, const PropertyContext& ctx) const noexcept;

    // 登録済み drawer 数 (bundled 含む)。
    u32 DrawerCount() const noexcept;

    // index 番目の drawer name。範囲外は nullptr。デバッグ表示 / イントロスペクション用。
    const char* DrawerName(u32 index) const noexcept;

    // 全 drawer 登録を破棄 (bundled も含めて空に戻す)。Init で再注入したい場合は
    // `Init()` を呼び直す (= ClearAll + bundled 再登録)。
    void ClearAll() noexcept;

    // ---- 公開定数 -------------------------------------------------------
    // AssetPath drawer が `AcceptDragDropPayload` で受け取る payload id。
    // AssetBrowser 側 (将来実装) が同 id でファイル path を `SetDragDropPayload`
    // するとテキストボックスにドロップ書き戻しが起きる契約。
    static constexpr const char* kAssetPathPayloadId = "ASSET_PATH";

    // TextInput drawer が使う固定長バッファサイズ (ImGui::InputText 制限と同形)。
    static constexpr u32 kTextInputBufferSize = 256u;

private:
    // 1 件の登録エントリ。`name` は登録者所有の永続文字列 (リテラル想定)。
    // POD 構造体 (= snake_case メンバ名、`_` prefix なし、ACS 規約に従う)。
    struct Entry {
        const char* name = nullptr;
        DrawerFn    fn   = nullptr;
    };

    // 線形探索で name 一致 index を返す。nullptr / 空文字は -1。
    // .cpp 側で per-byte StrEq を使う。
    isize FindIndex(const char* type_name) const noexcept;

    // 動的配列。bundled 9 種 + ゲーム拡張分。少数 (~10-30 件) を想定するため
    // hash table ではなく線形探索で十分。
    TArray<Entry> _entries;
};

} // namespace acs::game::editor_core
