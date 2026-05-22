// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q Phase 3 — WaterVolume (浮力 + 水域判定)
//
// AABB で定義された 2D 水域を複数登録し、点が水中に居るかの query と、
// 物体に掛かる浮力 + drag を世界座標 force ベクトルとして返す。
// PhysicsBody2D / EffectSystem 側は本クラスを参照することで「浮く」「沈む」「水流
// 抵抗」といった水中挙動を組み立てられる。水面演出 (波 / splash 粒子) はレンダラ
// 側で `WaterVolumeInfo::surface_y` と `water_color` を pull して描画する想定。
//
// 使い方:
//   acs::game::WaterVolume water;
//
//   // 池を 1 つ登録
//   acs::game::WaterVolumeInfo pond{
//       /*center=*/      {0.0f, -50.0f},
//       /*half_size=*/   {200.0f, 50.0f},
//       /*buoyancy=*/    9.8f,        // 重力と同程度の浮力強度
//       /*drag=*/        2.0f,        // 水中減衰係数
//       /*surface_y=*/    0.0f,        // 水面 y 座標 (= center.y + half_size.y)
//       /*water_color=*/ {0.1f, 0.3f, 0.6f},
//   };
//   auto id = water.AddVolume(pond);
//
//   // 毎フレーム、PhysicsBody2D に浮力を適用
//   if (water.IsUnderwater(body.position)) {
//       Vec2 f = water.ComputeBuoyancyForce(body.position, body.velocity, body.mass);
//       body.ApplyForce(f);
//   }
//
// 設計選択 (Pillar Q Phase 3):
//   ・**WaterVolumeId は ShapeId / NodeId と同パターン**: 24bit index + 8bit
//     generation。remove → re-add で slot 再利用しても旧 handle は無効化される。
//   ・**broad phase は線形走査**: Phase 3 では全 volume を直接走査 (典型 N ≤ 数
//     十)。SpatialGrid 化は Phase 4 で。
//   ・**浮力モデル = 単純な「水深×強さ×質量」の上向き力**: depth = surface_y -
//     pos.y。depth が大きいほど浮力大きい。実 Archimedes の「排除体積」ではなく
//     ゲーム向けの簡易モデル。volume 毎に `buoyancy_strength` で調整可。
//   ・**drag**: velocity に対し `-velocity * drag` を加算。水中での運動減衰を
//     表現。`drag` は加速度ではなく力係数 (kg / s 単位の粘性的減衰) として扱う。
//   ・**surface_y は info に明示保持**: center.y + half_size.y で算出も可能だが、
//     利用者が「center 中央ではなく水面が y=0」のような幾何を作る場合に便利な
//     よう冗長保持。AddVolume 時に整合しないと判定がズレる可能性がある点を
//     コメントで注意。
//   ・**non-overlap な volume 前提**: 複数 volume が空間的に重なる場合、IsUnderwater
//     は最初に当たった volume だけを採用 (短絡)。浮力は重なる全 volume の合計。
//   ・**非コピー・非ムーブ**: handle 安定性のため複製禁止。
//   ・**全関数 noexcept、STL 不使用**: ACS 全体規約。
//
// 範囲外 (Phase 4+ で):
//   ・two-way coupling (物体が水面に波を作る)
//   ・SPH / 流体シミュレーション
//   ・水流ベクトル場 (current) の追加
//   ・volume 形状の Circle / OBB 化
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

/// 1 つの水域の幾何 + パラメータ。WaterVolume::AddVolume / UpdateVolume で値渡し。
struct WaterVolumeInfo {
    Vec2 center             {0.0f, 0.0f};   // AABB 中心 (world)
    Vec2 half_size          {1.0f, 1.0f};   // AABB 半サイズ (world、両軸 > 0 想定)
    f32  buoyancy_strength  = 9.8f;         // 浮力強度 (重力相当)
    f32  drag               = 1.0f;         // 水中 drag 係数 (kg/s 粘性)
    f32  surface_y          = 1.0f;         // 水面 y 座標 (= center.y + half_size.y が自然)
    Vec3 water_color        {0.1f, 0.3f, 0.6f}; // レンダラが水面描画に使う色 (linear RGB)
};

/// WaterVolume を識別する packed 32bit handle (generational)。
/// レイアウトは ShapeId と同一 (low24=index, high8=generation)。
struct WaterVolumeId {
    u32 _packed = 0;   // 0 = invalid

    constexpr WaterVolumeId() noexcept = default;
    constexpr WaterVolumeId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index() const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(_packed >> 24);
    }
    /// invalid (packed == 0) でなければ true。
    bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(WaterVolumeId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(WaterVolumeId o) const noexcept { return _packed != o._packed; }
};

class WaterVolume {
public:
    WaterVolume() noexcept = default;
    ~WaterVolume() noexcept = default;

    // 非コピー・非ムーブ (handle 安定性のため)
    WaterVolume(const WaterVolume&)            = delete;
    WaterVolume& operator=(const WaterVolume&) = delete;
    WaterVolume(WaterVolume&&)                 = delete;
    WaterVolume& operator=(WaterVolume&&)      = delete;

    // ----- Volume 登録 -----
    /// info の値を内部 slot に複製。返り値は以降の参照 / 更新に使う handle。
    WaterVolumeId AddVolume(const WaterVolumeInfo& info) noexcept;

    /// volume 削除。slot は再利用、generation が進む。stale handle は何もしない。
    void RemoveVolume(WaterVolumeId id) noexcept;

    /// 幾何のみ更新 (浮力強度 / drag / surface_y / color は不変)。stale handle は無視。
    /// `surface_y` は内部で自動更新せず info.surface_y を引き継ぐ点に注意。
    /// 水面位置を変えたい場合は Remove → Add で再登録すること。
    void UpdateVolume(WaterVolumeId id, Vec2 center, Vec2 half_size) noexcept;

    // ----- クエリ -----
    /// 任意の volume の AABB 内に pos が入っていれば true。
    bool IsUnderwater(Vec2 pos) const noexcept;

    /// 「水面からの沈み深さ」を返す。surface_y - pos.y。
    /// 戻り値 >= 0 で潜水中。水中でない (どの volume にも属さない) ときは 0。
    /// 複数 volume が重なる場合、最初に AABB 内に居る volume の surface_y を採用。
    f32 SubmersionDepth(Vec2 pos) const noexcept;

    /// pos / velocity / mass の物体に掛かる合計浮力 + drag 力を返す (world force)。
    /// 浮力  = (0, +buoyancy_strength * mass * depth)  ※ +Y 上向き想定
    /// drag  = -velocity * drag
    /// 複数 volume が重なる場合は全 volume の寄与を加算。
    /// pos が水中でない場合は (0,0)。
    Vec2 ComputeBuoyancyForce(Vec2 pos, Vec2 velocity, f32 mass) const noexcept;

    // ----- 全 volume 列挙 / 情報取得 -----
    /// active な volume 数。
    u32 VolumeCount() const noexcept { return _volume_count; }

    /// active な全 volume を packed array で返す (内部表現と異なる)。
    /// out_count に書き込んだ要素数を返し、戻り値は先頭ポインタ。
    /// VolumeCount() == 0 なら nullptr / out_count=0。
    /// Add/Remove/Update 直後に呼ぶことを想定 (内部 cache を返すので Add 等で
    /// 無効化される)。
    const WaterVolumeInfo* AllVolumes(u32& out_count) const noexcept;

    /// 全 volume を破棄。slot 配列も解放。
    void ClearAll() noexcept;

private:
    struct Slot {
        WaterVolumeInfo info{};
        bool            active = false;
        u8              gen    = 0;
    };

    u32  AcquireSlot() noexcept;
    /// AllVolumes() 用に active な info を `_packed_cache` に詰める。
    void RebuildPackedCacheIfNeeded() const noexcept;

    Array<Slot> _slots;
    u32         _volume_count = 0;

    // AllVolumes() の戻り値用 cache。Add/Update/Remove で dirty になる。
    // mutable なのは const なクエリ API から lazy build したいため。
    mutable Array<WaterVolumeInfo> _packed_cache;
    mutable bool                   _cache_dirty = true;
};

} // namespace acs::game
