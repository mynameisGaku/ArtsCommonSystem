// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * シーズン (Battle Pass) の 1 段階を表す定義。
 *
 * @details
 * tier_index をキーに DefineTier で登録し、xp_threshold 以上の累積 xp でこの tier が
 * 解放扱いになる。reward_id_free / reward_id_premium は解放時に「何が貰えるか」を表す
 * cosmetic ID で、本クラスは ID を返すだけで実体解放はしない。
 */
struct FTier {
    /** 0-origin の一意 ID (DefineTier 時のキー)。 */
    u32         tier_index        = 0;

    /** この値以上の累積 xp でこの tier が解放扱いになる閾値。 */
    u32         xp_threshold      = 0;

    /** 解放時の free ストリーム報酬 ID (非所有、nullptr = 該当ストリーム無し)。 */
    const char* reward_id_free    = nullptr;

    /** 解放時の premium ストリーム報酬 ID (非所有、nullptr = 該当ストリーム無し)。 */
    const char* reward_id_premium = nullptr;
};

/**
 * 1 シーズンの定義 (StartSeason に渡す)。
 *
 * @details
 * 期間は start_timestamp / end_timestamp で持ち、単位は呼出側が定義する (典型は Unix
 * seconds)。Tick(dt) が dt[秒] を加算する前提なので、timestamp も秒で揃えるのが推奨。
 */
struct FSeasonInfo {
    /** シーズンキー (永続化 / 分析用)。文字列リテラル想定 (非所有)。 */
    const char* season_id        = nullptr;

    /** UI 表示名 (非所有)。 */
    const char* display_name     = nullptr;

    /** シーズン開始時刻 (呼出側定義の単位、典型は Unix seconds)。 */
    u64         start_timestamp  = 0;

    /** シーズン終了時刻 (この時刻以降は Ended)。 */
    u64         end_timestamp    = 0;

    /** 期待される tier 数 (UI のプログレス表示用、DefineTier 件数と一致必須ではない)。 */
    u32         max_tier         = 0;
};

/**
 * 時刻ベースのシーズン状態。
 *
 * @details
 * 現在時刻と start/end timestamp の比較で決まり、Tick(dt) が現在時刻を進めて遷移させる。
 */
enum class ESeasonStatus : u8 {
    /** 現在時刻 < start_timestamp (シーズン未開始)。 */
    NotStarted = 0,

    /** start_timestamp <= 現在時刻 < end_timestamp (開催中)。 */
    Active     = 1,

    /** 現在時刻 >= end_timestamp、または EndSeason() の手動終了 (終了済み)。 */
    Ended      = 2,
};

/**
 * 期間限定シーズン (Battle Pass) の進行 + tier 報酬の請求状態を管理する小型マネージャ。
 *
 * @details
 * シーズン定義 + 累積 xp による tier 進行 + tier 報酬の claim 状態を 1 クラスにまとめる。
 * シーズン開始 / 終了は wall-clock timestamp で持ち、Tick(dt) によって Active → Ended の
 * 自動遷移を行う。報酬は乱数要素を持たない固定 tier 方式で、扱う報酬は cosmetic のみを
 * 強く推奨する (pay-to-win / loot box を倫理的に拒絶する設計)。非コピー・非ムーブ。
 */
class CSeasonPass {
public:
    /** 空のシーズンパスを構築する (シーズンは StartSeason で開始)。 */
    CSeasonPass()  noexcept = default;

    /** 破棄する。 */
    ~CSeasonPass() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    CSeasonPass(const CSeasonPass&)            = delete;

    /** コピー代入も禁止。 */
    CSeasonPass& operator=(const CSeasonPass&) = delete;

    /** ムーブ禁止。 */
    CSeasonPass(CSeasonPass&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSeasonPass& operator=(CSeasonPass&&)      = delete;

    /**
     * シーズンを開始する (xp = 0、premium = false、既存 tier 定義は破棄)。
     *
     * @details
     * m_CurrentTime は start_timestamp で初期化する (= 「ちょうど開始時刻」)。
     * start_timestamp >= end_timestamp も受理するが、即 Ended 状態になる。
     * @param info 開始するシーズンの定義。
     */
    void StartSeason(const FSeasonInfo& info) noexcept;

    /**
     * tier を 1 件登録する。
     *
     * @details tier_index が重複していたら no-op (WARN)。reward_id は nullptr 許容。
     * @param t 登録する tier 定義。
     */
    void DefineTier(const FTier& t) noexcept;

    /**
     * 手動でシーズンを終了させる (timestamp 経過を待たずに Ended へ遷移)。
     *
     * @details
     * m_CurrentTime = end_timestamp を強制する。tier 定義 / claim 状態は保持するので、
     * 報酬は Ended でも claim できる (シーズン後グレースピリオドを UI から実装するため)。
     */
    void EndSeason() noexcept;

    /**
     * 現在時刻を dt[秒] 進めて時刻ベースの status を更新する。
     *
     * @details
     * status が NotStarted / Active のときは経過に応じて自動で Ended へ遷移する。
     * 既に EndSeason() 済み (= m_CurrentTime >= end_timestamp) なら実質 no-op。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 累積 xp に amount を加算する (オーバーフロー時は max クランプ)。
     *
     * @details
     * Status() != Active でも加算は許可する (開始前のお試しや終了後の端数調整のため)。
     * 報酬請求側で Active 制約を強制したい場合は呼出側で Status() を確認すること。
     * @param amount 加算する xp 量。
     */
    void AwardXp(u32 amount) noexcept;

    /**
     * 現在の累積 xp を返す。
     *
     * @return シーズン内の累積 xp。
     */
    u32          CurrentXp()   const noexcept;

    /**
     * 累積 xp で解放済みの最大 tier_index を返す。
     *
     * @details 各 tier の xp_threshold と累積 xp を線形に比較する。
     * @return 解放済みの最大 tier_index。どの tier も未到達なら ~0u (哨兵値)。
     */
    u32          CurrentTier() const noexcept;

    /**
     * 現在のシーズン状態を返す。
     *
     * @return NotStarted / Active / Ended のいずれか。
     */
    ESeasonStatus Status()      const noexcept;

    /**
     * プレミアムパスを所持しているかを返す。
     *
     * @return 所持していれば true。
     */
    bool HasPremiumPass() const noexcept;

    /**
     * プレミアムパス所持フラグを設定する (課金確定 / 返金時等に呼ぶ)。
     *
     * @details false にしても既 claim 済の premium 報酬は claim 状態を保持する (巻き戻さない)。
     * @param has 所持していれば true。
     */
    void SetPremiumPass(bool has) noexcept;

    /**
     * 指定 tier の指定ストリーム (free or premium) が請求済みかを返す。
     *
     * @param tier_index 対象 tier の index。
     * @param premium true で premium ストリーム、false で free ストリームを照会。
     * @return 請求済みなら true。未定義 tier / nullptr reward_id ストリームは false。
     */
    bool IsRewardClaimed(u32 tier_index, bool premium) const noexcept;

    /**
     * 指定 tier の報酬を新規請求する (冪等)。
     *
     * @details
     * 以下のいずれかなら false: tier_index が未定義 / xp が xp_threshold 未満 (未解放) /
     * premium=true で HasPremiumPass()==false / 既に請求済み / 該当ストリームの
     * reward_id が nullptr (報酬無し tier)。
     * @param tier_index 請求する tier の index。
     * @param premium true で premium ストリーム、false で free ストリームを請求。
     * @return 新規に請求できたら true、それ以外は false。
     */
    bool ClaimReward(u32 tier_index, bool premium) noexcept;

    /**
     * 指定 tier の reward_id を返す。
     *
     * @param tier_index 対象 tier の index。
     * @param premium true で premium ストリーム、false で free ストリームの ID。
     * @return reward_id (非所有)。tier 未定義 / nullptr ストリームは nullptr。
     */
    const char* GetRewardId(u32 tier_index, bool premium) const noexcept;

    /**
     * 未請求の「解放済」報酬数 (free / premium 合算) を返す。
     *
     * @details
     * premium ストリームは HasPremiumPass()==true のときだけカウントに含める。nullptr
     * reward_id ストリームはカウント対象外。シーズン UI の赤バッジ表示に使う。
     * @return claim 可能な未請求報酬の合算数。
     */
    u32 ClaimableCount() const noexcept;

    /**
     * 登録済みの tier 件数を返す。
     *
     * @return DefineTier で登録された tier の数。
     */
    u32 TierCount() const noexcept;

private:
    /**
     * tier 報酬の claim 状態 (FTier と並行 TArray で 1:1 対応)。
     */
    struct FClaimState {
        /** 対応する FTier::tier_index (検索冗長化のため保持)。 */
        u32  tier_index      = 0;

        /** free ストリームの報酬を請求済みなら true。 */
        bool free_claimed    = false;

        /** premium ストリームの報酬を請求済みなら true。 */
        bool premium_claimed = false;
    };

    /**
     * tier_index から内部配列位置を線形検索する。
     *
     * @param tier_index 探す tier の index。
     * @return 見つかった配列位置。未検出は ~0u。
     */
    u32 FindTierSlot(u32 tier_index) const noexcept;

    /** 現在のシーズン定義 (StartSeason 時に上書き)。 */
    FSeasonInfo m_Info{};

    /** 累積 xp (シーズン内のみ意味を持つ)。 */
    u32 m_Xp = 0;

    /** プレミアムパス所持フラグ。 */
    bool m_HasPremium = false;

    /** 現在時刻 (timestamp 単位)。StartSeason で初期化、Tick で増分、EndSeason で強制セット。 */
    u64 m_CurrentTime = 0;

    /** tier 定義 (m_Claims と同 index で 1:1 対応)。 */
    TArray<FTier>       m_Tiers;

    /** tier ごとの claim 状態 (m_Tiers と同 index で 1:1 対応)。 */
    TArray<FClaimState> m_Claims;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSeasonPass = CSeasonPass;

} // namespace acs::game
