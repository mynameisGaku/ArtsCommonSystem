// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionWorld3D.h"

namespace acs::game {

/** 3D sphereの反復貫通解消結果。 */
struct FSpherePenetrationResolution3D {
    /** 一回のqueryで許可する反復回数の上限。 */
    static constexpr u32 kMaximumIterations = 64u;

    /** 反復分離後のworld空間sphere。 */
    FSphere ResolvedSphere{};

    /** 入力sphereからResolvedSphereまでのworld空間移動量。 */
    FVec3 Translation{};

    /** 実際に分離移動を適用した回数。 */
    u32 IterationCount = 0u;

    /** 対象layerのshapeへ正の深さで貫通していなければtrue。 */
    bool FullyResolved = false;
};

/**
 * worldを変更せず、最深接触から順にsphereの貫通を反復解消する。
 *
 * @param world 問い合わせる3D collision world。
 * @param sphere 解消対象のworld空間sphere。
 * @param out_result 解消後sphere、総移動量、反復回数、収束状態の書き込み先。
 * @param max_iterations 許可する分離移動回数。0は状態確認だけを行い、上限はkMaximumIterations。
 * @param exclude queryから除外するshape。無効またはstaleなら除外なし。
 * @param mask layerとのANDが0でないshapeだけを対象にするmask。
 * @return 入力と計算結果が有限で処理を完了できた場合true。未収束はFullyResolvedで返す。
 */
bool TryResolveSpherePenetrations3D(const CCollisionWorld3D& world, const FSphere& sphere, FSpherePenetrationResolution3D& out_result, u32 max_iterations = 4u, FCollisionShapeId3D exclude = {}, u32 mask = CCollisionWorld3D::kAllLayers) noexcept;

} // namespace acs::game
