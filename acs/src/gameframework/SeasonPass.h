// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — FSeasonPass (Battle Pass / Season tracker)
//
// 期間限定シーズン (Battle Pass) の定義 + 累積 XP に対する tier 進行 + tier 報酬の
// 請求状態を 1 クラスにまとめた小型マネージャ。シーズン開始 / 終了は wall-clock
// timestamp で持ち、Tick(dt) によって `Active → Ended` の自動遷移を行う。
//
// 倫理方針 (重要):
//   ・本クラスが扱う報酬は **cosmetic のみ** を強く推奨する。装備性能・ダメージ
//     倍率等、ゲームプレイ核心に影響する報酬を Premium tier に置く設計は
//     pay-to-win に直結し、ACS としては明示的に反対する立場 (FEntitlement.h の
//     方針を継承)。loot box (中身がランダムで対価が確率に依存する仕組み) も
//     倫理的に拒絶しており、本 API には乱数要素を一切持たせない (= 払うと
//     必ず指定の cosmetic が得られる、固定 tier 方式に限定)。
//   ・FSeasonPass 自体には強制機構を置かないが、`reward_id_*` は cosmetic 系
//     entitlement / unlock を指すよう設計時に揃えること。
//
// 想定する位置付け:
//   ・Pillar O (FEntitlement) との違い:
//     - FEntitlement は「持っているかどうか」の権利フラグを保持する受動レジストリ。
//     - FSeasonPass は「期間内に xp を稼ぐと tier が上がり、各 tier の報酬を請求
//       できる」進行 + claim 状態の能動マネージャ。報酬 ID は entitlement 側に
//       橋渡しする想定 (本クラスは ID を吐き出すだけ)。
//   ・Pillar O (FProgression / FAchievementManager) との違い:
//     - FProgression は「永続レベル / 累積 milestone」を扱う、シーズンを跨いだ進行。
//     - FAchievementManager は「永続実績」を扱い、SDK と双方向。
//     - FSeasonPass は「シーズン (start/end timestamp) で区切られたリセット可能な
//       進行」を扱う。シーズン切替で完全リセットされる想定。
//
// 使い方:
//   FSeasonPass sp;
//
//   SeasonInfo info;
//   info.season_id        = "season.spring_2026";
//   info.display_name     = "Spring 2026";
//   info.start_timestamp  = 1'748'736'000ull;        // 2026-06-01 (Unix seconds)
//   info.end_timestamp    = 1'756'598'400ull;        // 2026-08-31
//   info.max_tier         = 50;
//   sp.StartSeason(info);
//
//   // 起動時にゲーム側で tier を定義 (xp 閾値で並び、tier_index は 0..max_tier-1)。
//   sp.DefineTier({ 0, 100,  "cosmetic.frame_basic_t00", "cosmetic.skin_premium_t00" });
//   sp.DefineTier({ 1, 250,  "cosmetic.frame_basic_t01", "cosmetic.skin_premium_t01" });
//   // ...
//
//   // (任意) プレミアムパス購入が確定したら通知。FEntitlement 側で課金検証する。
//   sp.SetPremiumPass(true);
//
//   // ゲームプレイ中の xp 加算 + 毎フレームの時刻更新。
//   sp.AwardXp(50);
//   sp.Tick(dt_seconds);
//
//   // UI で claimable を表示。
//   if (sp.ClaimableCount() > 0) ShowSeasonRewardBadge();
//
//   // プレイヤーが UI から請求。
//   if (sp.ClaimReward(/*tier_index*/ 0, /*premium*/ false)) {
//       // 新規 claim 成功 → entitlement 側に reward_id を追加
//       entitlement.Add({ sp.GetRewardId(0, false), EntitlementKind::CosmeticPack, true });
//   }
//
// 設計選択 (Pillar O Phase 2):
//   ・**Tier は事前定義型 (固定 reward_id)**: 乱数 / loot box を避けるため、tier
//     ごとに free 報酬 ID と premium 報酬 ID を 1 つずつ静的に持つ。両方とも
//     nullptr 可 (= その tier には該当ストリームの報酬がない、例: free のみ tier)。
//   ・**tier_index は 0-origin で連続**: DefineTier(t) は t.tier_index を一意な
//     インデックスとして扱う。重複 index は黙って弾く (no-op + WARN)。
//     xp_threshold は単調増加であることが期待されるが、本クラスは強制しない
//     (呼出側の責務)。
//   ・**xp は u32 累積**: FProgression と同じ pattern。オーバーフロー時は max クランプ。
//   ・**CurrentTier() は xp_threshold ベースの線形走査**: tier 件数は通常 50〜100
//     のオーダーなので二分探索化は不要。`xp >= threshold` を満たす最大 tier_index
//     を返す (どれも満たさなければ ~0u = 「未到達」)。
//   ・**claim 状態は Def と並行 TArray**: ClaimState{ tier_index; free_claimed;
//     premium_claimed } を Tier 定義と 1:1 で持つ。`bool` 2 つで Pillar S
//     FAchievementProgress と同じ Def/State 分離設計を踏襲。
//   ・**status は (start/end timestamp) と現在時刻の比較**:
//       NotStarted: now < start_timestamp
//       Active:     start_timestamp <= now < end_timestamp
//       Ended:      now >= end_timestamp
//     現在時刻は内部カウンタ `_current_time` で保持し、Tick(dt) で増分。
//     StartSeason 時に `_current_time = start_timestamp` で初期化する設計 (Tick
//     呼出が始まる前は「ちょうど開始時刻」と仮定)。f32 dt は秒として扱い、
//     timestamp 単位も「秒」として一貫させる (呼出側責務 — UTC seconds since epoch
//     を渡すのが典型)。
//   ・**ClaimReward は冪等 + bool 返し**: 既請求 tier に再 claim しても false (新規でない)。
//     未到達 tier への claim も false。新規請求が成功したときだけ true。
//   ・**ClaimableCount は未請求の解放済 reward の合算**: free / premium 両方を
//     カウント (premium pass 未所持なら premium 分は除外)。シーズン UI で
//     「赤バッジ何個」表示に使う。
//   ・**EndSeason は手動終了用**: timestamp 経過を待たずにシーズン完了させたい
//     管理者操作 / デバッグ用。`_current_time = end_timestamp` を強制する。
//   ・**永続化は範囲外 (Phase 3+)**: FProgression と同じく、Save/Load は本フェーズ
//     では未実装。Pillar J Serialize 統合後に追加する。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 3+ で):
//   ・永続化 (Save/Load) — Pillar J Serialize と統合
//   ・サーバ側 claim 検証 — Pillar V Backend Services に委譲
//   ・ストア連携 (premium pass 購入トランザクション) — Pillar S Storefront に委譲
//   ・複数シーズンの同時進行 / 履歴保持 — シーズンは「現在の 1 つ」のみを扱う
//   ・wall-clock の自動取得 — 現在時刻は呼出側が timestamp で渡し、Tick で増分させる
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- Tier: シーズンの 1 段階 -----------------------------------------------
// tier_index   : 0-origin の一意 ID (DefineTier 時のキー)。
// xp_threshold : この値以上の累積 xp で当該 tier 解放扱い。
// reward_id_free / reward_id_premium :
//                解放時に「何が貰えるか」を表す cosmetic ID。本クラスは ID を
//                返すだけで実体解放はしない。nullptr 可 (= 該当ストリーム無し)。
struct Tier {
    u32         tier_index        = 0;
    u32         xp_threshold      = 0;
    const char* reward_id_free    = nullptr;
    const char* reward_id_premium = nullptr;
};

// ---- SeasonInfo: 1 シーズンの定義 ------------------------------------------
// season_id      : シーズンキー (永続化 / 分析用)。文字列リテラル想定 (非所有)。
// display_name   : UI 表示名 (非所有)。
// start_timestamp / end_timestamp :
//                  シーズン期間。単位は呼出側が定義 (典型は Unix seconds)。
//                  Tick(dt) が dt[秒] を加算する前提なので、timestamp も秒推奨。
// max_tier       : 期待される tier 数 (UI のプログレス表示用に保存)。
//                  DefineTier 件数が必ず一致する必要はない (情報用)。
struct SeasonInfo {
    const char* season_id        = nullptr;
    const char* display_name     = nullptr;
    u64         start_timestamp  = 0;
    u64         end_timestamp    = 0;
    u32         max_tier         = 0;
};

// ---- ESeasonStatus: 時刻ベースのシーズン状態 -------------------------------
//   NotStarted : 現在時刻 < start_timestamp
//   Active     : start_timestamp <= 現在時刻 < end_timestamp
//   Ended      : 現在時刻 >= end_timestamp (or EndSeason() 手動呼出)
enum class ESeasonStatus : u8 {
    NotStarted = 0,
    Active     = 1,
    Ended      = 2,
};

// ---- FSeasonPass -----------------------------------------------------------
class FSeasonPass {
public:
    FSeasonPass()  noexcept = default;
    ~FSeasonPass() noexcept = default;

    FSeasonPass(const FSeasonPass&)            = delete;
    FSeasonPass& operator=(const FSeasonPass&) = delete;
    FSeasonPass(FSeasonPass&&)                 = delete;
    FSeasonPass& operator=(FSeasonPass&&)      = delete;

    // ---- シーズン定義 / 制御 ---------------------------------------------
    // 当該シーズンを開始 (xp = 0, premium = false, 既存 tier 定義は破棄)。
    // start_timestamp >= end_timestamp は受理するが、即 Ended 状態になる。
    // _current_time は start_timestamp で初期化 (= 「ちょうど開始」)。
    void StartSeason(const SeasonInfo& info) noexcept;

    // tier を 1 件登録。tier_index 重複は no-op (WARN)。
    // reward_id_free / reward_id_premium は nullptr 許容。
    void DefineTier(const Tier& t) noexcept;

    // 手動でシーズンを終了させる (timestamp 経過を待たずに Ended に遷移)。
    // _current_time = end_timestamp を強制。tier 定義 / claim 状態は保持する
    // (報酬は Ended でも claim 可能 — UI からシーズン後グレースピリオドを
    //  実装するため)。
    void EndSeason() noexcept;

    // 時刻ベース status 更新。dt は秒。
    // status が NotStarted / Active → 自動で Ended へ遷移する。
    // EndSeason() が既に呼ばれている (= _current_time >= end_timestamp) なら no-op。
    void Tick(f32 dt) noexcept;

    // ---- XP 操作 ----------------------------------------------------------
    // 累積 xp に amount を加算 (オーバーフロー時は max クランプ)。
    // Status() != Active でも加算は許可する (シーズン開始前のお試し / 終了後の
    // 端数調整に使えるよう柔軟側に倒す)。報酬請求側で Active 制約を別途強制
    // したい場合は呼出側で Status() を見ること。
    void AwardXp(u32 amount) noexcept;

    // ---- 照会 -------------------------------------------------------------
    u32          CurrentXp()   const noexcept;
    // 累積 xp と各 tier の xp_threshold を比較し、解放済みの最大 tier_index を返す。
    // どの tier も到達していなければ ~0u (= 「未到達」を表す哨兵値)。
    u32          CurrentTier() const noexcept;
    ESeasonStatus Status()      const noexcept;

    // ---- Premium Pass -----------------------------------------------------
    bool HasPremiumPass() const noexcept;
    // 課金確定 / 返金時等に呼ぶ。false にしても既 claim 済の premium 報酬は
    // claim 状態を保持する (= 過去に貰ったものを巻き戻さない設計)。
    void SetPremiumPass(bool has) noexcept;

    // ---- 報酬 claim -------------------------------------------------------
    // 指定 tier_index の指定ストリーム (free or premium) が請求済みか。
    // 未定義 tier / nullptr reward_id ストリームは false。
    bool IsRewardClaimed(u32 tier_index, bool premium) const noexcept;

    // 報酬を新規請求。true if newly claimed (= 解放条件を満たしかつ未請求だった)。
    // 失敗条件 (false):
    //   ・tier_index が未定義
    //   ・xp が当該 tier の xp_threshold 未満 (未解放)
    //   ・premium=true で HasPremiumPass()==false (プレミアム未購入)
    //   ・既に請求済み
    //   ・該当ストリームの reward_id が nullptr (= 報酬無し tier)
    bool ClaimReward(u32 tier_index, bool premium) noexcept;

    // 指定 tier の reward_id を取得。tier 未定義 / nullptr ストリームは nullptr。
    const char* GetRewardId(u32 tier_index, bool premium) const noexcept;

    // 未請求の「解放済」報酬数 (free / premium 合算)。
    // premium ストリームは HasPremiumPass()==true のときだけカウントに含める。
    // nullptr reward_id ストリームはカウント対象外。UI バッジ用。
    u32 ClaimableCount() const noexcept;

    // 登録済 tier 件数。
    u32 TierCount() const noexcept;

private:
    // claim 状態。Tier と並行 TArray で 1:1 対応。
    struct ClaimState {
        u32  tier_index      = 0;     // Tier::tier_index と一致 (検索冗長化)
        bool free_claimed    = false;
        bool premium_claimed = false;
    };

    // tier_index から内部配列位置を線形検索。未検出は ~0u。
    u32 FindTierSlot(u32 tier_index) const noexcept;

    // シーズン定義 (StartSeason 時に上書き)。
    SeasonInfo _info{};

    // 累積 xp (シーズン内のみ意味を持つ)。
    u32 _xp = 0;

    // プレミアムパス所持フラグ。
    bool _has_premium = false;

    // 現在時刻 (timestamp 単位)。StartSeason で start_timestamp に初期化、
    // Tick(dt) で dt[秒] を加算、EndSeason で end_timestamp に強制セット。
    u64 _current_time = 0;

    // Tier 定義 + claim 状態 (同 index で 1:1 対応)。
    TArray<Tier>       _tiers;
    TArray<ClaimState> _claims;
};

} // namespace acs::game
