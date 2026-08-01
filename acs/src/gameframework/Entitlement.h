// SPDX-License-Identifier: Apache-2.0
// CEntitlementRegistry — DLC / シーズンパス / コスメティック等の権利チェック
//
// プレイヤーが「持っているかどうか」をゲームロジック側から問い合わせる窓口。
// DLC、シーズンパス、バトルパス、コスメティックパック、グッズ同梱の引換コード等の
// **権利情報** (entitlement) をローカルに保持し、ストアからの取得結果や
// プラットフォーム SDK (Pillar S = Steamworks / EOS / 各家庭機 SDK) の問い合わせ結果を
// Add() で流し込んでもらう想定。CEntitlementRegistry 自体は **ストア非依存** であり、
// 配信プラットフォームに紐付かない (Pillar S 側がアダプタ層になる)。
//
// 設計上の倫理方針 (LiveOps と pay-to-win の境界):
//   ・本レジストリは **cosmetic 中心** で運用することを推奨する。装備性能や
//     ゲームプレイのコア体験を金銭で買わせる pay-to-win 設計は、開発者の責任で
//     避けるべきという立場 (ACS の出荷フィルタ層には強制機構を置かないが、
//     IsActive() を見て「攻撃力 +10」のようなコードを書くと簡単に踏み外せる)。
//   ・GoodsRedeemCode は物販同梱のリアル商品コード等を想定。再現性ある安全な
//     redeem フローは Pillar S 側で実装し、ここには結果だけが流れてくる。
//
// 使い方:
//   CEntitlementRegistry reg;
//   reg.Add({ "dlc.expansion_1",  EEntitlementKind::Dlc,           true  });
//   reg.Add({ "cosmetic.hat_red", EEntitlementKind::CosmeticPack,  true  });
//
//   if (reg.IsActive("dlc.expansion_1")) UnlockExpansionMap();
//   if (reg.HasAny(EEntitlementKind::SeasonPass)) ShowSeasonPassBadge();
//
// 設計選択:
//   ・id は **const char* 非所有**: ACS の STL 禁止方針 + 寿命管理単純化のため、
//     呼び出し側が文字列リテラル or 長寿命バッファを保証する。短命バッファを
//     渡すと dangling になる点は API ドキュメントで明示。
//   ・コピー / ムーブ禁止: registry は通常 1 つの長寿命オブジェクトで運用される。
//     誤って値渡しされた結果として entitlement が分裂すると検知しづらいため、
//     最初から非コピー・非ムーブで固定する。
//   ・全 noexcept: 例外不使用方針 (TResult<T,E> + bool 戻り値)。
//
// 範囲外:
//   ・永続化 / シリアライズ (Pillar J Serialize 側で扱う)
//   ・プラットフォーム SDK 連携の具象 (Pillar S Storefront 側で実装、こちらは依存しない)
//   ・期限付き entitlement (期限切れ判定や残時間照会) — `active` フラグの更新を
//     ストア側で行う前提とし、本レジストリは時間概念を持たない
//   ・サーバ検証 (Pillar V Backend Services 側で実装)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 権利種別。
 *
 * @details
 * Pillar S 側のアダプタが分類してから Add() に渡す前提で、レジストリは単に
 * 分類タグとして保持・検索キーに使う。
 */
enum class EEntitlementKind : u8 {
    /** 追加コンテンツ (マップ / シナリオ / キャラ等)。 */
    Dlc,

    /** 一定期間 / シーズン束ねの権利。 */
    SeasonPass,

    /** tier 進行型 (cosmetic 中心を推奨)。 */
    BattlePass,

    /** 見た目だけのスキン・装飾。 */
    CosmeticPack,

    /** 物販同梱コード等から redeem された結果。 */
    GoodsRedeemCode,
};

/**
 * 1 つの権利情報。
 *
 * @details `id` は registry 側で所有しない (呼び出し側が寿命を保証する)。
 */
struct FEntitlementInfo {
    /** 権利を識別する文字列 (非所有、呼び出し側が寿命を保証)。 */
    const char*     id     = nullptr;

    /** 権利の分類タグ。 */
    EEntitlementKind kind   = EEntitlementKind::Dlc;

    /** 有効フラグ (false で「持ってはいるが現在無効」を表現可)。 */
    bool            active = false;
};

/**
 * 権利情報 (entitlement) をローカルに保持し問い合わせるレジストリ。
 *
 * @details
 * DLC・シーズンパス・コスメティックパック等の権利をストア非依存で保持し、
 * IsActive()/HasAny() でゲームロジックから所持判定を行う。Pillar S (ストア SDK)
 * 側のアダプタが取得結果を Add() で流し込む想定。非コピー・非ムーブ。
 */
class CEntitlementRegistry {
public:
    /** 空のレジストリを構築する。 */
    CEntitlementRegistry()  noexcept = default;

    /** 破棄する (保持していた権利情報を解放)。 */
    ~CEntitlementRegistry() noexcept = default;

    /** コピー禁止 (通常 1 つの長寿命オブジェクトで運用するため)。 */
    CEntitlementRegistry(const CEntitlementRegistry&)            = delete;

    /** コピー代入も禁止。 */
    CEntitlementRegistry& operator=(const CEntitlementRegistry&) = delete;

    /** ムーブ禁止 (entitlement の分裂を防ぐため)。 */
    CEntitlementRegistry(CEntitlementRegistry&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEntitlementRegistry& operator=(CEntitlementRegistry&&)      = delete;

    /**
     * 新規 entitlement を登録する。
     *
     * @details
     * 同一 id の重複は禁止せず、上書きもしない (Pillar S 側で dedup する想定)。
     * id == nullptr は no-op で防御する。
     * @param info 登録する権利情報。
     */
    void Add(FEntitlementInfo info) noexcept;

    /**
     * id が登録済みかを返す (active 不問)。
     *
     * @param id 探す権利 id (nullptr なら false)。
     * @return 登録済みなら true。
     */
    bool Has(const char* id) const noexcept;

    /**
     * id が登録済み かつ active かを返す。
     *
     * @param id 探す権利 id (nullptr なら false)。
     * @return 登録済みかつ active なら true。
     */
    bool IsActive(const char* id) const noexcept;

    /**
     * 指定 kind の active な entitlement が 1 つでもあるかを返す。
     *
     * @details 「シーズンパス所持者向け UI を出すか」等の判定に使う。
     * @param k 探す権利種別。
     * @return 該当する active な権利が 1 つでもあれば true。
     */
    bool HasAny(EEntitlementKind k) const noexcept;

    /**
     * 全 entitlement を削除する (ストア再同期時に呼ばれる想定)。
     */
    void Clear() noexcept;

    /**
     * 登録件数を返す (active / inactive 含む)。
     *
     * @return 保持している権利情報の件数。
     */
    u32 Count() const noexcept;

    /**
     * 生バッファの先頭ポインタを返す (デバッグ表示 / イテレーション用)。
     *
     * @return Count() 件の連続バッファ。Clear() / Add() で無効化される。
     */
    const FEntitlementInfo* AllInfos() const noexcept;

private:
    /** 登録済み権利情報の配列。 */
    TArray<FEntitlementInfo> m_Infos;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FEntitlementRegistry = CEntitlementRegistry;

} // namespace acs::game
