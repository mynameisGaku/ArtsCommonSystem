// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FAchievementManager 実装
//
// 設計上のポイント:
//   ・id 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     FEntitlement.cpp と同じ StrEq を anonymous namespace に再掲する (両方とも
//     2〜3 行で重複コストが軽く、cross-file の inline helper を増やすより
//     ピラー単位で独立しているほうが読みやすい)。
//   ・実績件数は通常 50〜300 のオーダーなので線形走査で十分。
//   ・Bridge へは Unlock() / 自動 unlock のタイミングで一度だけ送信。
//     失敗 (= Bridge 未初期化 / 未実装) は WARN ログだけ残し、ローカル進捗
//     を破棄しない (オフライン → オンライン復帰の再送は Phase 3+)。
//   ・unlock_timestamp は Clock::MillisSinceStartup()。プラットフォーム時刻
//     (wall clock) ではなく起動からの相対 ms で十分というのは設計判断 (Phase 2)。
#include "gameframework/AchievementManager.h"

#include "gameframework/SteamworksBridge.h"  // ISteamworksBridge 完全型 + UnlockAchievement()
#include "platform/Time.h"                   // Clock::MillisSinceStartup()
#include "foundation/Result.h"               // Result<void> の IsErr 判定

namespace acs::game {

namespace {

// const char* の per-byte 安全比較。FEntitlement.cpp と同設計。
// どちらかが nullptr なら false を返す (StrEq("a", nullptr) は false)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「id 未発見」を表す哨兵値。u32 全ビット 1 (= 0xFFFFFFFF)。
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

// =============================================================================
// 内部ユーティリティ
// =============================================================================

u32 FAchievementManager::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return kNotFound;
    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Defs[i].id, id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

void FAchievementManager::UnlockInternal(u32 index) noexcept {
    // 呼出側で index 有効性は保証済 (FindIndex から流れてくる) だが、
    // 念のため範囲チェック (Reset 等経由のレースに備える保険)。
    if (static_cast<usize>(index) >= m_Progress.Size()) return;

    FAchievementProgress& p = m_Progress[index];
    if (p.unlocked) return;  // 多重 unlock は no-op (timestamp は最初の値を保持)

    p.current_progress = p.max_progress;
    p.unlocked         = true;
    // 起動からの ms。プラットフォーム時刻が必要になったら Phase 3 で拡張。
    p.unlock_timestamp = Clock::MillisSinceStartup();

    // Bridge attach 中なら SDK へ送信。失敗時はローカル進捗には影響させない。
    if (m_Bridge != nullptr) {
        TResult<void> r = m_Bridge->UnlockAchievement(p.id);
        if (r.IsErr()) {
            // Stub だと未実装で必ず Err になるため、Warn 1 行で抑える。
            // 実 SDK 統合後は Err = 通信失敗 / 未初期化 → 監視対象に格上げ可。
            ACS_LOG_WARN("FAchievementManager: bridge UnlockAchievement('%s') failed",
                         p.id != nullptr ? p.id : "<null>");
        }
    }
}

// =============================================================================
// 定義登録
// =============================================================================

void FAchievementManager::RegisterAchievement(const FAchievementDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindIndex(def.id) != kNotFound) {
        ACS_LOG_WARN("FAchievementManager: duplicate registration ignored ('%s')", def.id);
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

    m_Defs.PushBack(d);
    m_Progress.PushBack(p);
}

// =============================================================================
// 進捗操作
// =============================================================================

void FAchievementManager::SetProgress(const char* id, u32 progress) noexcept {
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

void FAchievementManager::IncrementProgress(const char* id, u32 delta) noexcept {
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

void FAchievementManager::Unlock(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;
    UnlockInternal(idx);
}

void FAchievementManager::Reset(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return;
    FAchievementProgress& p = m_Progress[idx];
    p.current_progress = 0;
    p.unlocked         = false;
    p.unlock_timestamp = 0;
    // Bridge 側のリセットは呼ばない (SDK 仕様によっては不可能 / 危険)。
}

void FAchievementManager::ResetAll() noexcept {
    const usize n = m_Progress.Size();
    for (usize i = 0; i < n; ++i) {
        FAchievementProgress& p = m_Progress[i];
        p.current_progress = 0;
        p.unlocked         = false;
        p.unlock_timestamp = 0;
    }
}

// =============================================================================
// 照会
// =============================================================================

bool FAchievementManager::IsUnlocked(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;
    return m_Progress[idx].unlocked;
}

u32 FAchievementManager::GetProgress(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return 0;
    return m_Progress[idx].current_progress;
}

u32 FAchievementManager::TotalCount() const noexcept {
    return static_cast<u32>(m_Progress.Size());
}

u32 FAchievementManager::UnlockedCount() const noexcept {
    u32 c = 0;
    const usize n = m_Progress.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Progress[i].unlocked) ++c;
    }
    return c;
}

const FAchievementProgress* FAchievementManager::GetState(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return nullptr;
    return &m_Progress[idx];
}

const FAchievementProgress* FAchievementManager::AllStates(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Progress.Size());
    return m_Progress.Data();
}

// =============================================================================
// FSteamworksBridge 統合
// =============================================================================

void FAchievementManager::AttachSteamworks(ISteamworksBridge* bridge) noexcept {
    // nullptr で detach は明示的に許可 (オフラインモードへ戻す)。
    m_Bridge = bridge;
}

// =============================================================================
// Tick
// =============================================================================

void FAchievementManager::Tick(f32 dt) noexcept {
    // Phase 2 では完全 no-op。将来「累計時間系実績」を内蔵する場合の予約。
    (void)dt;
}

} // namespace acs::game
