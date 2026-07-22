// SPDX-License-Identifier: Apache-2.0
// GameFramework — FRollbackSession 実装
//
// 設計メモ:
//   ・入力台帳と状態履歴は同じ長さ (history_length) の同位相リング。tick %
//     history のモジュロが一致するため「HasFrame(t) が true ⇔ 台帳 slot も
//     まだ t のまま」が成り立ち、SubmitInput の受理判定が HasFrame だけで済む。
//   ・used 台帳は「実際に sim へ渡した値」。遅延確定入力との bit 比較で
//     再シミュレーション省略を判定し、繰り返し予測の種にもなる。
//   ・ResimulateIfNeeded 途中の SaveFrame 失敗 (OOM) は dirty を保持したまま
//     false を返す。World は tick 境界の一貫状態で止まるため、次の
//     AdvanceTick が同じ dirty から安全に再試行できる。
#include "gameframework/RollbackSession.h"

namespace acs::game {

namespace {

/**
 * 2 つの入力の実効値 (buttons / axis) が bit 一致するかを返す。
 *
 * @details tick / player_id はリング座標から自明なので比較しない。
 */
bool SameEffectiveInput(const FInputFrame& a, const FInputFrame& b) noexcept
{
    return a.buttons == b.buttons && a.axis.x == b.axis.x && a.axis.y == b.axis.y;
}

} // namespace

bool FRollbackSession::Init(FWorld* world, const FRollbackSessionConfig& config) noexcept
{
    // 再 Init に備えて先に未初期化状態へ戻す (失敗時もこの状態で返す)。
    m_World = nullptr;
    m_History.Shutdown();

    if (world == nullptr) return false;
    if (config.player_count == 0 || config.player_count > kMaxRollbackPlayers) return false;
    if (config.history_length == 0) return false;
    if (config.max_prediction != 0 && config.max_prediction >= config.history_length) return false;

    const u32 slots = config.history_length * config.player_count;
    if (!m_History.Init(config.history_length)) return false;
    if (!m_SlotTick.TryResize(config.history_length) ||
        !m_Confirmed.TryResize(slots) ||
        !m_Ledger.TryResize(slots) ||
        !m_Used.TryResize(slots) ||
        !m_TickInputs.TryResize(config.player_count)) {
        m_History.Shutdown();
        return false;
    }
    for (usize i = 0; i < m_SlotTick.Size(); ++i) m_SlotTick[i] = kInvalidSlotTick;
    for (usize i = 0; i < m_Confirmed.Size(); ++i) m_Confirmed[i] = 0;

    m_PlayerCount    = config.player_count;
    m_HistoryLength  = config.history_length;
    m_MaxPrediction  = config.max_prediction;
    m_CurrentTick    = 0;
    m_ConfirmedFloor = 0;
    m_DirtyTick      = kNoDirtyTick;
    m_World          = world;
    return true;
}

void FRollbackSession::Reset(u32 start_tick) noexcept
{
    if (!IsInitialized()) return;
    m_History.InvalidateAll();
    for (usize i = 0; i < m_SlotTick.Size(); ++i) m_SlotTick[i] = kInvalidSlotTick;
    for (usize i = 0; i < m_Confirmed.Size(); ++i) m_Confirmed[i] = 0;
    m_CurrentTick    = start_tick;
    m_ConfirmedFloor = start_tick;
    m_DirtyTick      = kNoDirtyTick;
}

void FRollbackSession::EnsureSlot(u32 tick) noexcept
{
    const u32 idx = tick % m_HistoryLength;
    if (m_SlotTick[idx] == tick) return;
    m_SlotTick[idx] = tick;
    const u32 base = idx * m_PlayerCount;
    for (u32 p = 0; p < m_PlayerCount; ++p) m_Confirmed[base + p] = 0;
}

bool FRollbackSession::SubmitInput(const FInputFrame& frame) noexcept
{
    if (!IsInitialized()) return false;
    if (frame.player_id >= m_PlayerCount) return false;

    // 未来 tick は受理しない (呼び出し側トランスポートでバッファする契約)。
    // 台帳と状態履歴は同位相リングなので、tick T を先行受理すると同じ slot を
    // 共有する過去 tick T-H (まだ巻き戻し対象になり得る) の台帳を破壊してしまう。
    if (frame.tick > m_CurrentTick) return false;

    if (frame.tick == m_CurrentTick) {
        EnsureSlot(frame.tick);
        const u32 base = (frame.tick % m_HistoryLength) * m_PlayerCount;
        m_Ledger[base + frame.player_id]    = frame;
        m_Confirmed[base + frame.player_id] = 1;
        AdvanceConfirmedFloor();
        return true;
    }

    // 過去 tick: 実効巻き戻し窓は history_length - 1 tick。最古の 1 slot は
    // 「次に実行する現在 tick」と共有するため予約し、受理しない (受理すると
    // EnsureSlot(current) の relabel で台帳・履歴の整合が崩れる)。
    if (m_CurrentTick - frame.tick >= m_HistoryLength) return false;
    if (!m_History.HasFrame(frame.tick)) return false;

    const u32 base = (frame.tick % m_HistoryLength) * m_PlayerCount;
    m_Ledger[base + frame.player_id]    = frame;
    m_Confirmed[base + frame.player_id] = 1;

    // 予測と bit 一致なら結果は変わらないので巻き戻し不要 (GGPO と同じ省略)。
    if (!SameEffectiveInput(m_Used[base + frame.player_id], frame)) {
        if (frame.tick < m_DirtyTick) m_DirtyTick = frame.tick;
    }
    // getter (ConfirmedFloor / PredictionDepth) を次の AdvanceTick を待たず新鮮に保つ。
    AdvanceConfirmedFloor();
    return true;
}

void FRollbackSession::GatherInputs(u32 tick) noexcept
{
    const u32 idx  = tick % m_HistoryLength;
    const u32 base = idx * m_PlayerCount;
    for (u32 p = 0; p < m_PlayerCount; ++p) {
        FInputFrame f{};
        f.tick      = tick;
        f.player_id = p;
        if (m_Confirmed[base + p]) {
            const FInputFrame& c = m_Ledger[base + p];
            f.buttons = c.buttons;
            f.axis    = c.axis;
        } else if (tick > 0) {
            // 繰り返し予測: 直前 tick で実際に使った入力をそのまま使う。
            // 直前 slot が追い出し済み (リング一周) ならニュートラルのまま。
            const u32 pidx = (tick - 1u) % m_HistoryLength;
            if (m_SlotTick[pidx] == tick - 1u) {
                const FInputFrame& prev = m_Used[pidx * m_PlayerCount + p];
                f.buttons = prev.buttons;
                f.axis    = prev.axis;
            }
        }
        m_Used[base + p]  = f;
        m_TickInputs[p]   = f;
    }
}

void FRollbackSession::AdvanceConfirmedFloor() noexcept
{
    while (m_ConfirmedFloor < m_CurrentTick) {
        const u32 idx = m_ConfirmedFloor % m_HistoryLength;
        if (m_SlotTick[idx] != m_ConfirmedFloor) {
            // 追い出し済み tick は「確定を諦めた」ものとして床を進める
            // (SubmitInput 側でも既に受理不能なので、これ以上待つ意味がない)。
            ++m_ConfirmedFloor;
            continue;
        }
        const u32 base = idx * m_PlayerCount;
        bool all_confirmed = true;
        for (u32 p = 0; p < m_PlayerCount; ++p) {
            if (!m_Confirmed[base + p]) { all_confirmed = false; break; }
        }
        if (!all_confirmed) break;
        ++m_ConfirmedFloor;
    }
}

bool FRollbackSession::ResimulateIfNeeded() noexcept
{
    if (m_DirtyTick == kNoDirtyTick) return true;

    // dirty tick 開始時点の状態へ巻き戻す。SubmitInput が HasFrame で受理判定して
    // いるため、ここで失敗するのは OOM (CopyFrom 失敗) のみ。dirty は保持し、
    // 次の AdvanceTick で再試行できるようにする。
    if (!m_History.RestoreFrame(m_DirtyTick, *m_World)) return false;

    for (u32 t = m_DirtyTick; t < m_CurrentTick; ++t) {
        // 履歴も正しい系列で上書きする。失敗しても World は tick t 開始時点の
        // 一貫状態なので、dirty を t に更新して次回 t から再試行する。
        if (!m_History.SaveFrame(t, *m_World)) {
            m_DirtyTick = t;
            return false;
        }
        GatherInputs(t);
        m_SimFn(*m_World, t, m_TickInputs.Data(), m_PlayerCount, m_SimUser);
    }
    m_DirtyTick = kNoDirtyTick;
    return true;
}

bool FRollbackSession::AdvanceTick() noexcept
{
    if (!IsInitialized() || m_SimFn == nullptr) return false;

    // 予測上限: 未確定のまま先行しすぎたら入力が届くまで待つ (lockstep へ退化)。
    if (m_MaxPrediction != 0) {
        AdvanceConfirmedFloor();
        if (m_CurrentTick - m_ConfirmedFloor >= m_MaxPrediction) return false;
    }

    if (!ResimulateIfNeeded()) return false;

    EnsureSlot(m_CurrentTick);
    if (!m_History.SaveFrame(m_CurrentTick, *m_World)) return false;
    GatherInputs(m_CurrentTick);
    m_SimFn(*m_World, m_CurrentTick, m_TickInputs.Data(), m_PlayerCount, m_SimUser);
    ++m_CurrentTick;
    AdvanceConfirmedFloor();
    return true;
}

} // namespace acs::game
