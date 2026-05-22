// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 1 — 実装 (Phase 10)
#include "gameframework/CollisionWorld2D.h"
#include "foundation/Move.h"
#include "math/Math.h"

namespace acs::game {

u32 CollisionWorld2D::AcquireSlot() noexcept {
    for (u32 i = 1; i < _slots.Size(); ++i) {   // index 0 を予約 (= invalid)
        if (!_slots[i].active) return i;
    }
    if (_slots.IsEmpty()) {
        _slots.PushBack({});   // dummy at index 0
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

ShapeId CollisionWorld2D::AddAabb(const Aabb2& a) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.kind   = Kind::Aabb;
    s.aabb   = a;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++_shape_count;
    MarkDirty();
    return ShapeId{idx, s.gen};
}

ShapeId CollisionWorld2D::AddCircle(const Circle& c) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.kind   = Kind::Circle;
    s.circle = c;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++_shape_count;
    MarkDirty();
    return ShapeId{idx, s.gen};
}

void CollisionWorld2D::UpdateAabb(ShapeId id, const Aabb2& a) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    Slot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Aabb) return;
    s.aabb = a;
    MarkDirty();
}

void CollisionWorld2D::UpdateCircle(ShapeId id, const Circle& c) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    Slot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != Kind::Circle) return;
    s.circle = c;
    MarkDirty();
}

void CollisionWorld2D::Remove(ShapeId id) noexcept {
    if (!id.IsValid() || id.Index() >= _slots.Size()) return;
    Slot& s = _slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    s.kind   = Kind::None;
    if (_shape_count > 0) --_shape_count;
    MarkDirty();
}

void CollisionWorld2D::ClearAll() noexcept {
    _slots.Clear();
    _cells.Clear();
    _shape_count = 0;
    _dirty = false;
}

// AABB 中心 + half_size から overlapping cell 範囲
void CollisionWorld2D::CellRange(const Aabb2& a, i32& cx_min, i32& cy_min,
                                  i32& cx_max, i32& cy_max) const noexcept {
    const f32 inv = 1.0f / _cell_size;
    const Vec2 mn = a.Min();
    const Vec2 mx = a.Max();
    cx_min = static_cast<i32>(Floor(mn.x * inv));
    cy_min = static_cast<i32>(Floor(mn.y * inv));
    cx_max = static_cast<i32>(Floor(mx.x * inv));
    cy_max = static_cast<i32>(Floor(mx.y * inv));
}

void CollisionWorld2D::CellRange(const Circle& c, i32& cx_min, i32& cy_min,
                                  i32& cx_max, i32& cy_max) const noexcept {
    Aabb2 a;
    a.center    = c.center;
    a.half_size = Vec2{c.radius, c.radius};
    CellRange(a, cx_min, cy_min, cx_max, cy_max);
}

CollisionWorld2D::GridCell* CollisionWorld2D::FindCell(i32 cx, i32 cy) noexcept {
    for (u32 i = 0; i < _cells.Size(); ++i) {
        if (_cells[i].cx == cx && _cells[i].cy == cy) return &_cells[i];
    }
    return nullptr;
}

CollisionWorld2D::GridCell& CollisionWorld2D::GetOrCreateCell(i32 cx, i32 cy) noexcept {
    if (GridCell* found = FindCell(cx, cy)) return *found;
    GridCell nc;
    nc.cx = cx;
    nc.cy = cy;
    _cells.PushBack(Move(nc));
    return _cells.Back();
}

void CollisionWorld2D::InsertSlotIntoCells(u32 slot_idx) noexcept {
    const Slot& s = _slots[slot_idx];
    if (!s.active) return;
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    switch (s.kind) {
    case Kind::Aabb:   CellRange(s.aabb,   cx_min, cy_min, cx_max, cy_max); break;
    case Kind::Circle: CellRange(s.circle, cx_min, cy_min, cx_max, cy_max); break;
    default: return;
    }
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GetOrCreateCell(cx, cy).shapes.PushBack(slot_idx);
        }
    }
}

void CollisionWorld2D::RebuildGridIfDirty() noexcept {
    if (!_dirty) return;
    _cells.Clear();
    for (u32 i = 1; i < _slots.Size(); ++i) {   // 0 は invalid
        if (_slots[i].active) InsertSlotIntoCells(i);
    }
    _dirty = false;
}

// ===== Narrow phase helpers =====
bool CollisionWorld2D::NarrowIntersectAabb(u32 slot_idx, const Aabb2& a) const noexcept {
    const Slot& s = _slots[slot_idx];
    switch (s.kind) {
    case Kind::Aabb:   return Intersect(s.aabb,   a);
    case Kind::Circle: return Intersect(s.circle, a);
    default: return false;
    }
}

bool CollisionWorld2D::NarrowIntersectCircle(u32 slot_idx, const Circle& c) const noexcept {
    const Slot& s = _slots[slot_idx];
    switch (s.kind) {
    case Kind::Aabb:   return Intersect(s.aabb,   c);
    case Kind::Circle: return Intersect(s.circle, c);
    default: return false;
    }
}

// ===== クエリ =====
void CollisionWorld2D::OverlapAabb(const Aabb2& a, Array<ShapeId>& out, ShapeId exclude) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    _query_marks.Resize(_slots.Size());
    for (u32 i = 0; i < _query_marks.Size(); ++i) _query_marks[i] = 0;
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
                if (_query_marks[idx]) continue;
                _query_marks[idx] = 1;
                if (NarrowIntersectAabb(idx, a)) {
                    out.PushBack(ShapeId{idx, _slots[idx].gen});
                }
            }
        }
    }
}

void CollisionWorld2D::OverlapCircle(const Circle& c, Array<ShapeId>& out, ShapeId exclude) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    _query_marks.Resize(_slots.Size());
    for (u32 i = 0; i < _query_marks.Size(); ++i) _query_marks[i] = 0;
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
                if (_query_marks[idx]) continue;
                _query_marks[idx] = 1;
                if (NarrowIntersectCircle(idx, c)) {
                    out.PushBack(ShapeId{idx, _slots[idx].gen});
                }
            }
        }
    }
}

bool CollisionWorld2D::Raycast(const Ray2& ray, f32 max_t,
                                RayHit2& out_hit, ShapeId& out_id) noexcept {
    out_hit = {};
    out_id  = {};
    RebuildGridIfDirty();
    // Phase 1 簡略化: ray の長さ範囲を AABB で囲って overlap → 各候補に narrow raycast。
    // 後段で DDA traversal に置換 (Phase 2)。
    Vec2 ray_min{
        ray.origin.x + (ray.direction.x < 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y < 0 ? ray.direction.y * max_t : 0.0f),
    };
    Vec2 ray_max{
        ray.origin.x + (ray.direction.x > 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y > 0 ? ray.direction.y * max_t : 0.0f),
    };
    const Aabb2 broad = Aabb2::FromMinMax(ray_min, ray_max);

    _query_marks.Resize(_slots.Size());
    for (u32 i = 0; i < _query_marks.Size(); ++i) _query_marks[i] = 0;

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
                if (_query_marks[idx]) continue;
                _query_marks[idx] = 1;
                const Slot& s = _slots[idx];
                RayHit2 rh{};
                switch (s.kind) {
                case Kind::Aabb:   rh = RaycastAabb(ray,   s.aabb,   max_t); break;
                case Kind::Circle: rh = RaycastCircle(ray, s.circle, max_t); break;
                default: break;
                }
                if (rh.hit && rh.t < best_t) {
                    best_t = rh.t;
                    out_hit = rh;
                    out_id  = ShapeId{idx, s.gen};
                    any_hit = true;
                }
            }
        }
    }
    return any_hit;
}

} // namespace acs::game
