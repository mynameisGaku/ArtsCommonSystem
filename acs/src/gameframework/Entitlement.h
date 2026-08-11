// SPDX-License-Identifier: Apache-2.0
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
