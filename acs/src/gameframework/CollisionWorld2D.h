// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 1 — FCollisionWorld2D + SpatialGrid (Phase 10)
//
// 2D 衝突判定の高レベル API。形状 (Aabb2 / Circle) を `FShapeId` で管理し、
// 内部の SpatialGrid (一様グリッド broad-phase) で O(N + K) クエリ
// (K = 結果数) を提供する。narrow phase は `math/Collision2D.h` の
// 既存関数 (Intersect / Resolve / RaycastAabb / RaycastCircle) を再利用。
//
// 使い方:
//   FCollisionWorld2D world;
//   world.Init(/*cell_size=*/64.0f);
//
//   FShapeId player = world.AddCircle({ {0,0}, 16.0f });
//   FShapeId wall   = world.AddAabb({ {100,0}, {16,32} });
//
//   // 毎フレーム移動した形状を Update
//   world.UpdateCircle(player, { player_pos, 16.0f });
//
//   // クエリ
//   TArray<FShapeId> hits;
//   world.OverlapCircle({ player_pos, 32.0f }, hits);  // 32 範囲のもの全部
//
//   RayHit2 rh;
//   FShapeId hit_id;
//   if (world.Raycast({ origin, dir }, /*max_t=*/100.0f, rh, hit_id)) {
//       // rh.point/normal/t、hit_id でどの形状か分かる
//   }
//
// 設計 (Phase 10 = Pillar F Phase 1):
//   ・**FShapeId**: 32bit packed = 24bit index + 8bit generation。removed slot
//     を再利用しても古い handle は無効化される (FNodeId と同パターン)。
//   ・**一様グリッド**: `cell_size` で固定、shape は overlapping cells に
//     全部登録 (= shape は複数 cell に存在し得る)。クエリ時は cell 走査して
//     候補集合を作り、narrow phase で実際の交差判定。
//   ・**dirty flag + lazy rebuild**: Add/Update/Remove で `m_Dirty = true`、
//     query 直前にグリッド再構築。Phase 1 は per-frame full rebuild。
//   ・**Layer / mask は Phase 2 で**。今は全 shape が交差候補。
//   ・**FPhysicsBody2D (kinematic body + collide-and-slide) は Phase 2 で**。
//     Phase 1 は「形状登録 + クエリ」のみ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

namespace acs::game {

struct FShapeId {
    u32 m_Packed = 0;   // 0 = invalid。layout: low24=index, high8=generation

    constexpr FShapeId() noexcept = default;
    constexpr FShapeId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }
    constexpr bool IsValid() const noexcept { return m_Packed != 0; }

    constexpr bool operator==(FShapeId o) const noexcept { return m_Packed == o.m_Packed; }
    constexpr bool operator!=(FShapeId o) const noexcept { return m_Packed != o.m_Packed; }
};

class FCollisionWorld2D {
public:
    FCollisionWorld2D() noexcept = default;
    ~FCollisionWorld2D() noexcept = default;

    FCollisionWorld2D(const FCollisionWorld2D&)            = delete;
    FCollisionWorld2D& operator=(const FCollisionWorld2D&) = delete;

    // cell_size: SpatialGrid のセルサイズ (world unit)。典型的に形状の最大
    // サイズの 2-3 倍。0 以下 / 未呼出時は 64.0f が既定。
    void Init(f32 cell_size = 64.0f) noexcept {
        m_CellSize = cell_size > 0.0f ? cell_size : 64.0f;
    }

    // ----- Shape 登録 -----
    FShapeId AddAabb   (const Aabb2& a) noexcept;
    FShapeId AddCircle (const Circle& c) noexcept;
    FShapeId AddPolygon(const ConvexPoly2& p) noexcept;   // スプライト凸包コライダー等

    // 形状更新 (移動した時)。dirty にして次のクエリで再構築。
    void UpdateAabb   (FShapeId id, const Aabb2& a) noexcept;
    void UpdateCircle (FShapeId id, const Circle& c) noexcept;
    void UpdatePolygon(FShapeId id, const ConvexPoly2& p) noexcept;

    // shape 削除 (slot は再利用、generation 進む)
    void Remove(FShapeId id) noexcept;

    // 全 shape 破棄。grid もクリア。
    void ClearAll() noexcept;

    u32 ShapeCount() const noexcept { return m_ShapeCount; }

    // ----- クエリ (broad-phase grid → narrow-phase math/Collision2D) -----
    // exclude: 自身を除外したい時 (PhysicsBody が自己 overlap を無視するため)。invalid なら除外無し。
    void OverlapAabb   (const Aabb2& a,   TArray<FShapeId>& out, FShapeId exclude = {}) noexcept;
    void OverlapCircle (const Circle& c,  TArray<FShapeId>& out, FShapeId exclude = {}) noexcept;
    void OverlapPolygon(const ConvexPoly2& p, TArray<FShapeId>& out, FShapeId exclude = {}) noexcept;
    // Raycast: 最も近い shape を 1 つ返す (out_hit / out_id 設定、無ければ false)
    bool Raycast(const Ray2& ray, f32 max_t, RayHit2& out_hit, FShapeId& out_id) noexcept;

private:
    enum class Kind : u8 { None = 0, FAabb, Circle, Poly };

    struct Slot {
        Kind        kind   = Kind::None;
        bool        active = false;
        u8          gen    = 0;
        Aabb2       aabb   {};
        Circle      circle {};
        ConvexPoly2 poly   {};
    };

    // SpatialGrid: cell key (hash of 2D cell coords) → list of slot indices
    struct GridCell {
        i32 cx = 0;
        i32 cy = 0;
        TArray<u32> shapes;
    };

    u32  AcquireSlot() noexcept;
    void MarkDirty() noexcept { m_Dirty = true; }
    void RebuildGridIfDirty() noexcept;

    // AABB が overlapping するセル範囲 (cx_min, cy_min, cx_max, cy_max) を返す
    void CellRange(const Aabb2& a, i32& cx_min, i32& cy_min,
                                    i32& cx_max, i32& cy_max) const noexcept;
    void CellRange(const Circle& c, i32& cx_min, i32& cy_min,
                                    i32& cx_max, i32& cy_max) const noexcept;

    GridCell& GetOrCreateCell(i32 cx, i32 cy) noexcept;
    GridCell* FindCell(i32 cx, i32 cy) noexcept;
    void InsertSlotIntoCells(u32 slot_idx) noexcept;

    // Narrow phase: slot[idx] が形状 X と交差するか
    bool NarrowIntersectAabb  (u32 slot_idx, const Aabb2& a) const noexcept;
    bool NarrowIntersectCircle(u32 slot_idx, const Circle& c) const noexcept;
    bool NarrowIntersectPoly  (u32 slot_idx, const ConvexPoly2& p) const noexcept;

    TArray<Slot>     m_Slots;
    u32             m_ShapeCount = 0;
    TArray<GridCell> m_Cells;          // 直接 TArray、cx/cy で線形検索 (Phase 1 簡略化)
    f32             m_CellSize   = 64.0f;
    bool            m_Dirty       = false;
    // クエリ中の重複除去用 (Phase 1: shape_count 長 bool array、簡素)
    TArray<u8>       m_QueryMarks;
};

} // namespace acs::game
