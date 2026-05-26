// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — InspectorSeam (Phase 2)
//
// reflection-driven debug inspector の「シーム (seam)」インターフェース。
// ゲーム内の任意のオブジェクト (Player / Enemy / FCamera / Settings 等) が
// 自身のフィールドを `IInspectableProvider` 経由で公開し、上位レイヤ
// (ImGui / ACS::Ui / 外部 DevTool) がそれを描画 / 編集する形を取る。
//
// 使い方:
//   class Player : public acs::game::IInspectableProvider {
//       f32  _speed = 5.0f;
//       i32  _hp    = 100;
//       bool _invincible = false;
//
//       u32 ObjectCount() noexcept override { return 1; }
//       InspectableObject GetObject(u32) noexcept override {
//           static InspectableField fields[] = {
//               { "speed",      EFieldKind::F32,  &_speed,      0, nullptr },
//               { "hp",         EFieldKind::I32,  &_hp,         0, nullptr },
//               { "invincible", EFieldKind::Bool, &_invincible, 0, nullptr },
//           };
//           return { "Player", "P1", fields, 3 };
//       }
//       void OnFieldChanged(u32, u32) noexcept override { /* re-validate */ }
//   };
//
//   // 起動時:
//   InspectorSeam inspector;
//   inspector.Init();
//   inspector.RegisterProvider(&player);
//
//   // UI レイヤ (Phase K-3 で本実装):
//   for (u32 p = 0; p < inspector.ProviderCount(); ++p) {
//       auto* prov = inspector.GetProvider(p);
//       for (u32 o = 0; o < prov->ObjectCount(); ++o) {
//           auto obj = prov->GetObject(o);
//           // ImGui::Text("[%s] %s", obj.type_name, obj.instance_name);
//           // for (u32 f = 0; f < obj.field_count; ++f) ...
//       }
//   }
//
// 設計選択 (Pillar K Phase 2):
//   ・**シーム化 (= I/F + 集中点) で UI と切り離す**: `InspectorSeam` 本体は
//     Provider レジストリだけを持ち、ImGui / Ui 描画は Phase K-3 で別レイヤから
//     呼ぶ。Ship build では Provider 登録自体を #ifdef で消す前提。
//   ・**Provider は non-owning**: ゲーム側の生存期間に従う。RegisterProvider 後に
//     Provider を destruct する場合は呼び出し側で必ず Unregister すること。
//     `InspectorSeam::ClearAll()` でも Provider は破棄しない。
//   ・**field は POD 4-tuple**: name + kind + data ポインタ + (enum 専用) ラベル配列。
//     描画側は kind で switch し、data を該当型にキャストして表示 / 編集する。
//   ・**fields 配列は Provider 所有**: `GetObject()` の戻り値が指す `fields` は
//     Provider が永続所有する (static 配列 / メンバ配列 を想定)。InspectorSeam は
//     コピーしない。
//   ・**OnFieldChanged は通知のみ**: UI 側で値を書き換えた後に呼ばれる。Provider は
//     再バリデーション (clamp / 派生値の再計算 / dirty flag) をここで行う。
//   ・**non-copy / non-move**: 内部の TArray<Provider*> 所有を曖昧にしない。
//   ・**全 noexcept**: ACS 規約。エラーは現状なし (Phase 2 は登録 / 取得のみ)。
//   ・**STL 不使用**: `acs::TArray<IInspectableProvider*>` で保持。
//
// 範囲外 (Phase K-3 以降で):
//   ・実 ImGui / ACS::Ui との接続 (描画 + 編集 widget)
//   ・複合型 (struct / array of T) の自動展開
//   ・min/max/step メタデータ (現状は raw 値のみ)
//   ・field の hide / readonly 属性
//   ・undo/redo 統合
//   ・JSON 形式での値ダンプ / ロード
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- フィールド種別 ------------------------------------------------------
// 描画 / 編集レイヤが switch して扱う「データの型タグ」。
// 現状は最小セット: スカラ + FVec2-4 + 文字列 (read-only) + Enum (整数 + ラベル配列)。
enum class EFieldKind : u8 {
    Bool,    // bool*
    I32,     // i32*
    U32,     // u32*
    F32,     // f32*
    FVec2,    // acs::FVec2*  (描画側は data を FVec2* にキャスト)
    FVec3,    // acs::FVec3*
    FVec4,    // acs::FVec4*
    FString,  // const char**  (read-only 想定。書き換えは Phase K-3+)
    Enum,    // i32* + enum_values[0..enum_value_count) のラベル
};

// ---- 公開フィールド -----------------------------------------------------
// Provider が `InspectableObject` 経由で配列を返す 1 件。配列の寿命は
// Provider が保持する (static / メンバ)。InspectorSeam はコピーしない。
struct InspectableField {
    const char*  name             = nullptr;  // フィールド表示名 (caller 所有、リテラル想定)
    EFieldKind    kind             = EFieldKind::Bool;
    void*        data             = nullptr;  // 該当型へのポインタ。kind に応じてキャスト
    u32          enum_value_count = 0;        // kind == Enum のときの有効ラベル数
    const char** enum_values      = nullptr;  // kind == Enum のときの値 → ラベル配列
};

// ---- 公開オブジェクト ---------------------------------------------------
// Provider が公開する 1 オブジェクト (例: 1 体の Player、1 つの FCamera)。
// `fields` 配列の寿命は Provider 所有。
struct InspectableObject {
    const char*       type_name     = nullptr;  // クラス名相当 ("Player" 等)
    const char*       instance_name = nullptr;  // インスタンス名 ("P1" / "Boss" 等)
    InspectableField* fields        = nullptr;  // Provider 所有
    u32               field_count   = 0;
};

// ---- 抽象 I/F: Inspectable な対象を提供する ------------------------------
// ゲーム側オブジェクトが自身 (または管理下のリスト) を Inspector に公開する。
// 1 Provider = 1 オブジェクトでも、1 Provider = N オブジェクト (Manager 型) でも OK。
class IInspectableProvider {
public:
    IInspectableProvider() noexcept = default;
    virtual ~IInspectableProvider() noexcept = default;

    IInspectableProvider(const IInspectableProvider&)            = delete;
    IInspectableProvider& operator=(const IInspectableProvider&) = delete;
    IInspectableProvider(IInspectableProvider&&)                 = delete;
    IInspectableProvider& operator=(IInspectableProvider&&)      = delete;

    // この Provider が公開するオブジェクト数。0 なら何も描画されない。
    virtual u32 ObjectCount() noexcept = 0;

    // index 番目のオブジェクトを返す。index >= ObjectCount() は呼び出し側で
    // 弾く前提 (UB を許容)。fields 配列の所有権は Provider 側に残る。
    virtual InspectableObject GetObject(u32 index) noexcept = 0;

    // UI 側が field の値を書き換えた直後に呼ばれる。Provider は clamp /
    // 派生値再計算 / dirty flag のセット等をここで行う。obj_index, field_index は
    // GetObject() / fields の添字に対応。
    virtual void OnFieldChanged(u32 obj_index, u32 field_index) noexcept = 0;
};

// ---- 集中点: Provider レジストリ ----------------------------------------
// Provider の登録 / 列挙 / 変更通知のハブ。
// 描画レイヤ (Phase K-3 で ImGui or ACS::Ui) はこのインスタンスから
// ProviderCount() / GetProvider() を回して描画する。
class InspectorSeam {
public:
    InspectorSeam() noexcept = default;
    ~InspectorSeam() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray<Provider*> の所有を曖昧にしない。
    InspectorSeam(const InspectorSeam&)            = delete;
    InspectorSeam& operator=(const InspectorSeam&) = delete;
    InspectorSeam(InspectorSeam&&)                 = delete;
    InspectorSeam& operator=(InspectorSeam&&)      = delete;

    // 初期化。現状は何もしない (将来 Phase K-3 で ImGui コンテキスト等の
    // セットアップを足す想定の予約点)。多重呼び出し可。
    void Init() noexcept;

    // Provider 登録。non-owning: provider の生存期間は呼び出し側責務。
    // 同一ポインタの多重登録は許可しない (重複は無視 + 何もしない)。
    // nullptr は無視。
    void RegisterProvider(IInspectableProvider* provider) noexcept;

    // Provider 登録解除。未登録 / nullptr は無視 (no-op)。
    // 解除後、Provider オブジェクト自体は破棄しない (non-owning なので)。
    void UnregisterProvider(IInspectableProvider* provider) noexcept;

    // 登録済み Provider 数。
    u32 ProviderCount() const noexcept;

    // index 番目の Provider。範囲外 (index >= ProviderCount()) は nullptr 安全。
    IInspectableProvider* GetProvider(u32 index) const noexcept;

    // UI 側が値を書き換えた直後に呼ぶ。指定 Provider の OnFieldChanged() に
    // forward する。範囲外 (provider_index >= ProviderCount()) は no-op。
    void NotifyFieldChanged(u32 provider_index, u32 obj_index, u32 field_index) noexcept;

    // 全 Provider 登録を破棄 (Provider 自体は破棄しない)。
    void ClearAll() noexcept;

    // TODO(Phase K-3): ImGui / ACS::Ui との接続レイヤを別 class
    //   (e.g. `InspectorImGuiAdapter`) として実装し、本クラスは描画ロジックを
    //   持たない純粋なレジストリのままにする。

private:
    TArray<IInspectableProvider*> _providers;
};

} // namespace acs::game
