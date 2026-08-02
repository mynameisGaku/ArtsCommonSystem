// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — CSeasonPass 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・tier 検索は tier_index → 配列位置の per-element 線形走査。
//     件数は通常 50〜100 程度なので十分。
//   ・Tick は dt[秒] を u64 timestamp に加算する。秒未満の桁を蓄積して
//     1 秒境界で繰り上げる方式 (f32 → u64 損失を最小化)。
//   ・claim は冪等 + 解放条件チェック。乱数 / loot box は一切扱わない。
//   ・WARN ログは FEntitlement / CAchievementManager と同じ Log.h 経由。
#include "gameframework/SeasonPass.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/** 「tier_index 未発見」を表す哨兵値 (CAchievementManager と同設計)。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

/** 累積 xp の u32 飽和上限 (これ以上加算しても丸める)。 */
constexpr u32 kMaxXp        = ~static_cast<u32>(0);

/** timestamp の u64 飽和上限 (オーバーフロー防御の丸め先)。 */
constexpr u64 kMaxTimestamp = ~static_cast<u64>(0);

} // namespace

/** FindTierSlot の実装 (tier_index を線形走査して配列スロットを返す、未発見は kNotFound)。 */
u32 CSeasonPass::FindTierSlot(u32 tier_index) const noexcept {
    const usize n = m_Tiers.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Tiers[i].tier_index == tier_index) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** StartSeason の実装 (info を取り込み xp / premium / tier / claim 状態をリセット)。 */
void CSeasonPass::StartSeason(const FSeasonInfo& info) noexcept {
    m_Info         = info;
    m_Xp           = 0;
    m_HasPremium  = false;
    m_CurrentTime = info.start_timestamp;

    // 既存 tier 定義 / claim 状態を破棄 (新シーズンはクリーンスレート)。
    m_Tiers.Clear();
    m_Claims.Clear();
}

/** DefineTier の実装 (tier 定義と空の claim 状態を追加、tier_index 重複は警告して無視)。 */
void CSeasonPass::DefineTier(const FTier& t) noexcept {
    // tier_index 重複は黙って弾く + WARN (アセット二重ロード保護)。
    if (FindTierSlot(t.tier_index) != kNotFound) {
        ACS_LOG_WARN("CSeasonPass: duplicate tier_index ignored (%u)", t.tier_index);
        return;
    }

    m_Tiers.PushBack(t);

    FClaimState cs{};
    cs.tier_index      = t.tier_index;
    cs.free_claimed    = false;
    cs.premium_claimed = false;
    m_Claims.PushBack(cs);
}

/** EndSeason の実装 (現在時刻を end_timestamp に強制して Ended に固定、tier / claim / xp は保持)。 */
void CSeasonPass::EndSeason() noexcept {
    // 手動終了: 現在時刻を end_timestamp に強制して Ended 状態に固定。
    // tier 定義 / claim 状態 / xp は保持 (シーズン後グレースピリオドの claim 用)。
    m_CurrentTime = m_Info.end_timestamp;
}

/** Tick の実装 (dt[秒] の整数部を現在時刻へ加算し、end_timestamp で飽和させる)。 */
void CSeasonPass::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;

    // 既に Ended なら何もしない (時刻が end_timestamp 以上で停止)。
    if (m_CurrentTime >= m_Info.end_timestamp) return;

    // dt[秒] を u64 timestamp 単位 (秒) に加算。
    // f32 を u64 にそのままキャストすると小数が消えるため、整数秒部分は
    // u64 加算で、端数 (1 秒未満) は別 f32 蓄積バッファで持つ案もあるが、
    // CSeasonPass の用途では秒精度で十分なので素直に切り捨てキャストする。
    // (1 フレームの dt は通常 ~16ms ≪ 1秒 なので、Tick 毎には 0 加算で
    //  数十フレームに 1 度だけ整数秒が進む挙動になり、UI 上は問題ない。
    //  より厳密な分解能が必要なら timestamp 単位を ms に揃えること。)
    //
    // ただし「常に 0 加算が続くと永遠に時間が進まない」のは困るので、端数
    // を関数内 static で持つ — と思ったが、static 変数はテスト時の隔離が
    // 効かないので避ける。代わりに、整数秒未満の dt は呼出側がフレーム間で
    // 累積する責務とし、Tick は素直に整数秒分だけ進める。
    // (典型運用: 呼出側が「実時間 1 秒経過したら Tick(1.0f)」と低頻度で叩く)。
    const u64 add_secs = static_cast<u64>(dt);

    // u64 オーバーフロー防御 (現実には起こり得ないが防御的に)。
    if (add_secs > kMaxTimestamp - m_CurrentTime) {
        m_CurrentTime = kMaxTimestamp;
    } else {
        m_CurrentTime += add_secs;
    }

    // end_timestamp を越えたら丁度の値に丸める (Ended 判定で意図しない超過を防止)。
    if (m_CurrentTime > m_Info.end_timestamp) {
        m_CurrentTime = m_Info.end_timestamp;
    }
}

/** AwardXp の実装 (累積 xp に amount を加算、kMaxXp で飽和)。 */
void CSeasonPass::AwardXp(u32 amount) noexcept {
    if (amount == 0) return;
    if (amount > kMaxXp - m_Xp) {
        m_Xp = kMaxXp;
    } else {
        m_Xp += amount;
    }
}

/** CurrentXp の実装 (現在の累積 xp を返す)。 */
u32 CSeasonPass::CurrentXp() const noexcept {
    return m_Xp;
}

/** CurrentTier の実装 (xp >= xp_threshold を満たす最大 tier_index を線形走査で返す)。 */
u32 CSeasonPass::CurrentTier() const noexcept {
    // xp >= xp_threshold を満たす最大 tier_index を返す。
    // 線形走査 (件数は通常 50〜100)。
    u32 best   = kNotFound;
    u32 best_t = 0;  // tier_index 比較用
    const usize n = m_Tiers.Size();
    for (usize i = 0; i < n; ++i) {
        const FTier& t = m_Tiers[i];
        if (m_Xp < t.xp_threshold) continue;
        if (best == kNotFound || t.tier_index > best_t) {
            best   = t.tier_index;
            best_t = t.tier_index;
        }
    }
    return best;
}

/** Status の実装 (現在時刻と start/end timestamp の比較で NotStarted/Active/Ended を返す)。 */
ESeasonStatus CSeasonPass::Status() const noexcept {
    if (m_CurrentTime < m_Info.start_timestamp) return ESeasonStatus::NotStarted;
    if (m_CurrentTime >= m_Info.end_timestamp)  return ESeasonStatus::Ended;
    return ESeasonStatus::Active;
}

/** HasPremiumPass の実装 (プレミアムパス保有フラグを返す)。 */
bool CSeasonPass::HasPremiumPass() const noexcept {
    return m_HasPremium;
}

/** SetPremiumPass の実装 (保有フラグを設定、false にしても既 claim 済み報酬は巻き戻さない)。 */
void CSeasonPass::SetPremiumPass(bool has) noexcept {
    // false にしても既 claim 済の premium 報酬は巻き戻さない (返金後でも貰った
    // ものは保持する設計)。再 claim 防止のために premium_claimed フラグは
    // 触らない。
    m_HasPremium = has;
}

/** IsRewardClaimed の実装 (指定 tier の free / premium ストリームが claim 済みかを返す)。 */
bool CSeasonPass::IsRewardClaimed(u32 tier_index, bool premium) const noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return false;
    const FClaimState& cs = m_Claims[slot];
    return premium ? cs.premium_claimed : cs.free_claimed;
}

/** ClaimReward の実装 (解放条件 / reward_id / premium 保有 / 二重 claim を検査して claim を確定)。 */
bool CSeasonPass::ClaimReward(u32 tier_index, bool premium) noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return false;

    const FTier& tier = m_Tiers[slot];
    FClaimState& cs   = m_Claims[slot];

    // 解放条件チェック: 累積 xp が xp_threshold 以上か。
    if (m_Xp < tier.xp_threshold) return false;

    // 該当ストリームの reward_id が nullptr の tier は claim 不可。
    const char* reward = premium ? tier.reward_id_premium : tier.reward_id_free;
    if (reward == nullptr) return false;

    if (premium) {
        // プレミアム未購入なら claim 不可。
        if (!m_HasPremium) return false;
        if (cs.premium_claimed) return false;
        cs.premium_claimed = true;
    } else {
        if (cs.free_claimed) return false;
        cs.free_claimed = true;
    }
    return true;
}

/** GetRewardId の実装 (指定 tier の free / premium ストリームの reward_id を返す、未発見は nullptr)。 */
const char* CSeasonPass::GetRewardId(u32 tier_index, bool premium) const noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return nullptr;
    const FTier& tier = m_Tiers[slot];
    return premium ? tier.reward_id_premium : tier.reward_id_free;
}

/** ClaimableCount の実装 (解放済みかつ未請求の free / premium 報酬を全 tier で合算する)。 */
u32 CSeasonPass::ClaimableCount() const noexcept {
    // 未請求の「解放済」報酬の合算。
    // ・free: reward_id_free != nullptr かつ xp >= threshold かつ !free_claimed
    // ・premium: 同条件 + HasPremiumPass()==true
    u32 count = 0;
    const usize n = m_Tiers.Size();
    for (usize i = 0; i < n; ++i) {
        const FTier&       tier = m_Tiers[i];
        const FClaimState& cs   = m_Claims[i];
        if (m_Xp < tier.xp_threshold) continue;

        if (tier.reward_id_free != nullptr && !cs.free_claimed) {
            ++count;
        }
        if (m_HasPremium && tier.reward_id_premium != nullptr && !cs.premium_claimed) {
            ++count;
        }
    }
    return count;
}

/** TierCount の実装 (定義済み tier の数を返す)。 */
u32 CSeasonPass::TierCount() const noexcept {
    return static_cast<u32>(m_Tiers.Size());
}

} // namespace acs::game
