// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/ANode.h"
#include "gameframework/LightComponent3D.h"
#include "render/StandardShader.h"   // FDirLight / FPointLight

namespace acs::game {

/**
 * ノードの木から光を集めて、shader へ渡せる配列にする。
 *
 * @details
 * `ALightComponent3D` はノードに付いた 1 灯を表すだけで、shader は配列で受け取る。
 * その間を埋める。**確保はしない** (shader 側の上限ぶんを内側に持つ)。
 *
 * shader が受け取れるのは平行光源 4 灯 + 点光源 4 灯まで。**越えたぶんは捨てる**が、
 * 捨てた数を `DroppedCount()` で返す。黙って消すと「置いたのに光らない」の原因が
 * 分からなくなるため。
 *
 * 点光源が上限を越えるときは、**視点に近いものを残す**。遠くの光は画面への寄与が小さく、
 * 宣言順で切ると「近くの光が消えて遠くの光が残る」という一番おかしな見え方になる。
 *
 * @code
 * CLightCollector3D lights;
 * lights.CollectFrom(scene.Root(), camera.EffectiveEye());
 *
 * shader.SetLights(view_projection, eye, lights.DirectionalLights(), lights.DirectionalCount(), ambient);
 * shader.SetPointLights(lights.PointLights(), lights.PointCount());
 * @endcode
 */
class CLightCollector3D {
public:
    /** shader が受け取れる平行光源の数。 */
    static constexpr u32 kMaxDirectional = 4u;

    /** shader が受け取れる点光源の数。 */
    static constexpr u32 kMaxPoint = 4u;

    /** これより深い階層は辿らない (壊れた木で戻ってこなくなるのを防ぐ)。 */
    static constexpr u32 kMaxDepth = 512u;

    /** 空の状態で構築する。 */
    CLightCollector3D() noexcept = default;

    /** 集めた結果を捨てる。 */
    void Clear() noexcept {
        m_DirectionalCount = 0u;
        m_PointCount       = 0u;
        m_Dropped          = 0u;
    }

    /**
     * 木を辿って光を集める。
     *
     * @details
     * 先に `Clear()` するので、毎フレーム呼んでよい。光っていない灯 (強さ 0) は飛ばす。
     * @param root 辿り始めるノード。root 自身も対象。
     * @param view_position 視点のワールド位置。点光源が上限を越えたとき、ここに近いものを残す。
     */
    void CollectFrom(const ANode& root, FVec3 view_position = FVec3{ 0.0f, 0.0f, 0.0f }) noexcept {
        Clear();
        Visit(root, view_position, 0u);
    }

    /**
     * 平行光源の配列を返す。
     *
     * @return 先頭。個数は `DirectionalCount()`。
     */
    const FDirLight* DirectionalLights() const noexcept { return m_Directional; }

    /**
     * 集まった平行光源の数を返す。
     *
     * @return 0 から `kMaxDirectional` まで。
     */
    u32 DirectionalCount() const noexcept { return m_DirectionalCount; }

    /**
     * 点光源の配列を返す。
     *
     * @return 先頭。個数は `PointCount()`。
     */
    const FPointLight* PointLights() const noexcept { return m_Point; }

    /**
     * 集まった点光源の数を返す。
     *
     * @return 0 から `kMaxPoint` まで。
     */
    u32 PointCount() const noexcept { return m_PointCount; }

    /**
     * 上限を越えて捨てた灯の数を返す。
     *
     * @details
     * 0 でなければ、置いたのに反映されていない光がある。診断に出すこと。
     * @return 捨てた数。
     */
    u32 DroppedCount() const noexcept { return m_Dropped; }

private:
    /**
     * ノード 1 つとその子を辿る。
     *
     * @param node 見るノード。
     * @param view_position 視点のワールド位置。
     * @param depth いまの深さ。
     */
    void Visit(const ANode& node, FVec3 view_position, u32 depth) noexcept {
        if (depth >= kMaxDepth) return;

        if (const ALightComponent3D* const light = const_cast<ANode&>(node).GetComponent<ALightComponent3D>()) {
            Add(*light, view_position);
        }

        const u32 count = node.ChildCount();
        for (u32 i = 0u; i < count; ++i) {
            const ANode* const child = node.Child(i);
            if (child != nullptr) Visit(*child, view_position, depth + 1u);
        }
    }

    /**
     * 光 1 灯を配列へ入れる。
     *
     * @param light 入れる灯。
     * @param view_position 視点のワールド位置。
     */
    void Add(const ALightComponent3D& light, FVec3 view_position) noexcept {
        if (!light.IsEmitting()) return;

        if (light.LightKind() == ELight3DKind::Directional) {
            FDirLight out{};
            if (!light.FillDirectional(out)) return;

            // 太陽はまず 1 灯なので、越えたら宣言順で切る。
            if (m_DirectionalCount >= kMaxDirectional) { ++m_Dropped; return; }
            m_Directional[m_DirectionalCount] = out;
            ++m_DirectionalCount;
            return;
        }

        FPointLight out{};
        if (!light.FillPoint(out)) return;

        if (m_PointCount < kMaxPoint) {
            m_Point[m_PointCount]         = out;
            m_PointDistance[m_PointCount] = DistanceSquared(out.position, view_position);
            ++m_PointCount;
            return;
        }

        // 満杯。いちばん遠いものより近ければ入れ替える。遠い光を残すと、
        // 近くの光が消えて «一番おかしな見え方» になる。
        u32 farthest = 0u;
        for (u32 i = 1u; i < m_PointCount; ++i) {
            if (m_PointDistance[i] > m_PointDistance[farthest]) farthest = i;
        }

        const f32 distance = DistanceSquared(out.position, view_position);
        if (distance < m_PointDistance[farthest]) {
            m_Point[farthest]         = out;
            m_PointDistance[farthest] = distance;
        }

        ++m_Dropped;
    }

    /** 2 点の距離の 2 乗を返す。 */
    static f32 DistanceSquared(FVec3 a, FVec3 b) noexcept {
        const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; // 各軸の差。
        return dx * dx + dy * dy + dz * dz;
    }

    /** 集まった平行光源。 */
    FDirLight m_Directional[kMaxDirectional] = {};

    /** 集まった点光源。 */
    FPointLight m_Point[kMaxPoint] = {};

    /** 点光源それぞれの視点からの距離 (2 乗)。入れ替えの判断に使う。 */
    f32 m_PointDistance[kMaxPoint] = {};

    /** 集まった平行光源の数。 */
    u32 m_DirectionalCount = 0u;

    /** 集まった点光源の数。 */
    u32 m_PointCount = 0u;

    /** 上限を越えて捨てた数。 */
    u32 m_Dropped = 0u;
};

} // namespace acs::game
