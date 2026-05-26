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
//  ・_turn_order は slot index の配列。RebuildTurnOrder は insertion sort
//    で initiative 降順 + 同値は AddSide 順 (= slot index 昇順) の安定ソート。
//    side 数が小さいので insertion sort で十分高速。
//  ・EndCurrentTurn 連鎖: EndOfRound → RoundEndCallback → StartRound の流れは
//    再入しない (StartRound 内では callback 経由で AddSide/RemoveSide が呼ばれ
//    ても _turn_order を都度確認する作りにしている)。
//  ・進行中の RemoveSide: 削除対象が current 側なら EndCurrentTurn 相当に
//    フォールスルーする。
#include "gameframework/TurnManager.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

bool FTurnManager::IsEnvironmentName(const char* name) noexcept {
    if (name == nullptr) return false;
    // strncmp 相当を手書き (STL 禁止 / <cstring> も避け、依存最小化)
    if (name[0] != 'E') return false;
    if (name[1] != 'n') return false;
    if (name[2] != 'v') return false;
    return true;
}

ETurnPhase FTurnManager::ClassifyPhase(const FSideSlot& s) noexcept {
    if (s.view.is_player_controlled) return ETurnPhase::PlayerTurn;
    if (IsEnvironmentName(s.view.display_name)) return ETurnPhase::EnvironmentTurn;
    return ETurnPhase::EnemyTurn;
}

u32 FTurnManager::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        if (!_slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    // 全 slot 使用中 → 末尾に追加。24bit index 上限を守る。
    if (n >= static_cast<usize>(FTurnSideId::kMaxIndex)) {
        return FTurnSideId::kMaxIndex; // sentinel
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

FTurnManager::FSideSlot* FTurnManager::Resolve(FTurnSideId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    FSideSlot& s = _slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

const FTurnManager::FSideSlot* FTurnManager::Resolve(FTurnSideId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    const FSideSlot& s = _slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

// ----------------------------------------------------------------------------
// init / clear
// ----------------------------------------------------------------------------

void FTurnManager::Init() noexcept {
    // 全 slot を inactive 化 (gen は維持 = 次 Acquire で +1 される)
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        FSideSlot& s = _slots[i];
        s.active                     = false;
        s.view.display_name          = nullptr;
        s.view.max_action_points     = 0u;
        s.view.current_action_points = 0u;
        s.view.initiative            = 0u;
        s.view.is_player_controlled  = false;
        s.view.has_acted             = false;
        // gen はそのまま
    }
    _turn_order.Clear();
    _active_count        = 0u;
    _round               = 0u;
    _phase               = ETurnPhase::Setup;
    _current_order_index = kInvalidOrderIndex;
    // callback は保持 (Init は再 enter 用)。
}

void FTurnManager::ClearAll() noexcept {
    Init();
    _on_turn_start      = nullptr;
    _on_turn_start_user = nullptr;
    _on_round_end       = nullptr;
    _on_round_end_user  = nullptr;
}

// ----------------------------------------------------------------------------
// side 登録 / 解除
// ----------------------------------------------------------------------------

FTurnSideId FTurnManager::AddSide(const char* display_name, u32 max_ap, u32 initiative,
                                bool is_player_controlled) noexcept {
    if (display_name == nullptr) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FTurnSideId::kMaxIndex) return {}; // 上限到達

    FSideSlot& s = _slots[idx];

    // generation を 1 進める (0 は未使用扱いなので必ず 1 以上を保つ)
    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.view.display_name      = display_name;
    s.view.max_action_points = max_ap;
    // ラウンド進行中に追加された side は今ラウンドには参加しない (has_acted=true)。
    // Setup 中 (phase==Setup) なら通常通り未行動 (has_acted=false)。
    // ※ StartRound 時に has_acted は false に refill されるのでどちらでも次ラウンドからは参加可能。
    const bool mid_round = (_phase != ETurnPhase::Setup);
    s.view.current_action_points = mid_round ? 0u : max_ap;
    s.view.initiative            = initiative;
    s.view.is_player_controlled  = is_player_controlled;
    s.view.has_acted             = mid_round; // mid-round 追加は今ラウンドではスキップ
    s.active                     = true;
    s.gen                        = new_gen;

    ++_active_count;
    return FTurnSideId::Pack(idx, new_gen);
}

void FTurnManager::RemoveSide(FTurnSideId id) noexcept {
    FSideSlot* s = Resolve(id);
    if (s == nullptr) return;

    // 現 turn side を削除する場合は EndCurrentTurn 相当の流れに乗せる必要があるが、
    // まずは slot を deactivate して _turn_order から index を取り除く。
    const u32 removed_index = id.Index();

    // 現 turn が自身かどうかを判定
    bool was_current = false;
    if (_current_order_index != kInvalidOrderIndex &&
        _current_order_index < _turn_order.Size() &&
        _turn_order[_current_order_index] == removed_index) {
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

    if (_active_count > 0u) --_active_count;

    // _turn_order から removed_index を除去 (順序保持のため線形 erase)。
    // RemoveAtSwap を使うと initiative 順が崩れるので index 単位で前詰めする。
    const usize order_n = _turn_order.Size();
    usize found_at = order_n;
    for (usize i = 0; i < order_n; ++i) {
        if (_turn_order[i] == removed_index) {
            found_at = i;
            break;
        }
    }
    if (found_at < order_n) {
        for (usize i = found_at; i + 1u < order_n; ++i) {
            _turn_order[i] = _turn_order[i + 1u];
        }
        _turn_order.Resize(order_n - 1u);

        // 現 turn が後ろにずれていれば追従。削除位置より前の current は影響なし。
        if (was_current) {
            // 現 turn を削除した場合、その order index はそのまま「次の actor」を
            // 指す位置になる (前詰めで後続が来た) ので、新 current を探索しなおす。
            // ただし found_at が末尾だった場合は kInvalidOrderIndex 相当。
            const u32 start_from = static_cast<u32>(found_at);
            // 全 side が has_acted=true で打ち止まりなら EndOfRound → StartRound 連鎖。
            // AdvanceToNextActor 内で kInvalidOrderIndex が立つので、その後分岐する。
            AdvanceToNextActor(start_from);
            if (_current_order_index == kInvalidOrderIndex) {
                // ラウンド終了 → 次ラウンド開始
                _phase = ETurnPhase::EndOfRound;
                if (_on_round_end != nullptr) {
                    _on_round_end(_on_round_end_user, _round);
                }
                // side が残っていなければ Setup に戻る
                if (_active_count == 0u) {
                    _phase = ETurnPhase::Setup;
                    return;
                }
                StartRound();
            } else {
                // 新 current の phase / callback を発火
                const u32 new_slot_idx = _turn_order[_current_order_index];
                FSideSlot& ns = _slots[new_slot_idx];
                _phase = ClassifyPhase(ns);
                if (_on_turn_start != nullptr) {
                    const u8 gen = ns.gen;
                    const FTurnSideId new_id = FTurnSideId::Pack(new_slot_idx, gen);
                    _on_turn_start(_on_turn_start_user, new_id, _round);
                }
            }
        } else if (_current_order_index != kInvalidOrderIndex &&
                   found_at < static_cast<usize>(_current_order_index)) {
            // 削除位置より前 → current の位置を 1 つ前にずらす
            _current_order_index -= 1u;
        }
    }

    // active side が 0 になったら Setup へ戻す (callback 後でも安全)
    if (_active_count == 0u) {
        _phase               = ETurnPhase::Setup;
        _current_order_index = kInvalidOrderIndex;
        _turn_order.Clear();
    }
}

// ----------------------------------------------------------------------------
// turn order 構築
// ----------------------------------------------------------------------------

void FTurnManager::RebuildTurnOrder() noexcept {
    _turn_order.Clear();
    const usize n = _slots.Size();
    _turn_order.Reserve(n);

    // active な slot index を AddSide 順 (= slot index 昇順) で列挙
    for (usize i = 0; i < n; ++i) {
        if (_slots[i].active) {
            _turn_order.PushBack(static_cast<u32>(i));
        }
    }

    // insertion sort: initiative 降順、同値は slot index 昇順 (安定)
    // _turn_order が既に slot index 昇順なので、initiative 降順だけで安定性確保。
    const usize order_n = _turn_order.Size();
    for (usize i = 1; i < order_n; ++i) {
        const u32 cur_idx = _turn_order[i];
        const u32 cur_init = _slots[cur_idx].view.initiative;
        usize j = i;
        while (j > 0) {
            const u32 prev_idx = _turn_order[j - 1u];
            const u32 prev_init = _slots[prev_idx].view.initiative;
            // 降順: prev_init < cur_init なら入れ替え。同値なら入れ替えない (安定)。
            if (prev_init < cur_init) {
                _turn_order[j] = _turn_order[j - 1u];
                --j;
            } else {
                break;
            }
        }
        _turn_order[j] = cur_idx;
    }
}

void FTurnManager::AdvanceToNextActor(u32 start_from) noexcept {
    const u32 order_n = static_cast<u32>(_turn_order.Size());
    for (u32 i = start_from; i < order_n; ++i) {
        const u32 slot_idx = _turn_order[i];
        if (slot_idx >= _slots.Size()) continue;
        const FSideSlot& s = _slots[slot_idx];
        if (!s.active) continue;
        if (s.view.has_acted) continue;
        _current_order_index = i;
        return;
    }
    _current_order_index = kInvalidOrderIndex;
}

// ----------------------------------------------------------------------------
// ラウンド進行
// ----------------------------------------------------------------------------

void FTurnManager::StartRound() noexcept {
    if (_active_count == 0u) {
        // side 未登録 → no-op (phase は Setup のまま)
        return;
    }

    // 全 active side の AP refill + has_acted リセット
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        FSideSlot& s = _slots[i];
        if (!s.active) continue;
        s.view.current_action_points = s.view.max_action_points;
        s.view.has_acted             = false;
    }

    // turn order を initiative 順で再構築
    RebuildTurnOrder();

    ++_round;

    // 先頭 side を current に
    AdvanceToNextActor(0u);
    if (_current_order_index == kInvalidOrderIndex) {
        // 例外: active_count > 0 だが turn_order に未行動 side がいない (= 不整合)
        // 防御的に Setup に戻す。
        _phase = ETurnPhase::Setup;
        return;
    }

    const u32 slot_idx = _turn_order[_current_order_index];
    FSideSlot& cs = _slots[slot_idx];
    _phase = ClassifyPhase(cs);

    if (_on_turn_start != nullptr) {
        const FTurnSideId id = FTurnSideId::Pack(slot_idx, cs.gen);
        _on_turn_start(_on_turn_start_user, id, _round);
    }
}

void FTurnManager::EndCurrentTurn() noexcept {
    // Setup / EndOfRound 中の呼び出しは no-op
    if (_phase == ETurnPhase::Setup || _phase == ETurnPhase::EndOfRound) return;
    if (_current_order_index == kInvalidOrderIndex) return;
    if (_current_order_index >= _turn_order.Size()) return;

    // 現 side を行動済に
    const u32 cur_slot_idx = _turn_order[_current_order_index];
    if (cur_slot_idx < _slots.Size()) {
        FSideSlot& cs = _slots[cur_slot_idx];
        if (cs.active) {
            cs.view.has_acted = true;
        }
    }

    // 次の未行動 side を探す
    AdvanceToNextActor(_current_order_index + 1u);
    if (_current_order_index != kInvalidOrderIndex) {
        // 次 side のターンへ
        const u32 next_slot_idx = _turn_order[_current_order_index];
        FSideSlot& ns = _slots[next_slot_idx];
        _phase = ClassifyPhase(ns);

        if (_on_turn_start != nullptr) {
            const FTurnSideId id = FTurnSideId::Pack(next_slot_idx, ns.gen);
            _on_turn_start(_on_turn_start_user, id, _round);
        }
        return;
    }

    // 全 side 行動完了 → EndOfRound → StartRound 連鎖
    _phase = ETurnPhase::EndOfRound;
    const u32 ending_round = _round;
    if (_on_round_end != nullptr) {
        _on_round_end(_on_round_end_user, ending_round);
    }

    // callback 内で ClearAll / RemoveSide により side が 0 になった可能性に備える
    if (_active_count == 0u) {
        _phase               = ETurnPhase::Setup;
        _current_order_index = kInvalidOrderIndex;
        _turn_order.Clear();
        return;
    }

    StartRound();
}

// ----------------------------------------------------------------------------
// AP 消費
// ----------------------------------------------------------------------------

bool FTurnManager::TryConsumeAP(u32 amount) noexcept {
    if (amount == 0u) return false;
    if (_phase == ETurnPhase::Setup || _phase == ETurnPhase::EndOfRound) return false;
    if (_current_order_index == kInvalidOrderIndex) return false;
    if (_current_order_index >= _turn_order.Size()) return false;

    const u32 slot_idx = _turn_order[_current_order_index];
    if (slot_idx >= _slots.Size()) return false;

    FSideSlot& s = _slots[slot_idx];
    if (!s.active) return false;
    if (s.view.current_action_points < amount) return false;

    s.view.current_action_points -= amount;
    return true;
}

// ----------------------------------------------------------------------------
// 問い合わせ
// ----------------------------------------------------------------------------

FTurnSideId FTurnManager::CurrentTurnSide() const noexcept {
    if (_phase == ETurnPhase::Setup || _phase == ETurnPhase::EndOfRound) return {};
    if (_current_order_index == kInvalidOrderIndex) return {};
    if (_current_order_index >= _turn_order.Size()) return {};

    const u32 slot_idx = _turn_order[_current_order_index];
    if (slot_idx >= _slots.Size()) return {};

    const FSideSlot& s = _slots[slot_idx];
    if (!s.active) return {};

    return FTurnSideId::Pack(slot_idx, s.gen);
}

const FTurnSideState* FTurnManager::GetSideState(FTurnSideId id) const noexcept {
    const FSideSlot* s = Resolve(id);
    if (s == nullptr) return nullptr;
    // FSideSlot 内に FTurnSideState を埋め込んでいるため、そのアドレスを返すだけ。
    return &s->view;
}

u32 FTurnManager::TurnsRemainingThisRound() const noexcept {
    if (_phase == ETurnPhase::Setup || _phase == ETurnPhase::EndOfRound) return 0u;

    u32 count = 0u;
    const usize order_n = _turn_order.Size();
    for (usize i = 0; i < order_n; ++i) {
        const u32 slot_idx = _turn_order[i];
        if (slot_idx >= _slots.Size()) continue;
        const FSideSlot& s = _slots[slot_idx];
        if (!s.active) continue;
        if (!s.view.has_acted) ++count;
    }
    return count;
}

} // namespace acs::game
