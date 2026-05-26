// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 3 — TriggerWorld2D 実装
#include "gameframework/TriggerWorld2D.h"
#include "foundation/Move.h"

namespace acs::game {

// ===== Slot 管理 =====

u32 TriggerWorld2D::AcquireSlot() noexcept {
    // index 0 を invalid 用に予約 (TriggerId == 0 == invalid と一致させる)
    for (u32 i = 1; i < _slots.Size(); ++i) {
        if (!_slots[i].active) return i;
    }
    if (_slots.IsEmpty()) {
        _slots.PushBack({});   // dummy at index 0
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

void TriggerWorld2D::Init() noexcept {
    // Phase 3 では特に何もしない。将来 SpatialGrid を追加した時に cell_size 等を持つ。
}

TriggerId TriggerWorld2D::AddCircle(const Circle& c, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    TriggerSlot& s = _slots[idx];
    s.kind   = Kind::Circle;
    s.circle = c;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;   // generation 0 は invalid 予約のためスキップ
    s.active = true;
    ++_trigger_count;
    return TriggerId{idx, s.gen};
}

TriggerId TriggerWorld2D::AddAabb(const Aabb2& a, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    TriggerSlot& s = _slots[idx];
    s.kind   = Kind::FAabb;
    s.aabb   = a;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++_trigger_count;
    return TriggerId{idx, s.gen};
}

void TriggerWorld2D::UpdateCircle(TriggerId id, const Circle& c) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    TriggerSlot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Circle) return;
    s.circle = c;
}

void TriggerWorld2D::UpdateAabb(TriggerId id, const Aabb2& a) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    TriggerSlot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::FAabb) return;
    s.aabb = a;
}

void TriggerWorld2D::Remove(TriggerId id) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    TriggerSlot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    s.kind   = Kind::None;
    if (_trigger_count > 0) --_trigger_count;
    // 関連 pair は次 Tick で OnExit 発火後に自然消滅する (next_pairs に乗らないため)
}

void TriggerWorld2D::SetOnEnter(TriggerEventCallback cb, void* user) noexcept {
    _on_enter      = cb;
    _on_enter_user = user;
}

void TriggerWorld2D::SetOnStay(TriggerEventCallback cb, void* user) noexcept {
    _on_stay      = cb;
    _on_stay_user = user;
}

void TriggerWorld2D::SetOnExit(TriggerEventCallback cb, void* user) noexcept {
    _on_exit      = cb;
    _on_exit_user = user;
}

void TriggerWorld2D::ClearAll() noexcept {
    _slots.Clear();
    _pairs.Clear();
    _next_pairs.Clear();
    _trigger_count = 0;
}

// ===== Narrow phase: 任意の 2 trigger の overlap 判定 =====
bool TriggerWorld2D::ShapesOverlap(const TriggerSlot& a, const TriggerSlot& b) const noexcept {
    if (a.kind == Kind::FAabb && b.kind == Kind::FAabb)     return Intersect(a.aabb,   b.aabb);
    if (a.kind == Kind::Circle && b.kind == Kind::Circle) return Intersect(a.circle, b.circle);
    if (a.kind == Kind::FAabb && b.kind == Kind::Circle)   return Intersect(a.aabb,   b.circle);
    if (a.kind == Kind::Circle && b.kind == Kind::FAabb)   return Intersect(b.aabb,   a.circle);
    return false;
}

// ===== Tick: overlap pair 全比較 + イベント発火 =====
void TriggerWorld2D::Tick(f32 /*dt*/) noexcept {
    // 1. 今フレの overlap pair を全 O(N^2) ペアで再計算し _next_pairs に格納。
    //    (a, b) は a < b で正規化。active な slot のみを対象。
    _next_pairs.Clear();
    const u32 slot_count = static_cast<u32>(_slots.Size());
    for (u32 i = 1; i < slot_count; ++i) {                  // 0 は invalid 予約
        const TriggerSlot& sa = _slots[i];
        if (!sa.active) continue;
        for (u32 j = i + 1; j < slot_count; ++j) {
            const TriggerSlot& sb = _slots[j];
            if (!sb.active) continue;
            if (!ShapesOverlap(sa, sb)) continue;
            OverlapPair np;
            np.a_idx           = i;
            np.b_idx           = j;
            np.was_overlapping = true;   // 「今フレ overlap している」マーカ
            _next_pairs.PushBack(np);
        }
    }
    // 走査順 (i 昇順、j > i) により _next_pairs は (a_idx, b_idx) 辞書順でソート済み。

    // 2. 前フレ _pairs と今フレ _next_pairs を辞書順マージで突き合わせ:
    //    ・両方にある  → OnStay
    //    ・前のみ      → OnExit
    //    ・今のみ      → OnEnter
    u32 ip = 0;                                  // 前フレ pairs index
    u32 in = 0;                                  // 今フレ next_pairs index
    const u32 np = static_cast<u32>(_pairs.Size());
    const u32 nn = static_cast<u32>(_next_pairs.Size());

    while (ip < np && in < nn) {
        const OverlapPair& p = _pairs[ip];
        const OverlapPair& n = _next_pairs[in];
        // (a, b) 辞書順で比較
        const bool less_p = (p.a_idx <  n.a_idx) ||
                           (p.a_idx == n.a_idx && p.b_idx <  n.b_idx);
        const bool less_n = (n.a_idx <  p.a_idx) ||
                           (n.a_idx == p.a_idx && n.b_idx <  p.b_idx);
        if (!less_p && !less_n) {
            // 同じ pair → 前フレ overlap で今フレも overlap → OnStay
            if (_on_stay) {
                const TriggerSlot& sa = _slots[p.a_idx];
                const TriggerSlot& sb = _slots[p.b_idx];
                _on_stay(_on_stay_user,
                         TriggerId{p.a_idx, sa.gen},
                         TriggerId{p.b_idx, sb.gen});
            }
            ++ip; ++in;
        } else if (less_p) {
            // 前のみ → 離れた → OnExit
            // Remove で消えた slot が含まれる可能性があるため、active 検証して
            // generation は当時の値を再現できない (slot 再利用済みなら 0/別 gen)。
            // 前フレ pair は「直前まで存在していた」ので gen は現スロットの値を使う。
            // Remove 直後ケースだと gen が 0 (inactive) になるので IsValid だけ確認。
            if (_on_exit) {
                const TriggerSlot& sa = _slots[p.a_idx];
                const TriggerSlot& sb = _slots[p.b_idx];
                _on_exit(_on_exit_user,
                         TriggerId{p.a_idx, sa.gen},
                         TriggerId{p.b_idx, sb.gen});
            }
            ++ip;
        } else {
            // 今のみ → 新規 overlap → OnEnter
            if (_on_enter) {
                const TriggerSlot& sa = _slots[n.a_idx];
                const TriggerSlot& sb = _slots[n.b_idx];
                _on_enter(_on_enter_user,
                          TriggerId{n.a_idx, sa.gen},
                          TriggerId{n.b_idx, sb.gen});
            }
            ++in;
        }
    }
    // 残り: 前フレに余っているもの → 全て OnExit
    while (ip < np) {
        const OverlapPair& p = _pairs[ip];
        if (_on_exit) {
            const TriggerSlot& sa = _slots[p.a_idx];
            const TriggerSlot& sb = _slots[p.b_idx];
            _on_exit(_on_exit_user,
                     TriggerId{p.a_idx, sa.gen},
                     TriggerId{p.b_idx, sb.gen});
        }
        ++ip;
    }
    // 残り: 今フレに余っているもの → 全て OnEnter
    while (in < nn) {
        const OverlapPair& n = _next_pairs[in];
        if (_on_enter) {
            const TriggerSlot& sa = _slots[n.a_idx];
            const TriggerSlot& sb = _slots[n.b_idx];
            _on_enter(_on_enter_user,
                      TriggerId{n.a_idx, sa.gen},
                      TriggerId{n.b_idx, sb.gen});
        }
        ++in;
    }

    // 3. 次フレ用に _pairs を _next_pairs で置換。
    //    swap で再確保を抑え、_next_pairs は次 Tick で Clear して再利用。
    TArray<OverlapPair> tmp = Move(_pairs);
    _pairs = Move(_next_pairs);
    _next_pairs = Move(tmp);
}

} // namespace acs::game
