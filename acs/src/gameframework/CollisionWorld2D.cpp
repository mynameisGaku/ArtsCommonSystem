// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 1 — 実装 (Phase 10)
#include "gameframework/CollisionWorld2D.h"
#include "foundation/Move.h"
#include "math/Math.h"

namespace acs::game {

u32 FCollisionWorld2D::AcquireSlot() noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {   // index 0 を予約 (= invalid)
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

FShapeId FCollisionWorld2D::AddAabb(const Aabb2& a, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.kind   = Kind::FAabb;
    s.aabb   = a;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

FShapeId FCollisionWorld2D::AddCircle(const Circle& c, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.kind   = Kind::Circle;
    s.circle = c;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

FShapeId FCollisionWorld2D::AddPolygon(const ConvexPoly2& p, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.kind   = Kind::Poly;
    s.poly   = p;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

FShapeId FCollisionWorld2D::AddObb(const Obb2& o, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.kind   = Kind::Obb;
    s.obb    = o;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

void FCollisionWorld2D::UpdateAabb(FShapeId id, const Aabb2& a) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::FAabb) return;
    s.aabb = a;
    MarkDirty();
}

void FCollisionWorld2D::UpdateCircle(FShapeId id, const Circle& c) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Circle) return;
    s.circle = c;
    MarkDirty();
}

void FCollisionWorld2D::UpdatePolygon(FShapeId id, const ConvexPoly2& p) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Poly) return;
    s.poly = p;
    MarkDirty();
}

void FCollisionWorld2D::UpdateObb(FShapeId id, const Obb2& o) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Obb) return;
    s.obb = o;
    MarkDirty();
}

void FCollisionWorld2D::Remove(FShapeId id) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    s.kind   = Kind::None;
    if (m_ShapeCount > 0) --m_ShapeCount;
    MarkDirty();
}

void FCollisionWorld2D::ClearAll() noexcept {
    m_Slots.Clear();
    m_Cells.Clear();
    m_ShapeCount = 0;
    m_Dirty = false;
}

// float → セル index の安全変換。NaN は 0、i32 範囲外はクランプする。
// float→int は変換先に収まらない値だと未定義動作なので、必ずここを通す。
// 閾値は f32 で厳密表現できる 2^31-128 を使う (2^31 は float で 2147483648.0 になり危険)。
static ACS_FORCEINLINE i32 SafeCellIndex(f32 v) noexcept {
    if (!(v == v)) return 0;                            // NaN
    if (v <= -2147483520.0f) return (-2147483647 - 1);  // <= INT32_MIN
    if (v >=  2147483520.0f) return   2147483647;       // >= INT32_MAX
    return static_cast<i32>(v);
}

// AABB 中心 + half_size から overlapping cell 範囲
void FCollisionWorld2D::CellRange(const Aabb2& a, i32& cx_min, i32& cy_min,
                                  i32& cx_max, i32& cy_max) const noexcept {
    // m_CellSize が 0 / 負だと 1/0 = inf → mn*inf = NaN/inf となり後段の cast が UB。
    // 不正な CellSize は全 AABB を単一セル (index 0) に写像して退避する。
    const f32 inv = (m_CellSize > 0.0f) ? (1.0f / m_CellSize) : 0.0f;
    const FVec2 mn = a.Min();
    const FVec2 mx = a.Max();
    cx_min = SafeCellIndex(Floor(mn.x * inv));
    cy_min = SafeCellIndex(Floor(mn.y * inv));
    cx_max = SafeCellIndex(Floor(mx.x * inv));
    cy_max = SafeCellIndex(Floor(mx.y * inv));
}

void FCollisionWorld2D::CellRange(const Circle& c, i32& cx_min, i32& cy_min,
                                  i32& cx_max, i32& cy_max) const noexcept {
    Aabb2 a;
    a.center    = c.center;
    a.half_size = FVec2{c.radius, c.radius};
    CellRange(a, cx_min, cy_min, cx_max, cy_max);
}

FCollisionWorld2D::GridCell* FCollisionWorld2D::FindCell(i32 cx, i32 cy) noexcept {
    for (u32 i = 0; i < m_Cells.Size(); ++i) {
        if (m_Cells[i].cx == cx && m_Cells[i].cy == cy) return &m_Cells[i];
    }
    return nullptr;
}

FCollisionWorld2D::GridCell& FCollisionWorld2D::GetOrCreateCell(i32 cx, i32 cy) noexcept {
    if (GridCell* found = FindCell(cx, cy)) return *found;
    GridCell nc;
    nc.cx = cx;
    nc.cy = cy;
    m_Cells.PushBack(Move(nc));
    return m_Cells.Back();
}

void FCollisionWorld2D::InsertSlotIntoCells(u32 slot_idx) noexcept {
    const Slot& s = m_Slots[slot_idx];
    if (!s.active) return;
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    switch (s.kind) {
    case Kind::FAabb:   CellRange(s.aabb,   cx_min, cy_min, cx_max, cy_max); break;
    case Kind::Circle: CellRange(s.circle, cx_min, cy_min, cx_max, cy_max); break;
    case Kind::Poly:   CellRange(AabbOf(s.poly), cx_min, cy_min, cx_max, cy_max); break;
    case Kind::Obb:    CellRange(AabbOf(s.obb),  cx_min, cy_min, cx_max, cy_max); break;
    default: return;
    }
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GetOrCreateCell(cx, cy).shapes.PushBack(slot_idx);
        }
    }
}

void FCollisionWorld2D::RebuildGridIfDirty() noexcept {
    if (!m_Dirty) return;
    m_Cells.Clear();
    for (u32 i = 1; i < m_Slots.Size(); ++i) {   // 0 は invalid
        if (m_Slots[i].active) InsertSlotIntoCells(i);
    }
    m_Dirty = false;
}

// ===== Narrow phase helpers =====
bool FCollisionWorld2D::NarrowIntersectAabb(u32 slot_idx, const Aabb2& a) const noexcept {
    const Slot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case Kind::FAabb:   return Intersect(s.aabb,   a);
    case Kind::Circle: return Intersect(s.circle, a);
    case Kind::Poly:   return Intersect(s.poly,   a);
    case Kind::Obb:    return Intersect(s.obb,    a);
    default: return false;
    }
}

bool FCollisionWorld2D::NarrowIntersectCircle(u32 slot_idx, const Circle& c) const noexcept {
    const Slot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case Kind::FAabb:   return Intersect(s.aabb,   c);
    case Kind::Circle: return Intersect(s.circle, c);
    case Kind::Poly:   return Intersect(s.poly,   c);
    case Kind::Obb:    return Intersect(s.obb,    c);
    default: return false;
    }
}

bool FCollisionWorld2D::NarrowIntersectPoly(u32 slot_idx, const ConvexPoly2& p) const noexcept {
    const Slot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case Kind::FAabb:   return Intersect(p, s.aabb);
    case Kind::Circle: return Intersect(p, s.circle);
    case Kind::Poly:   return Intersect(p, s.poly);
    case Kind::Obb:    return Intersect(p, s.obb);
    default: return false;
    }
}

// ===== クエリ =====
void FCollisionWorld2D::OverlapAabb(const Aabb2& a, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(a, cx_min, cy_min, cx_max, cy_max);
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (idx == ex_idx) continue;
                if (m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                if (NarrowIntersectAabb(idx, a)) {
                    out.PushBack(FShapeId{idx, m_Slots[idx].gen});
                }
            }
        }
    }
}

void FCollisionWorld2D::OverlapCircle(const Circle& c, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(c, cx_min, cy_min, cx_max, cy_max);
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (idx == ex_idx) continue;
                if (m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                if (NarrowIntersectCircle(idx, c)) {
                    out.PushBack(FShapeId{idx, m_Slots[idx].gen});
                }
            }
        }
    }
}

void FCollisionWorld2D::OverlapPolygon(const ConvexPoly2& p, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    if (p.count < 3) return;
    RebuildGridIfDirty();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    const Aabb2 box = AabbOf(p);
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(box, cx_min, cy_min, cx_max, cy_max);
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (idx == ex_idx) continue;
                if (m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                if (NarrowIntersectPoly(idx, p)) {
                    out.PushBack(FShapeId{idx, m_Slots[idx].gen});
                }
            }
        }
    }
}

FVec2 FCollisionWorld2D::ResolveCircle(const Circle& c, FShapeId exclude, u32 mask) noexcept {
    RebuildGridIfDirty();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    FVec2 total{ 0, 0 };
    const Aabb2 box{ c.center, FVec2{ c.radius, c.radius } };
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(box, cx_min, cy_min, cx_max, cy_max);
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (idx == ex_idx || m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                const Slot& s = m_Slots[idx];
                FVec2 push{ 0, 0 }; bool hit = false;
                switch (s.kind) {
                case Kind::FAabb:   hit = Resolve(c, ToPoly(s.aabb), push); break;
                case Kind::Circle: hit = Resolve(c, s.circle, push);       break;
                case Kind::Poly:   hit = Resolve(c, s.poly, push);         break;
                case Kind::Obb:    hit = Resolve(c, s.obb, push);          break;
                default: break;
                }
                if (hit) { total.x += push.x; total.y += push.y; }
            }
        }
    }
    return total;
}

FVec2 FCollisionWorld2D::ResolvePolygon(const ConvexPoly2& p, FShapeId exclude, u32 mask) noexcept {
    RebuildGridIfDirty();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    FVec2 total{ 0, 0 };
    const Aabb2 box = AabbOf(p);
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(box, cx_min, cy_min, cx_max, cy_max);
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (idx == ex_idx || m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                const Slot& s = m_Slots[idx];
                FVec2 push{ 0, 0 }; bool hit = false;
                switch (s.kind) {
                case Kind::FAabb:   hit = Resolve(p, ToPoly(s.aabb), push); break;
                case Kind::Poly:   hit = Resolve(p, s.poly, push);         break;
                case Kind::Obb:    hit = Resolve(p, s.obb, push);          break;
                case Kind::Circle: {
                    FVec2 cp;                                 // 円を p から押す → 反転で p を押す
                    if (Resolve(s.circle, p, cp)) { push = FVec2{ -cp.x, -cp.y }; hit = true; }
                } break;
                default: break;
                }
                if (hit) { total.x += push.x; total.y += push.y; }
            }
        }
    }
    return total;
}

bool FCollisionWorld2D::Raycast(const Ray2& ray, f32 max_t,
                                RayHit2& out_hit, FShapeId& out_id, u32 mask) noexcept {
    out_hit = {};
    out_id  = {};
    RebuildGridIfDirty();
    // Phase 1 簡略化: ray の長さ範囲を AABB で囲って overlap → 各候補に narrow raycast。
    // 後段で DDA traversal に置換 (Phase 2)。
    FVec2 ray_min{
        ray.origin.x + (ray.direction.x < 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y < 0 ? ray.direction.y * max_t : 0.0f),
    };
    FVec2 ray_max{
        ray.origin.x + (ray.direction.x > 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y > 0 ? ray.direction.y * max_t : 0.0f),
    };
    const Aabb2 broad = Aabb2::FromMinMax(ray_min, ray_max);

    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;

    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(broad, cx_min, cy_min, cx_max, cy_max);

    f32 best_t = max_t + 1.0f;
    bool any_hit = false;

    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                if ((m_Slots[idx].layer & mask) == 0u) continue;
                const Slot& s = m_Slots[idx];
                RayHit2 rh{};
                switch (s.kind) {
                case Kind::FAabb:   rh = RaycastAabb(ray,   s.aabb,   max_t); break;
                case Kind::Circle: rh = RaycastCircle(ray, s.circle, max_t); break;
                case Kind::Poly:   rh = RaycastConvexPoly2(ray, s.poly, max_t); break;
                case Kind::Obb:    rh = RaycastObb2(ray, s.obb, max_t); break;
                default: break;
                }
                if (rh.hit && rh.t < best_t) {
                    best_t = rh.t;
                    out_hit = rh;
                    out_id  = FShapeId{idx, s.gen};
                    any_hit = true;
                }
            }
        }
    }
    return any_hit;
}

} // namespace acs::game
