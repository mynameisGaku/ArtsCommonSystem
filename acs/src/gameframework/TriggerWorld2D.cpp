// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — FTriggerWorld2D 実装
#include "gameframework/TriggerWorld2D.h"
#include "foundation/Move.h"

namespace acs::game {

/** 空き slot を確保する (index 0 は invalid 用に予約)。 */
u32 FTriggerWorld2D::AcquireSlot() noexcept {
    // index 0 を invalid 用に予約 (FTriggerId == 0 == invalid と一致させる)
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** 初期化する (現状は状態を持たないが、将来の SpatialGrid 等のため API を予約)。 */
void FTriggerWorld2D::Init() noexcept {
    // 現状は特に何もしない。将来 SpatialGrid を追加した時に cell_size 等を持つ。
}

/** 円形 trigger を登録する。 */
FTriggerId FTriggerWorld2D::AddCircle(const FCircle& c, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FTriggerSlot& s = m_Slots[idx];
    s.kind   = EKind::Circle;
    s.circle = c;
    s.layer  = layer;
    s.user   = nullptr;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;   // generation 0 は invalid 予約のためスキップ
    s.active = true;
    ++m_TriggerCount;
    return FTriggerId{idx, s.gen};
}

/** AABB trigger を登録する。 */
FTriggerId FTriggerWorld2D::AddAabb(const FAabb2& a, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FTriggerSlot& s = m_Slots[idx];
    s.kind   = EKind::Aabb;
    s.aabb   = a;
    s.layer  = layer;
    s.user   = nullptr;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_TriggerCount;
    return FTriggerId{idx, s.gen};
}

/** 円形 trigger の形状を更新する (移動時)。 */
void FTriggerWorld2D::UpdateCircle(FTriggerId id, const FCircle& c) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FTriggerSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Circle) return;
    s.circle = c;
}

/** AABB trigger の形状を更新する (移動時)。 */
void FTriggerWorld2D::UpdateAabb(FTriggerId id, const FAabb2& a) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FTriggerSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Aabb) return;
    s.aabb = a;
}

/** trigger を削除する (slot は再利用、generation が進む)。 */
void FTriggerWorld2D::Remove(FTriggerId id) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FTriggerSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    s.kind   = EKind::None;
    if (m_TriggerCount > 0) --m_TriggerCount;
    // 関連 pair は次 Tick で OnExit 発火後に自然消滅する (next_pairs に乗らないため)
}

/** trigger に任意の user ポインタを紐付ける。 */
void FTriggerWorld2D::SetUserData(FTriggerId id, void* user) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FTriggerSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.user = user;
}

/** trigger に紐付けた user ポインタを取得する。 */
void* FTriggerWorld2D::UserData(FTriggerId id) const noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return nullptr;
    const FTriggerSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return nullptr;
    return s.user;
}

/** overlap 開始イベントのコールバックを設定する。 */
void FTriggerWorld2D::SetOnEnter(TriggerEventCallback cb, void* user) noexcept {
    m_OnEnter      = cb;
    m_OnEnterUser = user;
}

/** overlap 継続イベントのコールバックを設定する。 */
void FTriggerWorld2D::SetOnStay(TriggerEventCallback cb, void* user) noexcept {
    m_OnStay      = cb;
    m_OnStayUser = user;
}

/** overlap 終了イベントのコールバックを設定する。 */
void FTriggerWorld2D::SetOnExit(TriggerEventCallback cb, void* user) noexcept {
    m_OnExit      = cb;
    m_OnExitUser = user;
}

/** 全 trigger と overlap state を破棄する。 */
void FTriggerWorld2D::ClearAll() noexcept {
    m_Slots.Clear();
    m_Pairs.Clear();
    m_NextPairs.Clear();
    m_TriggerCount = 0;
}

/** 2 つの trigger slot が幾何的に重なるかを返す (narrow phase)。 */
bool FTriggerWorld2D::ShapesOverlap(const FTriggerSlot& a, const FTriggerSlot& b) const noexcept {
    if (a.kind == EKind::Aabb && b.kind == EKind::Aabb)       return Intersect(a.aabb,   b.aabb);
    if (a.kind == EKind::Circle && b.kind == EKind::Circle) return Intersect(a.circle, b.circle);
    if (a.kind == EKind::Aabb && b.kind == EKind::Circle)     return Intersect(a.aabb,   b.circle);
    if (a.kind == EKind::Circle && b.kind == EKind::Aabb)     return Intersect(b.aabb,   a.circle);
    return false;
}

/** 全 pair の overlap 状態を更新し、前フレと比較してイベントを発火する。 */
void FTriggerWorld2D::Tick(f32 /*dt*/) noexcept {
    // 1. 今フレの overlap pair を全 O(N^2) ペアで再計算し m_NextPairs に格納。
    //    (a, b) は a < b で正規化。active な slot のみを対象。
    m_NextPairs.Clear();
    const u32 slot_count = static_cast<u32>(m_Slots.Size());
    for (u32 i = 1; i < slot_count; ++i) {                  // 0 は invalid 予約
        const FTriggerSlot& sa = m_Slots[i];
        if (!sa.active) continue;
        for (u32 j = i + 1; j < slot_count; ++j) {
            const FTriggerSlot& sb = m_Slots[j];
            if (!sb.active) continue;
            if (!ShapesOverlap(sa, sb)) continue;
            FOverlapPair np;
            np.a_idx           = i;
            np.b_idx           = j;
            np.was_overlapping = true;   // 「今フレ overlap している」マーカ
            m_NextPairs.PushBack(np);
        }
    }
    // 走査順 (i 昇順、j > i) により m_NextPairs は (a_idx, b_idx) 辞書順でソート済み。

    // 2. 前フレ m_Pairs と今フレ m_NextPairs を辞書順マージで突き合わせ:
    //    ・両方にある  → OnStay
    //    ・前のみ      → OnExit
    //    ・今のみ      → OnEnter
    u32 ip = 0;                                  // 前フレ pairs index
    u32 in = 0;                                  // 今フレ next_pairs index
    const u32 np = static_cast<u32>(m_Pairs.Size());
    const u32 nn = static_cast<u32>(m_NextPairs.Size());

    while (ip < np && in < nn) {
        const FOverlapPair& p = m_Pairs[ip];
        const FOverlapPair& n = m_NextPairs[in];
        // (a, b) 辞書順で比較
        const bool less_p = (p.a_idx <  n.a_idx) ||
                           (p.a_idx == n.a_idx && p.b_idx <  n.b_idx);
        const bool less_n = (n.a_idx <  p.a_idx) ||
                           (n.a_idx == p.a_idx && n.b_idx <  p.b_idx);
        if (!less_p && !less_n) {
            // 同じ pair → 前フレ overlap で今フレも overlap → OnStay
            if (m_OnStay) {
                const FTriggerSlot& sa = m_Slots[p.a_idx];
                const FTriggerSlot& sb = m_Slots[p.b_idx];
                m_OnStay(m_OnStayUser,
                         FTriggerId{p.a_idx, sa.gen},
                         FTriggerId{p.b_idx, sb.gen});
            }
            ++ip; ++in;
        } else if (less_p) {
            // 前のみ → 離れた → OnExit
            // Remove で消えた slot が含まれる可能性があるため、active 検証して
            // generation は当時の値を再現できない (slot 再利用済みなら 0/別 gen)。
            // 前フレ pair は「直前まで存在していた」ので gen は現スロットの値を使う。
            // Remove 直後ケースだと gen が 0 (inactive) になるので IsValid だけ確認。
            if (m_OnExit) {
                const FTriggerSlot& sa = m_Slots[p.a_idx];
                const FTriggerSlot& sb = m_Slots[p.b_idx];
                m_OnExit(m_OnExitUser,
                         FTriggerId{p.a_idx, sa.gen},
                         FTriggerId{p.b_idx, sb.gen});
            }
            ++ip;
        } else {
            // 今のみ → 新規 overlap → OnEnter
            if (m_OnEnter) {
                const FTriggerSlot& sa = m_Slots[n.a_idx];
                const FTriggerSlot& sb = m_Slots[n.b_idx];
                m_OnEnter(m_OnEnterUser,
                          FTriggerId{n.a_idx, sa.gen},
                          FTriggerId{n.b_idx, sb.gen});
            }
            ++in;
        }
    }
    // 残り: 前フレに余っているもの → 全て OnExit
    while (ip < np) {
        const FOverlapPair& p = m_Pairs[ip];
        if (m_OnExit) {
            const FTriggerSlot& sa = m_Slots[p.a_idx];
            const FTriggerSlot& sb = m_Slots[p.b_idx];
            m_OnExit(m_OnExitUser,
                     FTriggerId{p.a_idx, sa.gen},
                     FTriggerId{p.b_idx, sb.gen});
        }
        ++ip;
    }
    // 残り: 今フレに余っているもの → 全て OnEnter
    while (in < nn) {
        const FOverlapPair& n = m_NextPairs[in];
        if (m_OnEnter) {
            const FTriggerSlot& sa = m_Slots[n.a_idx];
            const FTriggerSlot& sb = m_Slots[n.b_idx];
            m_OnEnter(m_OnEnterUser,
                      FTriggerId{n.a_idx, sa.gen},
                      FTriggerId{n.b_idx, sb.gen});
        }
        ++in;
    }

    // 3. 次フレ用に m_Pairs を m_NextPairs で置換。
    //    swap で再確保を抑え、m_NextPairs は次 Tick で Clear して再利用。
    TArray<FOverlapPair> tmp = Move(m_Pairs);
    m_Pairs = Move(m_NextPairs);
    m_NextPairs = Move(tmp);
}

} // namespace acs::game
