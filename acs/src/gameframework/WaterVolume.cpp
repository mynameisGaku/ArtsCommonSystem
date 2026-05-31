// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q Phase 3 — FWaterVolume 実装
//
// AABB ベースの水域管理 + 浮力計算。slot+generation handle pattern は
// FCollisionWorld2D / FTriggerWorld2D と同等。Phase 3 は線形走査 (典型 N ≤ 数十)。
#include "gameframework/WaterVolume.h"

namespace acs::game {

// ----- 内部ヘルパ -----------------------------------------------------------

// AABB と点の包含判定。center / half_size から min/max を構築して比較。
// half_size 各成分は非負想定。負値が来ても |.| を取らずに比較に任せる
// (= 必ず外れる結果になり、利用者の構成ミスとして安全側に倒れる)。
static bool ContainsPoint(const FWaterVolumeInfo& v, FVec2 pos) noexcept {
    const f32 dx = pos.x - v.center.x;
    const f32 dy = pos.y - v.center.y;
    return (dx >= -v.half_size.x) && (dx <= v.half_size.x)
        && (dy >= -v.half_size.y) && (dy <= v.half_size.y);
}

// ----- Slot 管理 ------------------------------------------------------------

// index 0 は invalid handle に予約 (FCollisionWorld2D と同じ規約)。
u32 FWaterVolume::AcquireSlot() noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

// ----- パブリック API -------------------------------------------------------

FWaterVolumeId FWaterVolume::AddVolume(const FWaterVolumeInfo& info) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];
    s.info   = info;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;     // 0 を予約値として避ける (Add 時の安全弁)
    s.active = true;
    ++m_VolumeCount;
    m_CacheDirty = true;
    return FWaterVolumeId{idx, s.gen};
}

void FWaterVolume::RemoveVolume(FWaterVolumeId id) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    if (m_VolumeCount > 0) --m_VolumeCount;
    m_CacheDirty = true;
}

void FWaterVolume::UpdateVolume(FWaterVolumeId id, FVec2 center, FVec2 half_size) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    Slot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.info.center    = center;
    s.info.half_size = half_size;
    // surface_y / buoyancy_strength / drag / water_color は info から不変。
    // 水面位置を変えたい時は Remove → Add で。
    m_CacheDirty = true;
}

bool FWaterVolume::IsUnderwater(FVec2 pos) const noexcept {
    // index 0 は invalid 予約なので 1 から走査。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (ContainsPoint(s.info, pos)) return true;
    }
    return false;
}

f32 FWaterVolume::SubmersionDepth(FVec2 pos) const noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (!ContainsPoint(s.info, pos)) continue;
        const f32 depth = pos.y - s.info.surface_y;   // +Y=画面下: 水面=最小y、沈むほど y 大
        // depth が負 (pos が surface_y より上) の場合は 0 にクランプ。
        // AABB 内だが「水面より上」の異常な surface_y 設定でも 0 を返して
        // 利用側の浮力計算が暴走しないようにする。
        return depth > 0.0f ? depth : 0.0f;
    }
    return 0.0f;
}

FVec2 FWaterVolume::ComputeBuoyancyForce(FVec2 pos, FVec2 velocity, f32 mass) const noexcept {
    // 全 volume の寄与を加算。重なる volume があれば力も重畳。
    f32  total_depth_strength = 0.0f;  // Σ (buoyancy_strength * depth) を蓄積
    f32  total_drag           = 0.0f;  // Σ drag を蓄積
    bool in_water             = false;

    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (!ContainsPoint(s.info, pos)) continue;

        in_water = true;

        f32 depth = pos.y - s.info.surface_y;   // +Y=画面下: 水面=最小y、沈むほど y 大
        if (depth < 0.0f) depth = 0.0f;

        total_depth_strength += s.info.buoyancy_strength * depth;
        total_drag           += s.info.drag;
    }

    if (!in_water) return FVec2{0.0f, 0.0f};

    // 浮力 = (0, -Σ(strength * depth) * mass)  ※ 上向き = -Y (Y-down: +Y=画面下)
    // drag = -velocity * Σ(drag)
    const f32 fy = -(total_depth_strength * mass);
    return FVec2{
        -velocity.x * total_drag,
        fy - velocity.y * total_drag,
    };
}

const FWaterVolumeInfo* FWaterVolume::AllVolumes(u32& out_count) const noexcept {
    RebuildPackedCacheIfNeeded();
    out_count = static_cast<u32>(m_PackedCache.Size());
    return out_count > 0 ? m_PackedCache.Data() : nullptr;
}

void FWaterVolume::ClearAll() noexcept {
    m_Slots.Clear();
    m_PackedCache.Clear();
    m_VolumeCount = 0;
    m_CacheDirty  = false;   // 空状態は dirty 不要
}

// ----- private キャッシュ --------------------------------------------------

void FWaterVolume::RebuildPackedCacheIfNeeded() const noexcept {
    if (!m_CacheDirty) return;
    m_PackedCache.Clear();
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const Slot& s = m_Slots[i];
        if (s.active) m_PackedCache.PushBack(s.info);
    }
    m_CacheDirty = false;
}

} // namespace acs::game
