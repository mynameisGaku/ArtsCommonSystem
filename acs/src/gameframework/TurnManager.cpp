// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット — FTurnManager 実装
//
// マルチサイドターン制マネージャの完全実装。
//
// 実装メモ:
//  ・slot 再利用は線形走査 (FCooldownTimer と同方針)。side 数はせいぜい数〜十数
//    程度なので O(N) で十分。
//  ・generation は u8 (0=未使用、1〜255 が有効)。255 で wrap して 1 に戻す
//    (0 にすると IsValid が常に false になり stale 検知不能になる)。
//  ・m_TurnOrder は slot index の配列。RebuildTurnOrder は insertion sort
//    で initiative 降順 + 同値は AddSide 順 (= slot index 昇順) の安定ソート。
//    side 数が小さいので insertion sort で十分高速。
//  ・EndCurrentTurn 連鎖: EndOfRound → RoundEndCallback → StartRound の流れは
//    再入しない (StartRound 内では callback 経由で AddSide/RemoveSide が呼ばれ
//    ても m_TurnOrder を都度確認する作りにしている)。
//  ・進行中の RemoveSide: 削除対象が current 側なら EndCurrentTurn 相当に
//    フォールスルーする。
#include "gameframework/TurnManager.h"

namespace acs::game {

/** side 名が "Env" 接頭辞を持つ環境側かどうかを判定する。 */
bool FTurnManager::IsEnvironmentName(const char* name) noexcept {
    if (name == nullptr) return false;
    // strncmp 相当を手書き (STL 禁止 / <cstring> も避け、依存最小化)
    if (name[0] != 'E') return false;
    if (name[1] != 'n') return false;
    if (name[2] != 'v') return false;
    return true;
}

/** side の属性からターン phase (Player / Environment / Enemy) を分類する。 */
ETurnPhase FTurnManager::ClassifyPhase(const SideSlot& s) noexcept {
    if (s.view.is_player_controlled) return ETurnPhase::PlayerTurn;
    if (IsEnvironmentName(s.view.display_name)) return ETurnPhase::EnvironmentTurn;
    return ETurnPhase::EnemyTurn;
}

/** 空き slot index を確保する (inactive を再利用、無ければ末尾追加)。 */
u32 FTurnManager::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用
    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        if (!m_Slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    // 全 slot 使用中 → 末尾に追加。24bit index 上限を守る。
    if (n >= static_cast<usize>(FTurnSideId::kMaxIndex)) {
        return FTurnSideId::kMaxIndex; // sentinel
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** handle を生存中の slot へ解決する (stale なら nullptr)。 */
FTurnManager::SideSlot* FTurnManager::Resolve(FTurnSideId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return nullptr;
    SideSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

/** handle を生存中の slot へ解決する const 版 (stale なら nullptr)。 */
const FTurnManager::SideSlot* FTurnManager::Resolve(FTurnSideId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return nullptr;
    const SideSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

/** 全 slot を inactive 化し、ラウンド状態を Setup にリセットする (callback は保持)。 */
void FTurnManager::Init() noexcept {
    // 全 slot を inactive 化 (gen は維持 = 次 Acquire で +1 される)
    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        SideSlot& s = m_Slots[i];
        s.active                     = false;
        s.view.display_name          = nullptr;
        s.view.max_action_points     = 0u;
        s.view.current_action_points = 0u;
        s.view.initiative            = 0u;
        s.view.is_player_controlled  = false;
        s.view.has_acted             = false;
        // gen はそのまま
    }
    m_TurnOrder.Clear();
    m_ActiveCount        = 0u;
    m_Round               = 0u;
    m_Phase               = ETurnPhase::Setup;
    m_CurrentOrderIndex = kInvalidOrderIndex;
    // callback は保持 (Init は再 enter 用)。
}

/** Init に加えて登録済みコールバックも全てクリアする。 */
void FTurnManager::ClearAll() noexcept {
    Init();
    m_OnTurnStart      = nullptr;
    m_OnTurnStartUser = nullptr;
    m_OnRoundEnd       = nullptr;
    m_OnRoundEndUser  = nullptr;
}

/** side を登録し、generation 付き handle を返す。 */
FTurnSideId FTurnManager::AddSide(const char* display_name, u32 max_ap, u32 initiative,
                                bool is_player_controlled) noexcept {
    if (display_name == nullptr) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FTurnSideId::kMaxIndex) return {}; // 上限到達

    SideSlot& s = m_Slots[idx];

    // generation を 1 進める (0 は未使用扱いなので必ず 1 以上を保つ)
    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.view.display_name      = display_name;
    s.view.max_action_points = max_ap;
    // ラウンド進行中に追加された side は今ラウンドには参加しない (has_acted=true)。
    // Setup 中 (phase==Setup) なら通常通り未行動 (has_acted=false)。
    // ※ StartRound 時に has_acted は false に refill されるのでどちらでも次ラウンドからは参加可能。
    const bool mid_round = (m_Phase != ETurnPhase::Setup);
    s.view.current_action_points = mid_round ? 0u : max_ap;
    s.view.initiative            = initiative;
    s.view.is_player_controlled  = is_player_controlled;
    s.view.has_acted             = mid_round; // mid-round 追加は今ラウンドではスキップ
    s.active                     = true;
    s.gen                        = new_gen;

    ++m_ActiveCount;
    return FTurnSideId::Pack(idx, new_gen);
}

/** side を解除し、必要なら current ターン / turn order を追従調整する。 */
void FTurnManager::RemoveSide(FTurnSideId id) noexcept {
    SideSlot* s = Resolve(id);
    if (s == nullptr) return;

    // 現 turn side を削除する場合は EndCurrentTurn 相当の流れに乗せる必要があるが、
    // まずは slot を deactivate して m_TurnOrder から index を取り除く。
    const u32 removed_index = id.Index();

    // 現 turn が自身かどうかを判定
    bool was_current = false;
    if (m_CurrentOrderIndex != kInvalidOrderIndex &&
        m_CurrentOrderIndex < m_TurnOrder.Size() &&
        m_TurnOrder[m_CurrentOrderIndex] == removed_index) {
        was_current = true;
    }

    // slot を deactivate
    s->active                     = false;
    s->view.display_name          = nullptr;
    s->view.max_action_points     = 0u;
    s->view.current_action_points = 0u;
    s->view.initiative            = 0u;
    s->view.is_player_controlled  = false;
    s->view.has_acted             = false;
    // gen は維持 (次 Acquire で +1)

    if (m_ActiveCount > 0u) --m_ActiveCount;

    // m_TurnOrder から removed_index を除去 (順序保持のため線形 erase)。
    // RemoveAtSwap を使うと initiative 順が崩れるので index 単位で前詰めする。
    const usize order_n = m_TurnOrder.Size();
    usize found_at = order_n;
    for (usize i = 0; i < order_n; ++i) {
        if (m_TurnOrder[i] == removed_index) {
            found_at = i;
            break;
        }
    }
    if (found_at < order_n) {
        for (usize i = found_at; i + 1u < order_n; ++i) {
            m_TurnOrder[i] = m_TurnOrder[i + 1u];
        }
        m_TurnOrder.Resize(order_n - 1u);

        // 現 turn が後ろにずれていれば追従。削除位置より前の current は影響なし。
        if (was_current) {
            // 現 turn を削除した場合、その order index はそのまま「次の actor」を
            // 指す位置になる (前詰めで後続が来た) ので、新 current を探索しなおす。
            // ただし found_at が末尾だった場合は kInvalidOrderIndex 相当。
            const u32 start_from = static_cast<u32>(found_at);
            // 全 side が has_acted=true で打ち止まりなら EndOfRound → StartRound 連鎖。
            // AdvanceToNextActor 内で kInvalidOrderIndex が立つので、その後分岐する。
            AdvanceToNextActor(start_from);
            if (m_CurrentOrderIndex == kInvalidOrderIndex) {
                // ラウンド終了 → 次ラウンド開始
                m_Phase = ETurnPhase::EndOfRound;
                if (m_OnRoundEnd != nullptr) {
                    m_OnRoundEnd(m_OnRoundEndUser, m_Round);
                }
                // side が残っていなければ Setup に戻る
                if (m_ActiveCount == 0u) {
                    m_Phase = ETurnPhase::Setup;
                    return;
                }
                StartRound();
            } else {
                // 新 current の phase / callback を発火
                const u32 new_slot_idx = m_TurnOrder[m_CurrentOrderIndex];
                SideSlot& ns = m_Slots[new_slot_idx];
                m_Phase = ClassifyPhase(ns);
                if (m_OnTurnStart != nullptr) {
                    const u8 gen = ns.gen;
                    const FTurnSideId new_id = FTurnSideId::Pack(new_slot_idx, gen);
                    m_OnTurnStart(m_OnTurnStartUser, new_id, m_Round);
                }
            }
        } else if (m_CurrentOrderIndex != kInvalidOrderIndex &&
                   found_at < static_cast<usize>(m_CurrentOrderIndex)) {
            // 削除位置より前 → current の位置を 1 つ前にずらす
            m_CurrentOrderIndex -= 1u;
        }
    }

    // active side が 0 になったら Setup へ戻す (callback 後でも安全)
    if (m_ActiveCount == 0u) {
        m_Phase               = ETurnPhase::Setup;
        m_CurrentOrderIndex = kInvalidOrderIndex;
        m_TurnOrder.Clear();
    }
}

/** active side を initiative 降順 (同値は AddSide 順) で turn order に並べ直す。 */
void FTurnManager::RebuildTurnOrder() noexcept {
    m_TurnOrder.Clear();
    const usize n = m_Slots.Size();
    m_TurnOrder.Reserve(n);

    // active な slot index を AddSide 順 (= slot index 昇順) で列挙
    for (usize i = 0; i < n; ++i) {
        if (m_Slots[i].active) {
            m_TurnOrder.PushBack(static_cast<u32>(i));
        }
    }

    // insertion sort: initiative 降順、同値は slot index 昇順 (安定)
    // m_TurnOrder が既に slot index 昇順なので、initiative 降順だけで安定性確保。
    const usize order_n = m_TurnOrder.Size();
    for (usize i = 1; i < order_n; ++i) {
        const u32 cur_idx = m_TurnOrder[i];
        const u32 cur_init = m_Slots[cur_idx].view.initiative;
        usize j = i;
        while (j > 0) {
            const u32 prev_idx = m_TurnOrder[j - 1u];
            const u32 prev_init = m_Slots[prev_idx].view.initiative;
            // 降順: prev_init < cur_init なら入れ替え。同値なら入れ替えない (安定)。
            if (prev_init < cur_init) {
                m_TurnOrder[j] = m_TurnOrder[j - 1u];
                --j;
            } else {
                break;
            }
        }
        m_TurnOrder[j] = cur_idx;
    }
}

/** start_from 以降で最初の未行動 active side を探し m_CurrentOrderIndex に設定する。 */
void FTurnManager::AdvanceToNextActor(u32 start_from) noexcept {
    const u32 order_n = static_cast<u32>(m_TurnOrder.Size());
    for (u32 i = start_from; i < order_n; ++i) {
        const u32 slot_idx = m_TurnOrder[i];
        if (slot_idx >= m_Slots.Size()) continue;
        const SideSlot& s = m_Slots[slot_idx];
        if (!s.active) continue;
        if (s.view.has_acted) continue;
        m_CurrentOrderIndex = i;
        return;
    }
    m_CurrentOrderIndex = kInvalidOrderIndex;
}

/** 全 side の AP を refill して turn order を再構築し、先頭 side のターンを開始する。 */
void FTurnManager::StartRound() noexcept {
    if (m_ActiveCount == 0u) {
        // side 未登録 → no-op (phase は Setup のまま)
        return;
    }

    // 全 active side の AP refill + has_acted リセット
    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        SideSlot& s = m_Slots[i];
        if (!s.active) continue;
        s.view.current_action_points = s.view.max_action_points;
        s.view.has_acted             = false;
    }

    // turn order を initiative 順で再構築
    RebuildTurnOrder();

    ++m_Round;

    // 先頭 side を current に
    AdvanceToNextActor(0u);
    if (m_CurrentOrderIndex == kInvalidOrderIndex) {
        // 例外: active_count > 0 だが turn_order に未行動 side がいない (= 不整合)
        // 防御的に Setup に戻す。
        m_Phase = ETurnPhase::Setup;
        return;
    }

    const u32 slot_idx = m_TurnOrder[m_CurrentOrderIndex];
    SideSlot& cs = m_Slots[slot_idx];
    m_Phase = ClassifyPhase(cs);

    if (m_OnTurnStart != nullptr) {
        const FTurnSideId id = FTurnSideId::Pack(slot_idx, cs.gen);
        m_OnTurnStart(m_OnTurnStartUser, id, m_Round);
    }
}

/** 現 side を行動済にし、次 actor へ進む (全員完了なら EndOfRound→次ラウンド連鎖)。 */
void FTurnManager::EndCurrentTurn() noexcept {
    // Setup / EndOfRound 中の呼び出しは no-op
    if (m_Phase == ETurnPhase::Setup || m_Phase == ETurnPhase::EndOfRound) return;
    if (m_CurrentOrderIndex == kInvalidOrderIndex) return;
    if (m_CurrentOrderIndex >= m_TurnOrder.Size()) return;

    // 現 side を行動済に
    const u32 cur_slot_idx = m_TurnOrder[m_CurrentOrderIndex];
    if (cur_slot_idx < m_Slots.Size()) {
        SideSlot& cs = m_Slots[cur_slot_idx];
        if (cs.active) {
            cs.view.has_acted = true;
        }
    }

    // 次の未行動 side を探す
    AdvanceToNextActor(m_CurrentOrderIndex + 1u);
    if (m_CurrentOrderIndex != kInvalidOrderIndex) {
        // 次 side のターンへ
        const u32 next_slot_idx = m_TurnOrder[m_CurrentOrderIndex];
        SideSlot& ns = m_Slots[next_slot_idx];
        m_Phase = ClassifyPhase(ns);

        if (m_OnTurnStart != nullptr) {
            const FTurnSideId id = FTurnSideId::Pack(next_slot_idx, ns.gen);
            m_OnTurnStart(m_OnTurnStartUser, id, m_Round);
        }
        return;
    }

    // 全 side 行動完了 → EndOfRound → StartRound 連鎖
    m_Phase = ETurnPhase::EndOfRound;
    const u32 ending_round = m_Round;
    if (m_OnRoundEnd != nullptr) {
        m_OnRoundEnd(m_OnRoundEndUser, ending_round);
    }

    // callback 内で ClearAll / RemoveSide により side が 0 になった可能性に備える
    if (m_ActiveCount == 0u) {
        m_Phase               = ETurnPhase::Setup;
        m_CurrentOrderIndex = kInvalidOrderIndex;
        m_TurnOrder.Clear();
        return;
    }

    StartRound();
}

/** 現 side の AP を amount だけ消費する (不足や非進行中なら false)。 */
bool FTurnManager::TryConsumeAP(u32 amount) noexcept {
    if (amount == 0u) return false;
    if (m_Phase == ETurnPhase::Setup || m_Phase == ETurnPhase::EndOfRound) return false;
    if (m_CurrentOrderIndex == kInvalidOrderIndex) return false;
    if (m_CurrentOrderIndex >= m_TurnOrder.Size()) return false;

    const u32 slot_idx = m_TurnOrder[m_CurrentOrderIndex];
    if (slot_idx >= m_Slots.Size()) return false;

    SideSlot& s = m_Slots[slot_idx];
    if (!s.active) return false;
    if (s.view.current_action_points < amount) return false;

    s.view.current_action_points -= amount;
    return true;
}

/** 現在ターン中の side の handle を返す (非進行中なら invalid)。 */
FTurnSideId FTurnManager::CurrentTurnSide() const noexcept {
    if (m_Phase == ETurnPhase::Setup || m_Phase == ETurnPhase::EndOfRound) return {};
    if (m_CurrentOrderIndex == kInvalidOrderIndex) return {};
    if (m_CurrentOrderIndex >= m_TurnOrder.Size()) return {};

    const u32 slot_idx = m_TurnOrder[m_CurrentOrderIndex];
    if (slot_idx >= m_Slots.Size()) return {};

    const SideSlot& s = m_Slots[slot_idx];
    if (!s.active) return {};

    return FTurnSideId::Pack(slot_idx, s.gen);
}

/** handle から side の公開状態 (FTurnSideState) を取得する (stale なら nullptr)。 */
const FTurnSideState* FTurnManager::GetSideState(FTurnSideId id) const noexcept {
    const SideSlot* s = Resolve(id);
    if (s == nullptr) return nullptr;
    // SideSlot 内に FTurnSideState を埋め込んでいるため、そのアドレスを返すだけ。
    return &s->view;
}

/** 今ラウンドでまだ行動していない active side の数を返す。 */
u32 FTurnManager::TurnsRemainingThisRound() const noexcept {
    if (m_Phase == ETurnPhase::Setup || m_Phase == ETurnPhase::EndOfRound) return 0u;

    u32 count = 0u;
    const usize order_n = m_TurnOrder.Size();
    for (usize i = 0; i < order_n; ++i) {
        const u32 slot_idx = m_TurnOrder[i];
        if (slot_idx >= m_Slots.Size()) continue;
        const SideSlot& s = m_Slots[slot_idx];
        if (!s.active) continue;
        if (!s.view.has_acted) ++count;
    }
    return count;
}

} // namespace acs::game
