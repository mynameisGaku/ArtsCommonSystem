// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FWaterVolume (浮力 + 水域判定)
//
// AABB で定義された 2D 水域を複数登録し、点が水中に居るかの query と、
// 物体に掛かる浮力 + drag を世界座標 force ベクトルとして返す。
// FPhysicsBody2D / FEffectSystem 側は本クラスを参照することで「浮く」「沈む」「水流
// 抵抗」といった水中挙動を組み立てられる。水面演出 (波 / splash 粒子) はレンダラ
// 側で `FWaterVolumeInfo::surface_y` と `water_color` を pull して描画する想定。
//
// 使い方:
//   acs::game::FWaterVolume water;
//
//   // 池を 1 つ登録
//   acs::game::FWaterVolumeInfo pond{
//       /*center=*/      {0.0f, 50.0f},
//       /*half_size=*/   {200.0f, 50.0f},
//       /*buoyancy=*/    9.8f,        // 重力と同程度の浮力強度
//       /*drag=*/        2.0f,        // 水中減衰係数
//       /*surface_y=*/    0.0f,        // 水面 y 座標 (= center.y - half_size.y、+Y=画面下なので水面=最小 y)
//       /*water_color=*/ {0.1f, 0.3f, 0.6f},
//   };
//   auto id = water.AddVolume(pond);
//
//   // 毎フレーム、FPhysicsBody2D に浮力を適用
//   if (water.IsUnderwater(body.position)) {
//       FVec2 f = water.ComputeBuoyancyForce(body.position, body.velocity, body.mass);
//       body.ApplyForce(f);
//   }
//
// 設計選択 (Pillar Q):
//   ・**FWaterVolumeId は FShapeId / FNodeId と同パターン**: 24bit index + 8bit
//     generation。remove → re-add で slot 再利用しても旧 handle は無効化される。
//   ・**broad phase は線形走査**: 全 volume を直接走査 (典型 N ≤ 数十)。
//   ・**浮力モデル = 単純な「水深×強さ×質量」の上向き力**: depth = pos.y -
//     surface_y (+Y=画面下: 水面=最小y、沈むほど y 大)。depth が大きいほど浮力大きい。実 Archimedes の「排除体積」ではなく
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
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 1 つの水域の幾何 + パラメータ。
 *
 * @details FWaterVolume::AddVolume / UpdateVolume で値渡しされる。
 */
struct FWaterVolumeInfo {
    /** AABB 中心 (world)。 */
    FVec2 center             {0.0f, 0.0f};

    /** AABB 半サイズ (world、両軸 > 0 想定)。 */
    FVec2 half_size          {1.0f, 1.0f};

    /** 浮力強度 (重力相当)。 */
    f32  buoyancy_strength  = 9.8f;

    /** 水中 drag 係数 (kg/s 粘性)。 */
    f32  drag               = 1.0f;

    /** 水面 y 座標 (= center.y - half_size.y が自然、+Y=画面下)。 */
    f32  surface_y          = -1.0f;

    /** レンダラが水面描画に使う色 (linear RGB)。 */
    FVec3 water_color        {0.1f, 0.3f, 0.6f};
};

/**
 * FWaterVolume を識別する packed 32bit handle (generational)。
 *
 * @details レイアウトは FShapeId と同一 (low24=index, high8=generation)。
 */
struct FWaterVolumeId {
    /** packed 値 (0 = invalid)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な handle を構築する。 */
    constexpr FWaterVolumeId() noexcept = default;

    /**
     * index と generation を packing して handle を構築する。
     *
     * @param index slot index (低 24bit)。
     * @param gen generation (高 8bit)。
     */
    constexpr FWaterVolumeId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot index を取り出す。
     *
     * @return 低 24bit の index。
     */
    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を取り出す。
     *
     * @return 高 8bit の generation。
     */
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * 有効な handle かを返す。
     *
     * @return invalid (m_Packed == 0) でなければ true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 等価比較する。
     *
     * @param o 比較対象の handle。
     * @return packed 値が一致すれば true。
     */
    constexpr bool operator==(FWaterVolumeId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等価比較する。
     *
     * @param o 比較対象の handle。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FWaterVolumeId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * AABB で定義された 2D 水域を複数登録し、水中判定と浮力 + drag 力を返す。
 *
 * @details
 * 各 volume は slot+generation handle (FWaterVolumeId) で識別され、broad phase は
 * 全 volume の線形走査 (典型 N ≤ 数十)。浮力は depth = pos.y - surface_y に比例する
 * 簡易モデルで、複数 volume が重なる場合は全 volume の寄与を加算する。handle 安定性
 * のため非コピー・非ムーブ。
 */
class FWaterVolume {
public:
    /** 空状態で構築する。 */
    FWaterVolume() noexcept = default;

    /** 破棄する。 */
    ~FWaterVolume() noexcept = default;

    /** コピー禁止 (handle 安定性のため)。 */
    FWaterVolume(const FWaterVolume&)            = delete;

    /** コピー代入も禁止。 */
    FWaterVolume& operator=(const FWaterVolume&) = delete;

    /** ムーブ禁止 (handle 安定性のため)。 */
    FWaterVolume(FWaterVolume&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FWaterVolume& operator=(FWaterVolume&&)      = delete;

    /**
     * info の値を内部 slot に複製して volume を登録する。
     *
     * @param info 登録する水域の幾何 + パラメータ。
     * @return 以降の参照 / 更新に使う handle。
     */
    FWaterVolumeId AddVolume(const FWaterVolumeInfo& info) noexcept;

    /**
     * volume を削除する (slot は再利用され generation が進む)。
     *
     * @details stale handle を渡しても何もしない。
     * @param id 削除する volume の handle。
     */
    void RemoveVolume(FWaterVolumeId id) noexcept;

    /**
     * volume の幾何 (center / half_size) のみを更新する。
     *
     * @details
     * 浮力強度 / drag / surface_y / color は不変で info.surface_y を引き継ぐ。
     * 水面位置を変えたい場合は Remove → Add で再登録すること。stale handle は無視する。
     * @param id 更新する volume の handle。
     * @param center 新しい AABB 中心 (world)。
     * @param half_size 新しい AABB 半サイズ (world)。
     */
    void UpdateVolume(FWaterVolumeId id, FVec2 center, FVec2 half_size) noexcept;

    /**
     * pos がいずれかの volume の AABB 内に入っているかを返す。
     *
     * @param pos 判定する点 (world)。
     * @return 任意の volume の AABB 内なら true。
     */
    bool IsUnderwater(FVec2 pos) const noexcept;

    /**
     * 水面からの沈み深さを返す。
     *
     * @details
     * depth = pos.y - surface_y (+Y=画面下: 沈むと y 大)。負値は 0 にクランプする。
     * 複数 volume が重なる場合、最初に AABB 内に居る volume の surface_y を採用する。
     * @param pos 判定する点 (world)。
     * @return 沈み深さ (>= 0)。水中でないときは 0。
     */
    f32 SubmersionDepth(FVec2 pos) const noexcept;

    /**
     * pos / velocity / mass の物体に掛かる合計浮力 + drag 力を返す (world force)。
     *
     * @details
     * 浮力 = (0, -Σ(buoyancy_strength * depth) * mass) (上向き = -Y、Y-down: +Y=画面下)、
     * drag = -velocity * Σ(drag)。複数 volume が重なる場合は全 volume の寄与を加算する。
     * @param pos 物体の位置 (world)。
     * @param velocity 物体の速度 (world)。
     * @param mass 物体の質量。
     * @return 合計の浮力 + drag 力。pos が水中でない場合は (0,0)。
     */
    FVec2 ComputeBuoyancyForce(FVec2 pos, FVec2 velocity, f32 mass) const noexcept;

    /**
     * active な volume 数を返す。
     *
     * @return active な volume 数。
     */
    u32 VolumeCount() const noexcept { return m_VolumeCount; }

    /**
     * active な全 volume を packed array で返す (内部表現とは別の連続配列)。
     *
     * @details
     * 内部 cache を返すので Add/Remove/Update で無効化される。Add 等の直後に呼ぶこと。
     * @param out_count 書き込んだ要素数の出力先。
     * @return 先頭ポインタ。VolumeCount() == 0 なら nullptr (out_count=0)。
     */
    const FWaterVolumeInfo* AllVolumes(u32& out_count) const noexcept;

    /** 全 volume を破棄し slot 配列も解放する。 */
    void ClearAll() noexcept;

private:
    /** 1 volume 分の slot (info + active フラグ + generation)。 */
    struct Slot {
        /** この slot が保持する水域情報。 */
        FWaterVolumeInfo info{};

        /** この slot が使用中か。 */
        bool            active = false;

        /** slot の generation (handle 検証用)。 */
        u8              gen    = 0;
    };

    /**
     * 空き slot を確保する (なければ末尾に追加)。
     *
     * @return 確保した slot の index (index 0 は invalid handle に予約)。
     */
    u32  AcquireSlot() noexcept;

    /** AllVolumes() 用に active な info を m_PackedCache に詰め直す (dirty 時のみ)。 */
    void RebuildPackedCacheIfNeeded() const noexcept;

    /** volume の slot 配列 (index 0 は invalid 予約)。 */
    TArray<Slot> m_Slots;

    /** active な volume 数。 */
    u32         m_VolumeCount = 0;

    /** AllVolumes() の戻り値用 cache (const クエリから lazy build するため mutable)。 */
    mutable TArray<FWaterVolumeInfo> m_PackedCache;

    /** m_PackedCache が古くなっているか (Add/Update/Remove で true になる)。 */
    mutable bool                   m_CacheDirty = true;
};

} // namespace acs::game
