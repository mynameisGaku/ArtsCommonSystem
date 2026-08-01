// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — CWaterVolume 実装
//
// AABB ベースの水域管理 + 浮力計算。slot+generation handle pattern は
// CCollisionWorld2D / CTriggerWorld2D と同等。線形走査 (典型 N ≤ 数十)。
#include "gameframework/WaterVolume.h"

namespace acs::game {

/**
 * AABB と点の包含判定を行う。
 *
 * @details
 * center / half_size から min/max を構築して比較する。half_size 各成分は非負想定で、
 * 負値が来ても絶対値を取らず比較に任せる (= 必ず外れて安全側に倒れる)。
 * @param v 判定対象の水域情報。
 * @param pos 判定する点 (world)。
 * @return pos が v の AABB 内なら true。
 */
static bool ContainsPoint(const FWaterVolumeInfo& v, FVec2 pos) noexcept {
    const f32 dx = pos.x - v.center.x;
    const f32 dy = pos.y - v.center.y;
    return (dx >= -v.half_size.x) && (dx <= v.half_size.x)
        && (dy >= -v.half_size.y) && (dy <= v.half_size.y);
}

/** 空き slot を探して返す (index 0 は invalid handle に予約)。 */
u32 CWaterVolume::AcquireSlot() noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** slot を確保し info を複製、generation を進めて handle を返す。 */
FWaterVolumeId CWaterVolume::AddVolume(const FWaterVolumeInfo& info) noexcept {
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.info   = info;
    s.gen    = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;     // 0 を予約値として避ける (Add 時の安全弁)
    s.active = true;
    ++m_VolumeCount;
    m_CacheDirty = true;
    return FWaterVolumeId{idx, s.gen};
}

/** handle を検証して slot を非 active 化する (stale handle は無視)。 */
void CWaterVolume::RemoveVolume(FWaterVolumeId id) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.active = false;
    if (m_VolumeCount > 0) --m_VolumeCount;
    m_CacheDirty = true;
}

/** handle を検証して center / half_size のみ更新する (他のパラメータは不変)。 */
void CWaterVolume::UpdateVolume(FWaterVolumeId id, FVec2 center, FVec2 half_size) noexcept {
    if (!id.IsValid() || id.Index() >= m_Slots.Size()) return;
    FSlot& s = m_Slots[id.Index()];
    if (!s.active || s.gen != id.Generation()) return;
    s.info.center    = center;
    s.info.half_size = half_size;
    // surface_y / buoyancy_strength / drag / water_color は info から不変。
    // 水面位置を変えたい時は Remove → Add で。
    m_CacheDirty = true;
}

/** 全 active volume を線形走査し、pos を含む volume があれば true を返す。 */
bool CWaterVolume::IsUnderwater(FVec2 pos) const noexcept {
    // index 0 は invalid 予約なので 1 から走査。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const FSlot& s = m_Slots[i];
        if (!s.active) continue;
        if (ContainsPoint(s.info, pos)) return true;
    }
    return false;
}

/** 最初に pos を含む volume の surface_y からの沈み深さ (>= 0) を返す。 */
f32 CWaterVolume::SubmersionDepth(FVec2 pos) const noexcept {
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const FSlot& s = m_Slots[i];
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

/** pos を含む全 volume の浮力 + drag を合算して world force を返す。 */
FVec2 CWaterVolume::ComputeBuoyancyForce(FVec2 pos, FVec2 velocity, f32 mass) const noexcept {
    // 全 volume の寄与を加算。重なる volume があれば力も重畳。
    f32  total_depth_strength = 0.0f;  // Σ (buoyancy_strength * depth) を蓄積
    f32  total_drag           = 0.0f;  // Σ drag を蓄積
    bool in_water             = false;

    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const FSlot& s = m_Slots[i];
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

/** packed cache を必要なら再構築し、先頭ポインタと要素数を返す。 */
const FWaterVolumeInfo* CWaterVolume::AllVolumes(u32& out_count) const noexcept {
    RebuildPackedCacheIfNeeded();
    out_count = static_cast<u32>(m_PackedCache.Size());
    return out_count > 0 ? m_PackedCache.Data() : nullptr;
}

/** slot 配列と cache を解放し volume カウントを 0 に戻す。 */
void CWaterVolume::ClearAll() noexcept {
    m_Slots.Clear();
    m_PackedCache.Clear();
    m_VolumeCount = 0;
    m_CacheDirty  = false;   // 空状態は dirty 不要
}

/** dirty なら active な info を m_PackedCache に詰め直す。 */
void CWaterVolume::RebuildPackedCacheIfNeeded() const noexcept {
    if (!m_CacheDirty) return;
    m_PackedCache.Clear();
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        const FSlot& s = m_Slots[i];
        if (s.active) m_PackedCache.PushBack(s.info);
    }
    m_CacheDirty = false;
}

} // namespace acs::game
