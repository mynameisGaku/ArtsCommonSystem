// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — CAchievementManager 実装
//
// 設計上のポイント:
//   ・id 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     FEntitlement.cpp と同じ StrEq を anonymous namespace に再掲する (両方とも
//     2〜3 行で重複コストが軽く、cross-file の inline helper を増やすより
//     ピラー単位で独立しているほうが読みやすい)。
//   ・実績件数は通常 50〜300 のオーダーなので線形走査で十分。
//   ・Bridge へは Unlock() / 自動 unlock のタイミングで一度だけ送信。
//     失敗 (= Bridge 未初期化 / 未実装) は WARN ログだけ残し、ローカル進捗
//     を破棄しない。
//   ・unlock_timestamp は Clock::MillisSinceStartup()。プラットフォーム時刻
//     (wall clock) ではなく起動からの相対 ms で十分というのは設計判断。
#include "gameframework/AchievementManager.h"

#include "gameframework/SteamworksBridge.h"  // ISteamworksBridge 完全型 + UnlockAchievement()
#include "platform/Time.h"                   // Clock::MillisSinceStartup()
#include "foundation/Result.h"               // Result<void> の IsErr 判定

namespace acs::game {

namespace {

/**
 * const char* を per-byte で安全に比較する。
 *
 * @details どちらかが nullptr なら false を返す (StrEq("a", nullptr) は false)。
 * @param a 比較対象の文字列 1。
 * @param b 比較対象の文字列 2。
 * @return 両者が同一内容かつ非 nullptr なら true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/** 「id 未発見」を表す哨兵値 (u32 全ビット 1 = 0xFFFFFFFF)。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

/** id 文字列を全実績から線形検索し、index か kNotFound を返す。 */
u32 CAchievementManager::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return kNotFound;
    const usize n = m_Defs.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Defs[i].id, id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** 内部 unlock 共通処理 (進捗を max に固定し、timestamp 記録と Bridge 送信を行う)。 */
void CAchievementManager::UnlockInternal(u32 index) noexcept {
    // 呼出側で index 有効性は保証済 (FindIndex から流れてくる) だが、
    // 念のため範囲チェック (Reset 等経由のレースに備える保険)。
    if (static_cast<usize>(index) >= m_Progress.Num()) return;

    FAchievementProgress& p = m_Progress[index];
    if (p.unlocked) return;  // 多重 unlock は no-op (timestamp は最初の値を保持)

    p.current_progress = p.max_progress;
    p.unlocked         = true;
    // 起動からの ms。プラットフォーム時刻が必要になったら拡張する。
    p.unlock_timestamp = CClock::MillisSinceStartup();

    // Bridge attach 中なら SDK へ送信。失敗時はローカル進捗には影響させない。
    if (m_Bridge != nullptr) {
        TResult<void> r = m_Bridge->UnlockAchievement(p.id);
        if (r.IsErr()) {
            // Stub だと未実装で必ず Err になるため、Warn 1 行で抑える。
            // 実 SDK 統合後は Err = 通信失敗 / 未初期化 → 監視対象に格上げ可。
            ACS_LOG_WARN("CAchievementManager: bridge UnlockAchievement('%s') failed",
                         p.id != nullptr ? p.id : "<null>");
        }
    }
}

/** 実績定義を登録する (重複 id / nullptr は弾き、max_progress=0 は 1 に丸める)。 */
void CAchievementManager::RegisterAchievement(const FAchievementDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindIndex(def.id) != kNotFound) {
        ACS_LOG_WARN("CAchievementManager: duplicate registration ignored ('%s')", def.id);
        return;
    }

    // max_progress = 0 は意味不明なので 1 に丸めて「単発フラグ」扱いに。
    FAchievementDef d = def;
    if (d.max_progress == 0) d.max_progress = 1;

    FAchievementProgress p{};
    p.id               = d.id;
    p.current_progress = 0;
    p.max_progress     = d.max_progress;
    p.unlocked         = false;
    p.unlock_timestamp = 0;

    m_Defs.Add(d);
    m_Progress.Add(p);
}

/** 進捗を絶対値で設定し、max 到達なら自動 unlock する。 */
void CAchievementManager::SetProgress(const char* id, u32 progress) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;

    FAchievementProgress& p = m_Progress[idx];
    if (p.unlocked) return;  // 既 unlock は変更しない (再 reset したい場合は Reset() 経由で)

    // max_progress クランプ。
    p.current_progress = progress >= p.max_progress ? p.max_progress : progress;

    // 達成したら自動 unlock。
    if (p.current_progress >= p.max_progress) {
        UnlockInternal(idx);
    }
}

/** 進捗を delta 加算し、オーバーフロー / 上限超過を max にクランプして自動 unlock する。 */
void CAchievementManager::IncrementProgress(const char* id, u32 delta) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;

    FAchievementProgress& p = m_Progress[idx];
    if (p.unlocked) return;

    // u32 オーバーフロー時の安全側クランプ。
    // (a) delta が異常に大きい、(b) current + delta が wrap する、いずれも max にクランプ。
    u32 next;
    if (delta >= p.max_progress || p.current_progress + delta < p.current_progress) {
        next = p.max_progress;
    } else {
        next = p.current_progress + delta;
        if (next > p.max_progress) next = p.max_progress;
    }
    p.current_progress = next;

    if (p.current_progress >= p.max_progress) {
        UnlockInternal(idx);
    }
}

/** id を検索して即時 unlock する (見つからなければ no-op)。 */
void CAchievementManager::Unlock(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;
    UnlockInternal(idx);
}

/** 単一実績の進捗を初期化する (Bridge 側のリセットは行わない)。 */
void CAchievementManager::Reset(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;
    FAchievementProgress& p = m_Progress[idx];
    p.current_progress = 0;
    p.unlocked         = false;
    p.unlock_timestamp = 0;
    // Bridge 側のリセットは呼ばない (SDK 仕様によっては不可能 / 危険)。
}

/** 全実績の進捗を初期化する。 */
void CAchievementManager::ResetAll() noexcept {
    const usize n = m_Progress.Num();
    for (usize i = 0; i < n; ++i) {
        FAchievementProgress& p = m_Progress[i];
        p.current_progress = 0;
        p.unlocked         = false;
        p.unlock_timestamp = 0;
    }
}

/** 実績が unlock 済みかを返す (id が無ければ false)。 */
bool CAchievementManager::IsUnlocked(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;
    return m_Progress[idx].unlocked;
}

/** 実績の現在進捗を返す (id が無ければ 0)。 */
u32 CAchievementManager::GetProgress(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return 0;
    return m_Progress[idx].current_progress;
}

/** 登録済み実績の総数を返す。 */
u32 CAchievementManager::TotalCount() const noexcept {
    return static_cast<u32>(m_Progress.Num());
}

/** unlock 済み実績の数を数えて返す。 */
u32 CAchievementManager::UnlockedCount() const noexcept {
    u32 c = 0;
    const usize n = m_Progress.Num();
    for (usize i = 0; i < n; ++i) {
        if (m_Progress[i].unlocked) ++c;
    }
    return c;
}

/** 単一実績の状態ポインタを返す (id が無ければ nullptr)。 */
const FAchievementProgress* CAchievementManager::GetState(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return nullptr;
    return &m_Progress[idx];
}

/** 全実績状態の生バッファを返し、件数を out_count に書き出す。 */
const FAchievementProgress* CAchievementManager::AllStates(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Progress.Num());
    return m_Progress.GetData();
}

/** Storefront SDK ブリッジを attach / detach する (nullptr で detach)。 */
void CAchievementManager::AttachSteamworks(ISteamworksBridge* bridge) noexcept {
    // nullptr で detach は明示的に許可 (オフラインモードへ戻す)。
    m_Bridge = bridge;
}

/** 毎フレーム更新フック (現状は完全に no-op の予約点)。 */
void CAchievementManager::Tick(f32 dt) noexcept {
    // 現状は完全 no-op。「累計時間系実績」を内蔵する場合の予約点。
    (void)dt;
}

} // namespace acs::game
