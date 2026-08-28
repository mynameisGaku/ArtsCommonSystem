// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_RAY_MARCH_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_RAY_MARCH_INTERNAL_H

#include "render/Sky.h"

namespace acs::render_internal {

/** GPU中心レイと同じ二雲帯・整数セル割当てをCPUで検証する計画。 */
struct FVolumetricCloudRayMarchPlanInternal {
    /** 一つの物理雲帯に割り当てた連続区間と細密セル。 */
    struct FInterval {
        /** 雲帯へ入る視線距離。 */
        f32 enter = 0.0f;

        /** 雲帯を出るか、局所視程で打ち切る視線距離。 */
        f32 exit = 0.0f;

        /** 区間を端から端まで分割する細密セル数。 */
        u32 fine_cell_count = 0u;

        /** 通常の細密セル幅。区間終端の一セルだけが端数を吸収する。 */
        f32 fine_step = 0.0f;

        /** 下層=0、上層=1を表す物理雲帯ID。 */
        u32 physical_band_id = 0u;
    };

    /** 視点から近い順に並べた有効区間。 */
    FInterval intervals[2]{};

    /** 局所視程を反映した実際の打ち切り距離。 */
    f32 maximum_distance = 0.0f;

    /** 距離順で最初の区間に要求した細密刻み。各区間の値はfine_stepに保持する。 */
    f32 requested_fine_step = 0.0f;

    /** 最初の区間入口における距離減衰。 */
    f32 visibility = 0.0f;

    /** 有効な連続区間数。 */
    u32 interval_count = 0u;

    /** 全区間へ割り当てた細密セル数の合計。 */
    u32 total_fine_cell_count = 0u;

    /** 一視線に許可した反復数。 */
    u32 maximum_samples = kVolumetricCloudMaxViewMarchSamples;

    /** 打ち切り距離内に積分可能な区間があるか。 */
    bool hit = false;
};

/**
 * GPU中心レイと同じ外殻接線許容差・物理雲帯別予算で、局所視程と距離刻み拡大を含む計画を作る。
 * 画素幅で滑らかにする地面の地平線被覆はGPUだけで評価し、この計画は中心方向の幾何判定を返す。
 *
 * @param ray_origin 光線の始点。
 * @param ray_direction 光線の方向。関数内で正規化する。
 * @param lower_layer 下層設定。
 * @param upper_layer 上層設定。
 * @param has_upper_layer 上層を交差対象へ含めるか。
 * @param range 描画距離、境界減衰、遠距離刻み拡大。
 * @param world_origin 曲面雲層の基準原点。
 * @param maximum_samples 一視線の反復上限。0はrangeまたは通常既定値を使う。
 * @return 距離順の区間と、上限内へ整数で割り当てた細密セル。
 */
FVolumetricCloudRayMarchPlanInternal PlanVolumetricCloudRayMarch_Internal(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& lower_layer,
    const FVolumetricCloudUpperLayer& upper_layer,
    bool has_upper_layer, const FVolumetricCloudRange& range,
    FVec3 world_origin = FVec3{},
    u32 maximum_samples = 0u) noexcept;

/**
 * GPUと同じq形式で、単精度球面方程式の二交点を求める。
 * 外殻だけは負の判別式を相対許容差内で接線候補へ丸められる。内殻ではfalseを指定し、
 * 実際には内殻を外す雲中レイを偽の区間終端へ変えない。
 */
bool ResolveVolumetricCloudSphereRoots_Internal(
    f32 center_dot, f32 shell_c, bool accept_rounded_outer_tangent,
    f32& near_distance, f32& far_distance) noexcept;

/**
 * GPU主描画と同じ単精度係数で、一つの物理雲帯の最近接連続区間を求める。
 * @param shell_local_origin 曲面雲層の基準原点から見た始点。
 * @param normalized_direction 正規化済みの方向。
 * @param layer 対象の物理雲帯。
 * @return 正方向に存在する最近接区間。交差しない場合はhit=false。
 */
FVolumetricCloudRayInterval ResolveVolumetricCloudShellInterval_Internal(
    FVec3 shell_local_origin, FVec3 normalized_direction,
    const FVolumetricCloudLayer& layer) noexcept;

/**
 * 有効な上下雲帯へ、最低探索量を含む物理帯域別の標本予算を予約する。
 * @param lower_layer 下層設定。
 * @param upper_layer 上層設定。
 * @param has_upper_layer 上層を予算へ含めるか。
 * @param maximum_samples 全帯域で共有する標本上限。
 * @param lower_budget 下層へ予約した標本数の書き込み先。
 * @param upper_budget 上層へ予約した標本数の書き込み先。
 */
void ResolveVolumetricCloudPhysicalBandBudgets_Internal(
    const FVolumetricCloudLayer& lower_layer,
    const FVolumetricCloudUpperLayer& upper_layer,
    bool has_upper_layer, u32 maximum_samples,
    u32& lower_budget, u32& upper_budget) noexcept;

} // namespace acs::render_internal

#endif // ACS_RENDER_VOLUMETRIC_CLOUD_RAY_MARCH_INTERNAL_H
