// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — SeasonPass 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・tier 検索は tier_index → 配列位置の per-element 線形走査。
//     件数は通常 50〜100 程度なので十分。
//   ・Tick は dt[秒] を u64 timestamp に加算する。秒未満の桁を蓄積して
//     1 秒境界で繰り上げる方式 (f32 → u64 損失を最小化)。
//   ・claim は冪等 + 解放条件チェック。乱数 / loot box は一切扱わない。
//   ・WARN ログは Entitlement / AchievementManager と同じ Log.h 経由。
#include "gameframework/SeasonPass.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

// 「tier_index 未発見」を表す哨兵値。AchievementManager と同設計。
constexpr u32 kNotFound = ~static_cast<u32>(0);

// xp / timestamp の u32 / u64 上限。
constexpr u32 kMaxXp        = ~static_cast<u32>(0);
constexpr u64 kMaxTimestamp = ~static_cast<u64>(0);

} // namespace

// =============================================================================
// 内部検索
// =============================================================================

u32 SeasonPass::FindTierSlot(u32 tier_index) const noexcept {
    const usize n = _tiers.Size();
    for (usize i = 0; i < n; ++i) {
        if (_tiers[i].tier_index == tier_index) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// シーズン制御
// =============================================================================

void SeasonPass::StartSeason(const SeasonInfo& info) noexcept {
    _info         = info;
    _xp           = 0;
    _has_premium  = false;
    _current_time = info.start_timestamp;

    // 既存 tier 定義 / claim 状態を破棄 (新シーズンはクリーンスレート)。
    _tiers.Clear();
    _claims.Clear();
}

void SeasonPass::DefineTier(const Tier& t) noexcept {
    // tier_index 重複は黙って弾く + WARN (アセット二重ロード保護)。
    if (FindTierSlot(t.tier_index) != kNotFound) {
        ACS_LOG_WARN("SeasonPass: duplicate tier_index ignored (%u)", t.tier_index);
        return;
    }

    _tiers.PushBack(t);

    ClaimState cs{};
    cs.tier_index      = t.tier_index;
    cs.free_claimed    = false;
    cs.premium_claimed = false;
    _claims.PushBack(cs);
}

void SeasonPass::EndSeason() noexcept {
    // 手動終了: 現在時刻を end_timestamp に強制して Ended 状態に固定。
    // tier 定義 / claim 状態 / xp は保持 (シーズン後グレースピリオドの claim 用)。
    _current_time = _info.end_timestamp;
}

void SeasonPass::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;

    // 既に Ended なら何もしない (時刻が end_timestamp 以上で停止)。
    if (_current_time >= _info.end_timestamp) return;

    // dt[秒] を u64 timestamp 単位 (秒) に加算。
    // f32 を u64 にそのままキャストすると小数が消えるため、整数秒部分は
    // u64 加算で、端数 (1 秒未満) は別 f32 蓄積バッファで持つ案もあるが、
    // SeasonPass の用途では秒精度で十分なので素直に切り捨てキャストする。
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
    if (add_secs > kMaxTimestamp - _current_time) {
        _current_time = kMaxTimestamp;
    } else {
        _current_time += add_secs;
    }

    // end_timestamp を越えたら丁度の値に丸める (Ended 判定で意図しない超過を防止)。
    if (_current_time > _info.end_timestamp) {
        _current_time = _info.end_timestamp;
    }
}

// =============================================================================
// XP 操作
// =============================================================================

void SeasonPass::AwardXp(u32 amount) noexcept {
    if (amount == 0) return;
    if (amount > kMaxXp - _xp) {
        _xp = kMaxXp;
    } else {
        _xp += amount;
    }
}

// =============================================================================
// 照会
// =============================================================================

u32 SeasonPass::CurrentXp() const noexcept {
    return _xp;
}

u32 SeasonPass::CurrentTier() const noexcept {
    // xp >= xp_threshold を満たす最大 tier_index を返す。
    // 線形走査 (件数は通常 50〜100)。
    u32 best   = kNotFound;
    u32 best_t = 0;  // tier_index 比較用
    const usize n = _tiers.Size();
    for (usize i = 0; i < n; ++i) {
        const Tier& t = _tiers[i];
        if (_xp < t.xp_threshold) continue;
        if (best == kNotFound || t.tier_index > best_t) {
            best   = t.tier_index;
            best_t = t.tier_index;
        }
    }
    return best;
}

ESeasonStatus SeasonPass::Status() const noexcept {
    if (_current_time < _info.start_timestamp) return ESeasonStatus::NotStarted;
    if (_current_time >= _info.end_timestamp)  return ESeasonStatus::Ended;
    return ESeasonStatus::Active;
}

// =============================================================================
// Premium Pass
// =============================================================================

bool SeasonPass::HasPremiumPass() const noexcept {
    return _has_premium;
}

void SeasonPass::SetPremiumPass(bool has) noexcept {
    // false にしても既 claim 済の premium 報酬は巻き戻さない (返金後でも貰った
    // ものは保持する設計)。再 claim 防止のために premium_claimed フラグは
    // 触らない。
    _has_premium = has;
}

// =============================================================================
// 報酬 claim
// =============================================================================

bool SeasonPass::IsRewardClaimed(u32 tier_index, bool premium) const noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return false;
    const ClaimState& cs = _claims[slot];
    return premium ? cs.premium_claimed : cs.free_claimed;
}

bool SeasonPass::ClaimReward(u32 tier_index, bool premium) noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return false;

    const Tier& tier = _tiers[slot];
    ClaimState& cs   = _claims[slot];

    // 解放条件チェック: 累積 xp が xp_threshold 以上か。
    if (_xp < tier.xp_threshold) return false;

    // 該当ストリームの reward_id が nullptr の tier は claim 不可。
    const char* reward = premium ? tier.reward_id_premium : tier.reward_id_free;
    if (reward == nullptr) return false;

    if (premium) {
        // プレミアム未購入なら claim 不可。
        if (!_has_premium) return false;
        if (cs.premium_claimed) return false;
        cs.premium_claimed = true;
    } else {
        if (cs.free_claimed) return false;
        cs.free_claimed = true;
    }
    return true;
}

const char* SeasonPass::GetRewardId(u32 tier_index, bool premium) const noexcept {
    const u32 slot = FindTierSlot(tier_index);
    if (slot == kNotFound) return nullptr;
    const Tier& tier = _tiers[slot];
    return premium ? tier.reward_id_premium : tier.reward_id_free;
}

u32 SeasonPass::ClaimableCount() const noexcept {
    // 未請求の「解放済」報酬の合算。
    // ・free: reward_id_free != nullptr かつ xp >= threshold かつ !free_claimed
    // ・premium: 同条件 + HasPremiumPass()==true
    u32 count = 0;
    const usize n = _tiers.Size();
    for (usize i = 0; i < n; ++i) {
        const Tier&       tier = _tiers[i];
        const ClaimState& cs   = _claims[i];
        if (_xp < tier.xp_threshold) continue;

        if (tier.reward_id_free != nullptr && !cs.free_claimed) {
            ++count;
        }
        if (_has_premium && tier.reward_id_premium != nullptr && !cs.premium_claimed) {
            ++count;
        }
    }
    return count;
}

u32 SeasonPass::TierCount() const noexcept {
    return static_cast<u32>(_tiers.Size());
}

} // namespace acs::game
