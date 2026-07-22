// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FSceneTimer 実装
//
// 設計メモ:
//  ・slot 再利用は線形走査 (FTween/FSequenceRunner と同じ方針)。Scene あたりの
//    timer 数は数十〜数百が想定なので O(N) で十分。
//  ・generation は u8 (0 = 未使用, 1〜255 が有効)。255 で wrap して 1 に戻す
//    (0 にすると IsValid が常に false になり stale 検知不能になるため)。
//  ・Interval の連続発火は while (elapsed >= period) で carry する。コールバック
//    内で Cancel された場合の安全のため、ループ内で active/gen を都度確認。
//  ・Tick 中のコールバックが新規 SetTimeout/SetInterval を呼んだ場合、m_Entries
//    の PushBack が走り得る。生 reference を握り続けないよう、ループは index
//    アクセスで回し、毎反復ごとに `m_Entries[i]` を取り直す。
#include "gameframework/SceneTimer.h"

namespace acs::game {

/** inactive slot を再利用するか末尾に追加して空きスロット index を返す (上限到達で kMaxIndex)。 */
u32 FSceneTimer::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用 (キャッシュ局所性 + index 上限 24bit 保護)
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (!m_Entries[i].active) {
            return static_cast<u32>(i);
        }
    }
    // 全 slot 使用中 → 末尾に追加。24bit index 上限を守る (= 16M timer/scene)。
    if (n >= static_cast<usize>(FTimerHandle::kMaxIndex)) {
        return FTimerHandle::kMaxIndex; // sentinel: caller 側で invalid 扱い
    }
    m_Entries.PushBack({});
    return static_cast<u32>(m_Entries.Size()) - 1u;
}

/** index と gen を packed handle にまとめて返す。 */
FTimerHandle FSceneTimer::MakeHandle(u32 index, u8 gen) const noexcept {
    return FTimerHandle::Pack(index, gen);
}

/** delay_sec 後に 1 回だけ cb(user) を呼ぶ one-shot timer を登録する (不正引数は invalid handle)。 */
FTimerHandle FSceneTimer::SetTimeout(f32 delay_sec, TimerCallback cb, void* user) noexcept {
    if (cb == nullptr) return {};
    if (delay_sec <= 0.0f) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FTimerHandle::kMaxIndex) return {}; // 上限到達

    FTimerEntry& e = m_Entries[idx];
    // generation を 1 進める (0 は未使用扱いなので必ず 1 以上を保つ)
    u8 new_gen = static_cast<u8>(e.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    e.cb        = cb;
    e.user      = user;
    e.elapsed   = 0.0f;
    e.period    = delay_sec;
    e.repeating = false;
    e.active    = true;
    e.gen       = new_gen;

    ++m_ActiveCount;
    return MakeHandle(idx, new_gen);
}

/** period_sec ごとに cb(user) を繰り返す interval timer を登録する (不正引数は invalid handle)。 */
FTimerHandle FSceneTimer::SetInterval(f32 period_sec, TimerCallback cb, void* user) noexcept {
    if (cb == nullptr) return {};
    if (period_sec <= 0.0f) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FTimerHandle::kMaxIndex) return {};

    FTimerEntry& e = m_Entries[idx];
    u8 new_gen = static_cast<u8>(e.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    e.cb        = cb;
    e.user      = user;
    e.elapsed   = 0.0f;
    e.period    = period_sec;
    e.repeating = true;
    e.active    = true;
    e.gen       = new_gen;

    ++m_ActiveCount;
    return MakeHandle(idx, new_gen);
}

/** h が active なら停止して true を返す (stale / 完了済みは false)。 */
bool FSceneTimer::Cancel(FTimerHandle h) noexcept {
    if (!h.IsValid()) return false;
    const u32 idx = h.Index();
    if (idx >= m_Entries.Size()) return false;
    FTimerEntry& e = m_Entries[idx];
    if (!e.active || e.gen != h.Gen()) return false;

    e.active = false;
    e.cb     = nullptr;
    e.user   = nullptr;
    if (m_ActiveCount > 0u) --m_ActiveCount;
    return true;
}

/** 全 active timer を停止する (コールバックは呼ばない)。 */
void FSceneTimer::CancelAll() noexcept {
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        FTimerEntry& e = m_Entries[i];
        if (!e.active) continue;
        e.active = false;
        e.cb     = nullptr;
        e.user   = nullptr;
    }
    m_ActiveCount = 0u;
}

/** h が現在も有効な active timer を指しているかを返す。 */
bool FSceneTimer::IsActive(FTimerHandle h) const noexcept {
    if (!h.IsValid()) return false;
    const u32 idx = h.Index();
    if (idx >= m_Entries.Size()) return false;
    const FTimerEntry& e = m_Entries[idx];
    return e.active && e.gen == h.Gen();
}

/** dt 進めて満了した timer を登録順に発火する (Interval は大 dt で carry 連続発火)。 */
void FSceneTimer::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (m_ActiveCount == 0u) return;

    // 注意: コールバックは新規 SetTimeout/SetInterval を呼んで m_Entries.PushBack
    // を起こし得る (= reference invalidation)。ループは index で回し、毎反復で
    // size を取り直す。新規追加された timer は今回の Tick では発火させない
    // (= 登録時の elapsed=0 で次 Tick まで待つ — 直感的な挙動)。
    const usize snapshot_size = m_Entries.Size();

    for (usize i = 0; i < snapshot_size; ++i) {
        // 起動時の世代を覚えておく (コールバック内で Cancel→再 Acquire により
        // 同 slot が別 timer に化ける可能性を検出する)。
        FTimerEntry& e0 = m_Entries[i];
        if (!e0.active) continue;
        const u8 launched_gen = e0.gen;

        e0.elapsed += dt;

        if (!e0.repeating) {
            // ---- one-shot ----
            if (e0.elapsed >= e0.period) {
                TimerCallback cb = e0.cb;
                void*         u  = e0.user;
                // 完了処理を先に行ってからコールバックを呼ぶ
                // (cb 内で IsActive(handle) が false を返すべきため)
                e0.active = false;
                e0.cb     = nullptr;
                e0.user   = nullptr;
                if (m_ActiveCount > 0u) --m_ActiveCount;
                if (cb != nullptr) cb(u);
            }
        } else {
            // ---- repeating ----
            // 大 dt のとき複数回発火させる (carry 方式)。
            // コールバック内で Cancel された場合に備え、毎反復で
            // active/gen を確認する (m_Entries[i] を取り直す)。
            while (true) {
                FTimerEntry& e = m_Entries[i];
                if (!e.active || e.gen != launched_gen) break;
                if (e.elapsed < e.period) break;
                if (e.period <= 0.0f) break; // defense-in-depth

                e.elapsed -= e.period;
                TimerCallback cb = e.cb;
                void*         u  = e.user;
                if (cb != nullptr) cb(u);
                // cb 後、同 slot が Cancel→再利用されていれば next iter で
                // gen mismatch で抜ける。同 slot 継続なら次の発火判定へ。
            }
        }
    }
}

} // namespace acs::game
