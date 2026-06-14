// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FInspectorSeam
//
// reflection-driven debug inspector の「シーム (seam)」インターフェース。
// ゲーム内の任意のオブジェクト (Player / Enemy / FCamera / FSettings 等) が
// 自身のフィールドを `IInspectableProvider` 経由で公開し、上位レイヤ
// (ImGui / ACS::Ui / 外部 DevTool) がそれを描画 / 編集する形を取る。
//
// 使い方:
//   class Player : public acs::game::IInspectableProvider {
//       f32  m_Speed = 5.0f;
//       i32  m_Hp    = 100;
//       bool m_bInvincible = false;
//
//       u32 ObjectCount() noexcept override { return 1; }
//       InspectableObject GetObject(u32) noexcept override {
//           static InspectableField fields[] = {
//               { "speed",      EFieldKind::F32,  &m_Speed,      0, nullptr },
//               { "hp",         EFieldKind::I32,  &m_Hp,         0, nullptr },
//               { "invincible", EFieldKind::Bool, &m_bInvincible, 0, nullptr },
//           };
//           return { "Player", "P1", fields, 3 };
//       }
//       void OnFieldChanged(u32, u32) noexcept override { /* re-validate */ }
//   };
//
//   // 起動時:
//   FInspectorSeam inspector;
//   inspector.Init();
//   inspector.RegisterProvider(&player);
//
//   // UI レイヤ:
//   for (u32 p = 0; p < inspector.ProviderCount(); ++p) {
//       auto* prov = inspector.GetProvider(p);
//       for (u32 o = 0; o < prov->ObjectCount(); ++o) {
//           auto obj = prov->GetObject(o);
//           // ImGui::Text("[%s] %s", obj.type_name, obj.instance_name);
//           // for (u32 f = 0; f < obj.field_count; ++f) ...
//       }
//   }
//
// 設計選択:
//   ・**シーム化 (= I/F + 集中点) で UI と切り離す**: `FInspectorSeam` 本体は
//     Provider レジストリだけを持ち、ImGui / Ui 描画は別レイヤから
//     呼ぶ。Ship build では Provider 登録自体を #ifdef で消す前提。
//   ・**Provider は non-owning**: ゲーム側の生存期間に従う。RegisterProvider 後に
//     Provider を destruct する場合は呼び出し側で必ず Unregister すること。
//     `FInspectorSeam::ClearAll()` でも Provider は破棄しない。
//   ・**field は POD 4-tuple**: name + kind + data ポインタ + (enum 専用) ラベル配列。
//     描画側は kind で switch し、data を該当型にキャストして表示 / 編集する。
//   ・**fields 配列は Provider 所有**: `GetObject()` の戻り値が指す `fields` は
//     Provider が永続所有する (static 配列 / メンバ配列 を想定)。FInspectorSeam は
//     コピーしない。
//   ・**OnFieldChanged は通知のみ**: UI 側で値を書き換えた後に呼ばれる。Provider は
//     再バリデーション (clamp / 派生値の再計算 / dirty flag) をここで行う。
//   ・**non-copy / non-move**: 内部の TArray<Provider*> 所有を曖昧にしない。
//   ・**全 noexcept**: ACS 規約。エラーは現状なし (登録 / 取得のみ)。
//   ・**STL 不使用**: `acs::TArray<IInspectableProvider*>` で保持。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"  // FNodeId (node-keyed provider lookup 用)

namespace acs::game {

/**
 * フィールド種別 (描画 / 編集レイヤが扱うデータの型タグ)。
 *
 * @details
 * 描画 / 編集レイヤが switch して扱う。現状は最小セット: スカラ + FVec2-4 +
 * 文字列 (read-only) + Enum (整数 + ラベル配列)。
 */
enum class EFieldKind : u8 {
    /** bool* を指す。 */
    Bool,

    /** i32* を指す。 */
    I32,

    /** u32* を指す。 */
    U32,

    /** f32* を指す。 */
    F32,

    /** acs::FVec2* を指す (描画側は data を FVec2* にキャスト)。 */
    FVec2,

    /** acs::FVec3* を指す。 */
    FVec3,

    /** acs::FVec4* を指す。 */
    FVec4,

    /** const char** を指す (read-only 想定)。 */
    FString,

    /** i32* + enum_values[0..enum_value_count) のラベルを持つ。 */
    Enum,

    /** i32* (= 参照先オブジェクトの安定 ID、-1 で «なし»)。エディタはノードピッカーで編集する。 */
    ObjectRef,
};

/**
 * Provider が公開する 1 フィールド。
 *
 * @details
 * Provider が `InspectableObject` 経由で配列を返す 1 件。配列の寿命は
 * Provider が保持する (static / メンバ)。FInspectorSeam はコピーしない。
 */
struct InspectableField {
    /** フィールド表示名 (caller 所有、リテラル想定)。 */
    const char*  name             = nullptr;

    /** このフィールドのデータ型タグ。 */
    EFieldKind    kind             = EFieldKind::Bool;

    /** 該当型へのポインタ。kind に応じてキャストする。 */
    void*        data             = nullptr;

    /** kind == Enum のときの有効ラベル数。 */
    u32          enum_value_count = 0;

    /** kind == Enum のときの値 → ラベル配列。 */
    const char** enum_values      = nullptr;
};

/**
 * Provider が公開する 1 オブジェクト (例: 1 体の Player、1 つの FCamera)。
 *
 * @details `fields` 配列の寿命は Provider 所有。
 */
struct InspectableObject {
    /** クラス名相当 ("Player" 等)。 */
    const char*       type_name     = nullptr;

    /** インスタンス名 ("P1" / "Boss" 等)。 */
    const char*       instance_name = nullptr;

    /** 公開フィールド配列 (Provider 所有)。 */
    InspectableField* fields        = nullptr;

    /** fields の要素数。 */
    u32               field_count   = 0;
};

/**
 * Inspectable な対象を提供する抽象インターフェース。
 *
 * @details
 * ゲーム側オブジェクトが自身 (または管理下のリスト) を Inspector に公開する。
 * 1 Provider = 1 オブジェクトでも、1 Provider = N オブジェクト (Manager 型) でも OK。
 */
class IInspectableProvider {
public:
    /** 既定構築。 */
    IInspectableProvider() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IInspectableProvider() noexcept = default;

    /** コピー禁止。 */
    IInspectableProvider(const IInspectableProvider&)            = delete;

    /** コピー代入も禁止。 */
    IInspectableProvider& operator=(const IInspectableProvider&) = delete;

    /** ムーブ禁止。 */
    IInspectableProvider(IInspectableProvider&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IInspectableProvider& operator=(IInspectableProvider&&)      = delete;

    /**
     * この Provider が公開するオブジェクト数を返す。
     *
     * @return 公開オブジェクト数 (0 なら何も描画されない)。
     */
    virtual u32 ObjectCount() noexcept = 0;

    /**
     * index 番目のオブジェクトを返す。
     *
     * @details
     * index >= ObjectCount() は呼び出し側で弾く前提 (UB を許容)。fields 配列の
     * 所有権は Provider 側に残る。
     * @param index 取得するオブジェクトの添字。
     * @return index 番目の公開オブジェクト。
     */
    virtual InspectableObject GetObject(u32 index) noexcept = 0;

    /**
     * UI 側が field の値を書き換えた直後に呼ばれる通知フック。
     *
     * @details Provider は clamp / 派生値再計算 / dirty flag のセット等をここで行う。
     * @param obj_index GetObject() の添字に対応するオブジェクト番号。
     * @param field_index fields 配列の添字に対応するフィールド番号。
     */
    virtual void OnFieldChanged(u32 obj_index, u32 field_index) noexcept = 0;
};

/**
 * Provider の登録 / 列挙 / 変更通知のハブ (Provider レジストリ)。
 *
 * @details
 * 描画レイヤ (ImGui or ACS::Ui) はこのインスタンスから ProviderCount() /
 * GetProvider() を回して描画する。
 */
class FInspectorSeam {
public:
    /** 空のレジストリで構築する。 */
    FInspectorSeam() noexcept = default;

    /** 破棄する (Provider は non-owning なので破棄しない)。 */
    ~FInspectorSeam() noexcept = default;

    /** コピー禁止 (内部 TArray<Provider*> の所有を曖昧にしないため)。 */
    FInspectorSeam(const FInspectorSeam&)            = delete;

    /** コピー代入も禁止。 */
    FInspectorSeam& operator=(const FInspectorSeam&) = delete;

    /** ムーブ禁止。 */
    FInspectorSeam(FInspectorSeam&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FInspectorSeam& operator=(FInspectorSeam&&)      = delete;

    /**
     * 初期化する (多重呼び出し可)。
     *
     * @details 現状は何もしない (ImGui コンテキスト等のセットアップを足す想定の予約点)。
     */
    void Init() noexcept;

    /**
     * node 紐付けなしで Provider を登録する。
     *
     * @details
     * non-owning: provider の生存期間は呼び出し側責務。同一ポインタの多重登録は
     * 許可しない (重複は無視)。nullptr は無視。紐付く FNodeId は invalid となるため、
     * FNodeId からの逆引きを使いたい場合は RegisterProviderForNode を使うこと。
     * @param provider 登録する Provider (non-owning)。
     */
    void RegisterProvider(IInspectableProvider* provider) noexcept;

    /**
     * node 紐付きで Provider を登録する。
     *
     * @details
     * `node_id` をキーに後で GetProviderForNode で逆引きできる。non-owning は
     * RegisterProvider と同じ。同一 provider ポインタの多重登録は弾く
     * (この場合 node_id の更新も行わない)。nullptr は無視。FNodeId は
     * 「slot index + generation」を表すハンドルで provider レジストリの登録順
     * index とは別物のため、node→provider の対応をここで明示的に記録する。
     * @param node_id 逆引きキーにする FNodeId。
     * @param provider 登録する Provider (non-owning)。
     */
    void RegisterProviderForNode(FNodeId node_id, IInspectableProvider* provider) noexcept;

    /**
     * Provider 登録を解除する。
     *
     * @details
     * 未登録 / nullptr は無視 (no-op)。解除後、Provider オブジェクト自体は破棄
     * しない (non-owning なので)。
     * @param provider 解除する Provider。
     */
    void UnregisterProvider(IInspectableProvider* provider) noexcept;

    /**
     * 登録済み Provider 数を返す。
     *
     * @return 登録されている Provider の個数。
     */
    u32 ProviderCount() const noexcept;

    /**
     * 登録順 index で Provider を取得する。
     *
     * @details
     * 範囲外 (index >= ProviderCount()) は nullptr 安全。ここでの index は
     * 登録順 (provider レジストリ上の位置) であり、FNodeId の Index() ではない。
     * FNodeId から引きたい場合は GetProviderForNode を使う。
     * @param index 登録順の添字。
     * @return index 番目の Provider (範囲外なら nullptr)。
     */
    IInspectableProvider* GetProvider(u32 index) const noexcept;

    /**
     * FNodeId をキーに紐付く Provider を逆引きする。
     *
     * @details
     * RegisterProviderForNode で登録した node_id に完全一致 (index + generation)
     * する provider を返す。これが node-keyed lookup の正しい入口。
     * @param node_id 逆引きする FNodeId。
     * @return 一致する Provider (一致なし / invalid id なら nullptr)。
     */
    IInspectableProvider* GetProviderForNode(FNodeId node_id) const noexcept;

    /**
     * 指定 Provider の OnFieldChanged() に変更を forward する。
     *
     * @details 範囲外 (provider_index >= ProviderCount()) は no-op。
     * @param provider_index 通知先 Provider の登録順 index。
     * @param obj_index 変更されたオブジェクトの添字。
     * @param field_index 変更されたフィールドの添字。
     */
    void NotifyFieldChanged(u32 provider_index, u32 obj_index, u32 field_index) noexcept;

    /** 全 Provider 登録を破棄する (Provider 自体は破棄しない)。 */
    void ClearAll() noexcept;

private:
    /**
     * 登録済み Provider (non-owning)。m_NodeIds と同じ index で対応する。
     *
     * @details
     * m_Providers[i] と m_NodeIds[i] は常に同じ index で対応する parallel array。
     * 追加 / swap-remove / Clear は両者を必ず揃えて操作すること。
     */
    TArray<IInspectableProvider*> m_Providers;

    /**
     * 各 Provider に紐付く FNodeId (node 紐付けなしは invalid FNodeId{})。
     *
     * @details m_Providers と同じ index で対応する parallel array。
     */
    TArray<FNodeId>               m_NodeIds;
};

} // namespace acs::game
