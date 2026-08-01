// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — 実装
#include "gameframework/CollisionWorld2D.h"
#include "foundation/Move.h"
#include "math/Math.h"

namespace acs::game {

/** 空き slot を確保してインデックスを返す (index 0 は invalid 予約)。 */
u32 CCollisionWorld2D::AcquireSlot() noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {   // index 0 を予約 (= invalid)
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** AABB 形状を登録して handle を返す。 */
FShapeId CCollisionWorld2D::AddAabb(const FAabb2& a, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind   = EKind::Aabb;
    s.aabb   = a;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

/** 円形状を登録して handle を返す。 */
FShapeId CCollisionWorld2D::AddCircle(const FCircle& c, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind   = EKind::Circle;
    s.circle = c;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

/** 凸ポリゴン形状を登録して handle を返す。 */
FShapeId CCollisionWorld2D::AddPolygon(const FConvexPoly2& p, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind   = EKind::Poly;
    s.poly   = p;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

/** OBB (回転矩形) を登録して handle を返す。 */
FShapeId CCollisionWorld2D::AddObb(const FObb2& o, u32 layer) noexcept {
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind   = EKind::Obb;
    s.obb    = o;
    s.layer  = layer;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;
    s.active = true;
    ++m_ShapeCount;
    MarkDirty();
    return FShapeId{idx, s.gen};
}

/** AABB 形状を更新する (handle / 種別不一致なら no-op)。 */
void CCollisionWorld2D::UpdateAabb(FShapeId id, const FAabb2& a) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Aabb) return;
    s.aabb = a;
    MarkDirty();
}

/** 円形状を更新する (handle / 種別不一致なら no-op)。 */
void CCollisionWorld2D::UpdateCircle(FShapeId id, const FCircle& c) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Circle) return;
    s.circle = c;
    MarkDirty();
}

/** 凸ポリゴン形状を更新する (handle / 種別不一致なら no-op)。 */
void CCollisionWorld2D::UpdatePolygon(FShapeId id, const FConvexPoly2& p) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Poly) return;
    s.poly = p;
    MarkDirty();
}

/** OBB 形状を更新する (handle / 種別不一致なら no-op)。 */
void CCollisionWorld2D::UpdateObb(FShapeId id, const FObb2& o) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation() || s.kind != EKind::Obb) return;
    s.obb = o;
    MarkDirty();
}

/** shape を削除する (slot 再利用・generation 進行)。 */
void CCollisionWorld2D::Remove(FShapeId id) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    s.kind   = EKind::None;
    if (m_ShapeCount > 0) --m_ShapeCount;
    MarkDirty();
}

/** 全 shape を破棄し grid もクリアする。 */
void CCollisionWorld2D::ClearAll() noexcept {
    m_Slots.Clear();
    m_Cells.Clear();
    m_HugeShapes.Clear();
    m_ShapeCount = 0;
    m_Dirty = false;
}

/**
 * float をセル index へ安全に変換する (NaN は 0、i32 範囲外はクランプ)。
 *
 * @details
 * float→int は変換先に収まらない値だと未定義動作なので必ずここを通す。閾値は f32 で
 * 厳密表現できる 2^31-128 を使う (2^31 は float で 2147483648.0 になり危険)。
 * @param v 変換する浮動小数値。
 * @return 安全に丸めたセル index。
 */
static ACS_FORCEINLINE i32 SafeCellIndex(f32 v) noexcept {
    if (!(v == v)) return 0;                            // NaN
    if (v <= -2147483520.0f) return (-2147483647 - 1);  // <= INT32_MIN
    if (v >=  2147483520.0f) return   2147483647;       // >= INT32_MAX
    return static_cast<i32>(v);
}

/** 1 形状 / 1 クエリがセル二重ループで走査してよい総セル数の上限 (64x64)。 */
static constexpr i64 kMaxGridCells = 64 * 64;

/**
 * セル範囲の総セル数が kMaxGridCells を超えるかを判定する。
 *
 * @details
 * 巨大 (inf / 1e30 級) の extent は SafeCellIndex で i32 全域にクランプされ、
 * そのままセル二重ループへ入ると事実上終わらない (フリーズ)。掛け算前に
 * 軸単位で判定して i64 の桁あふれも避ける。
 * @return 上限を超えるなら true (呼び出し側はセル走査を諦める)。
 */
static bool CellRangeTooLarge(i32 cx_min, i32 cy_min, i32 cx_max, i32 cy_max) noexcept {
    const i64 sx = static_cast<i64>(cx_max) - cx_min + 1;
    const i64 sy = static_cast<i64>(cy_max) - cy_min + 1;
    if (sx > kMaxGridCells || sy > kMaxGridCells) return true;
    return sx * sy > kMaxGridCells;
}

/** AABB が重なるセル範囲を求める。 */
void CCollisionWorld2D::CellRange(const FAabb2& a, i32& cx_min, i32& cy_min,
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

/** 円が重なるセル範囲を求める (AABB に囲って委譲)。 */
void CCollisionWorld2D::CellRange(const FCircle& c, i32& cx_min, i32& cy_min,
                                  i32& cx_max, i32& cy_max) const noexcept {
    FAabb2 a;
    a.center    = c.center;
    a.half_size = FVec2{c.radius, c.radius};
    CellRange(a, cx_min, cy_min, cx_max, cy_max);
}

/** (cx, cy) のセルを線形探索する (無ければ nullptr)。 */
CCollisionWorld2D::FGridCell* CCollisionWorld2D::FindCell(i32 cx, i32 cy) noexcept {
    for (u32 i = 0; i < m_Cells.Size(); ++i) {
        if (m_Cells[i].cx == cx && m_Cells[i].cy == cy) return &m_Cells[i];
    }
    return nullptr;
}

/** (cx, cy) のセルを取得し、無ければ新規作成して返す。 */
CCollisionWorld2D::FGridCell& CCollisionWorld2D::GetOrCreateCell(i32 cx, i32 cy) noexcept {
    if (FGridCell* found = FindCell(cx, cy)) return *found;
    FGridCell nc;
    nc.cx = cx;
    nc.cy = cy;
    m_Cells.PushBack(Move(nc));
    return m_Cells.Back();
}

/** slot を重なる全セルに登録する (巨大形状は m_HugeShapes へ退避)。 */
void CCollisionWorld2D::InsertSlotIntoCells(u32 slot_idx) noexcept {
    const FSlot& s = m_Slots[slot_idx];
    if (!s.active) return;
    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    switch (s.kind) {
    case EKind::Aabb:    CellRange(s.aabb,   cx_min, cy_min, cx_max, cy_max); break;
    case EKind::Circle: CellRange(s.circle, cx_min, cy_min, cx_max, cy_max); break;
    case EKind::Poly:   CellRange(AabbOf(s.poly), cx_min, cy_min, cx_max, cy_max); break;
    case EKind::Obb:    CellRange(AabbOf(s.obb),  cx_min, cy_min, cx_max, cy_max); break;
    default: return;
    }
    if (CellRangeTooLarge(cx_min, cy_min, cx_max, cy_max)) {
        m_HugeShapes.PushBack(slot_idx);
        return;
    }
    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            GetOrCreateCell(cx, cy).shapes.PushBack(slot_idx);
        }
    }
}

/** dirty なときだけグリッドを全再構築する。 */
void CCollisionWorld2D::RebuildGridIfDirty() noexcept {
    if (!m_Dirty) return;
    m_Cells.Clear();
    m_HugeShapes.Clear();
    for (u32 i = 1; i < m_Slots.Size(); ++i) {   // 0 は invalid
        if (m_Slots[i].active) InsertSlotIntoCells(i);
    }
    m_Dirty = false;
}

/** クエリ AABB のブロードフェーズ候補 slot を m_QueryScratch へ集める。 */
void CCollisionWorld2D::CollectCandidates(const FAabb2& box) noexcept {
    m_QueryScratch.Clear();
    m_QueryMarks.Resize(m_Slots.Size());
    for (u32 i = 0; i < m_QueryMarks.Size(); ++i) m_QueryMarks[i] = 0;

    i32 cx_min = 0, cy_min = 0, cx_max = 0, cy_max = 0;
    CellRange(box, cx_min, cy_min, cx_max, cy_max);

    // クエリ範囲自体が巨大な場合はセル走査を諦めて全 slot 線形走査 (正しさ優先で
    // ハングしない)。巨大形状も含めて全て候補になるのでこれで完結する。
    if (CellRangeTooLarge(cx_min, cy_min, cx_max, cy_max)) {
        for (u32 i = 1; i < m_Slots.Size(); ++i) {
            if (m_Slots[i].active) m_QueryScratch.PushBack(i);
        }
        return;
    }

    for (i32 cy = cy_min; cy <= cy_max; ++cy) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            FGridCell* cell = FindCell(cx, cy);
            if (!cell) continue;
            for (u32 i = 0; i < cell->shapes.Size(); ++i) {
                const u32 idx = cell->shapes[i];
                if (m_QueryMarks[idx]) continue;
                m_QueryMarks[idx] = 1;
                m_QueryScratch.PushBack(idx);
            }
        }
    }
    // グリッドに入っていない巨大形状は常に候補へ加える。
    for (u32 i = 0; i < m_HugeShapes.Size(); ++i) {
        const u32 idx = m_HugeShapes[i];
        if (m_QueryMarks[idx]) continue;
        m_QueryMarks[idx] = 1;
        m_QueryScratch.PushBack(idx);
    }
}

/** slot[idx] が AABB と交差するかを判定する (narrow phase)。 */
bool CCollisionWorld2D::NarrowIntersectAabb(u32 slot_idx, const FAabb2& a) const noexcept {
    const FSlot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case EKind::Aabb:    return Intersect(s.aabb,   a);
    case EKind::Circle: return Intersect(s.circle, a);
    case EKind::Poly:   return Intersect(s.poly,   a);
    case EKind::Obb:    return Intersect(s.obb,    a);
    default: return false;
    }
}

/** slot[idx] が円と交差するかを判定する (narrow phase)。 */
bool CCollisionWorld2D::NarrowIntersectCircle(u32 slot_idx, const FCircle& c) const noexcept {
    const FSlot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case EKind::Aabb:    return Intersect(s.aabb,   c);
    case EKind::Circle: return Intersect(s.circle, c);
    case EKind::Poly:   return Intersect(s.poly,   c);
    case EKind::Obb:    return Intersect(s.obb,    c);
    default: return false;
    }
}

/** slot[idx] が凸ポリゴンと交差するかを判定する (narrow phase)。 */
bool CCollisionWorld2D::NarrowIntersectPoly(u32 slot_idx, const FConvexPoly2& p) const noexcept {
    const FSlot& s = m_Slots[slot_idx];
    switch (s.kind) {
    case EKind::Aabb:    return Intersect(p, s.aabb);
    case EKind::Circle: return Intersect(p, s.circle);
    case EKind::Poly:   return Intersect(p, s.poly);
    case EKind::Obb:    return Intersect(p, s.obb);
    default: return false;
    }
}

/** AABB と重なる shape を列挙する。 */
void CCollisionWorld2D::OverlapAabb(const FAabb2& a, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    CollectCandidates(a);
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if (idx == ex_idx) continue;
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        if (NarrowIntersectAabb(idx, a)) {
            out.PushBack(FShapeId{idx, m_Slots[idx].gen});
        }
    }
}

/** 円と重なる shape を列挙する。 */
void CCollisionWorld2D::OverlapCircle(const FCircle& c, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    RebuildGridIfDirty();
    const FAabb2 box{ c.center, FVec2{ c.radius, c.radius } };
    CollectCandidates(box);
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if (idx == ex_idx) continue;
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        if (NarrowIntersectCircle(idx, c)) {
            out.PushBack(FShapeId{idx, m_Slots[idx].gen});
        }
    }
}

/** 凸ポリゴンと重なる shape を列挙する (頂点数 3 未満は無視)。 */
void CCollisionWorld2D::OverlapPolygon(const FConvexPoly2& p, TArray<FShapeId>& out, FShapeId exclude, u32 mask) noexcept {
    out.Clear();
    if (p.count < 3) return;
    RebuildGridIfDirty();
    CollectCandidates(AabbOf(p));
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if (idx == ex_idx) continue;
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        if (NarrowIntersectPoly(idx, p)) {
            out.PushBack(FShapeId{idx, m_Slots[idx].gen});
        }
    }
}

/** 円を重なる全 shape から押し出す合計ベクトルを返す (collide-and-slide 用)。 */
FVec2 CCollisionWorld2D::ResolveCircle(const FCircle& c, FShapeId exclude, u32 mask) noexcept {
    RebuildGridIfDirty();
    const FAabb2 box{ c.center, FVec2{ c.radius, c.radius } };
    CollectCandidates(box);
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    FVec2 total{ 0, 0 };
    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if (idx == ex_idx) continue;
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        const FSlot& s = m_Slots[idx];
        FVec2 push{ 0, 0 }; bool hit = false;
        switch (s.kind) {
        case EKind::Aabb:    hit = Resolve(c, ToPoly(s.aabb), push); break;
        case EKind::Circle: hit = Resolve(c, s.circle, push);       break;
        case EKind::Poly:   hit = Resolve(c, s.poly, push);         break;
        case EKind::Obb:    hit = Resolve(c, s.obb, push);          break;
        default: break;
        }
        if (hit) { total.x += push.x; total.y += push.y; }
    }
    return total;
}

/** 凸ポリゴンを重なる全 shape から押し出す合計ベクトルを返す (collide-and-slide 用)。 */
FVec2 CCollisionWorld2D::ResolvePolygon(const FConvexPoly2& p, FShapeId exclude, u32 mask) noexcept {
    RebuildGridIfDirty();
    CollectCandidates(AabbOf(p));
    const u32 ex_idx = exclude.IsValid() ? exclude.Index() : 0u;
    FVec2 total{ 0, 0 };
    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if (idx == ex_idx) continue;
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        const FSlot& s = m_Slots[idx];
        FVec2 push{ 0, 0 }; bool hit = false;
        switch (s.kind) {
        case EKind::Aabb:    hit = Resolve(p, ToPoly(s.aabb), push); break;
        case EKind::Poly:   hit = Resolve(p, s.poly, push);         break;
        case EKind::Obb:    hit = Resolve(p, s.obb, push);          break;
        case EKind::Circle: {
            FVec2 cp;                                 // 円を p から押す → 反転で p を押す
            if (Resolve(s.circle, p, cp)) { push = FVec2{ -cp.x, -cp.y }; hit = true; }
        } break;
        default: break;
        }
        if (hit) { total.x += push.x; total.y += push.y; }
    }
    return total;
}

/** レイをキャストし最も近い shape を 1 つ返す。 */
bool CCollisionWorld2D::Raycast(const FRay2& ray, f32 max_t,
                                FRayHit2& out_hit, FShapeId& out_id, u32 mask) noexcept {
    out_hit = {};
    out_id  = {};
    RebuildGridIfDirty();
    // ray の長さ範囲を AABB で囲って overlap → 各候補に narrow raycast。
    FVec2 ray_min{
        ray.origin.x + (ray.direction.x < 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y < 0 ? ray.direction.y * max_t : 0.0f),
    };
    FVec2 ray_max{
        ray.origin.x + (ray.direction.x > 0 ? ray.direction.x * max_t : 0.0f),
        ray.origin.y + (ray.direction.y > 0 ? ray.direction.y * max_t : 0.0f),
    };
    const FAabb2 broad = FAabb2::FromMinMax(ray_min, ray_max);
    CollectCandidates(broad);

    f32 best_t = max_t + 1.0f;
    bool any_hit = false;

    for (u32 i = 0; i < m_QueryScratch.Size(); ++i) {
        const u32 idx = m_QueryScratch[i];
        if ((m_Slots[idx].layer & mask) == 0u) continue;
        const FSlot& s = m_Slots[idx];
        FRayHit2 rh{};
        switch (s.kind) {
        case EKind::Aabb:    rh = RaycastAabb(ray,   s.aabb,   max_t); break;
        case EKind::Circle: rh = RaycastCircle(ray, s.circle, max_t); break;
        case EKind::Poly:   rh = RaycastConvexPoly2(ray, s.poly, max_t); break;
        case EKind::Obb:    rh = RaycastObb2(ray, s.obb, max_t); break;
        default: break;
        }
        if (rh.hit && rh.t < best_t) {
            best_t = rh.t;
            out_hit = rh;
            out_id  = FShapeId{idx, s.gen};
            any_hit = true;
        }
    }
    return any_hit;
}

} // namespace acs::game
