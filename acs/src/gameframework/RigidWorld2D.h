// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar F — FRigidWorld2D (2D 剛体ダイナミクス、線形インパルス)
// -----------------------------------------------------------------------------
// APhysicsBody2D (swept kinematic) とは別の「本物の剛体」。質量を持つ動的ボディが
// 衝突時に**運動量を交換**する (跳ね返り / 積み重ね / 押し合い)。インパルスベースの
// 接触ソルバ + 位置補正で、落下・反発・静止を再現する。
//
// 範囲:
//   ・形状は **円 / 有向ボックス (OBB) / 凸ポリゴン**、動的・静的いずれも。回転ダイナミクス対応。
//   ・接触は円-円 / 円-凸 / 凸-凸 (SAT + 面クリッピング) を自前マニフォルド (法線 + めり込み深さ +
//     最大 2 接触点) で解く。AABB は angle=0 の OBB、有向ボックスは SetAngle で回す。
//   ・restitution (反発係数) + Coulomb 摩擦 + 角運動 (慣性モーメント) に対応。
//   ・**CCD (連続衝突判定)** は SetCcd でボディ単位に opt-in。高速な動的円が薄い静的形状を
//     すり抜けるトンネリングを、位置積分前のスウィープ (TOI クランプ) で防ぐ。既定 off。
//
// 使い方:
//   FRigidWorld2D w;
//   u32 floor = w.AddStaticAabb({0, 10}, {10, 0.5f});      // 上端 y=9.5 の床
//   u32 ball  = w.AddCircle({0, 0}, 0.5f, /*mass=*/1.0f, /*restitution=*/0.2f);
//   for (...) w.Step(1.0f/60.0f, FVec2{0, 10});            // +Y=下: 重力は +Y
//   FVec2 p = w.Position(ball);                            // 落ちて床上で静止
//
// 規約: no-STL / no-exceptions / 全 noexcept / 固定長 TArray。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 剛体ボディ 1 つの状態 (円 or AABB、動的 or 静的)。
 *
 * @details 角運動 (angle/ang_vel/inv_inertia) はダイナミクスと transform に効く。
 * 衝突形状は回転しない (円は回転不変、AABB は軸並行のまま = 近似。OBB は今後)。
 */
/** 凸ポリゴンコライダーの最大頂点数。 */
inline constexpr u32 kMaxPolyVerts = 8u;

struct FRigidBodyState {
    FVec2 pos{0.0f, 0.0f};       // 中心位置
    FVec2 vel{0.0f, 0.0f};       // 速度 (静的は 0 固定)
    FVec2 half{0.0f, 0.0f};      // AABB 半サイズ (is_circle のとき未使用)
    f32   radius      = 0.0f;    // 円半径 (is_circle のとき)
    f32   angle       = 0.0f;    // 回転角 (rad)
    f32   ang_vel     = 0.0f;    // 角速度 (rad/s)
    f32   inv_mass    = 0.0f;    // 質量の逆数 (0 = 静的 / 無限質量)
    f32   inv_inertia = 0.0f;    // 慣性モーメントの逆数 (0 = 回転不可 / 静的)
    f32   restitution = 0.0f;    // 反発係数 [0,1]
    f32   friction    = 0.3f;    // 摩擦係数 (接触ペアで sqrt 合成)
    f32   lin_damp    = 0.0f;    // 線形減衰 (1/s)。0 = 減衰なし (運動量保存)
    f32   ang_damp    = 0.0f;    // 角減衰 (1/s)。回転が時間で自然に弱まる
    bool  is_circle   = true;    // true=円, false=AABB/Polygon
    bool  active      = true;    // false=削除済み (tombstone、再利用待ち)
    bool  is_sensor   = false;   // true=センサ (衝突応答せず素通り、重なりは検出可能)
    bool  ccd         = false;   // true=連続衝突判定 (高速移動でも静的形状をすり抜けない)
    // 凸ポリゴンコライダー (poly_count>0 のとき box の代わりに使う。CCW、body 原点基準のローカル頂点)。
    u8    poly_count  = 0u;
    FVec2 poly[kMaxPolyVerts]{};
};

/** レイキャストの結果 (hit=false でヒットなし)。 */
struct FRayHit2D {
    bool  hit      = false;
    f32   distance = 0.0f;       // origin からヒット点までの距離
    FVec2 point{0.0f, 0.0f};     // ヒット点 (world)
    FVec2 normal{0.0f, 0.0f};    // ヒット面の外向き法線 (単位)
    u32   body     = 0u;         // ヒットしたボディ index
};

/**
 * 2D 剛体ワールド。動的円ボディと静的 AABB を保持し、毎ステップで重力積分 →
 * 接触検出 → インパルス解決 → 位置補正を行う。空間クエリ (point / ray / overlap) も提供する。
 */
class FRigidWorld2D {
public:
    FRigidWorld2D() noexcept = default;

    /**
     * 動的な円ボディを追加する。
     * @param pos 中心位置。
     * @param radius 半径。
     * @param mass 質量 (<=0 で静的 = 無限質量)。
     * @param restitution 反発係数 [0,1]。
     * @return ボディindex。
     */
    u32 AddCircle(FVec2 pos, f32 radius, f32 mass, f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /**
     * 動的な AABB ボディ (箱・クレート) を追加する。
     * @param pos 中心位置。
     * @param half 半サイズ。
     * @param mass 質量 (<=0 で静的)。
     * @param restitution 反発係数 [0,1]。
     * @param friction 摩擦係数。
     * @return ボディ index。
     */
    u32 AddDynamicAabb(FVec2 pos, FVec2 half, f32 mass, f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /**
     * 静的な AABB (床・壁) を追加する (無限質量・不動)。
     * @param center 中心。
     * @param half 半サイズ。
     * @param restitution 反発係数 [0,1]。
     * @param friction 摩擦係数。
     * @return ボディ index。
     */
    u32 AddStaticAabb(FVec2 center, FVec2 half, f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /**
     * 凸ポリゴンボディを追加する (動的 or 静的)。
     *
     * @details local_verts は body 原点 (= pos) 基準のローカル頂点 (CCW、凸が前提)。
     * 頂点数は kMaxPolyVerts で頭打ち。mass<=0 で静的。慣性はポリゴン公式で算出。
     * @param pos 中心位置 (ローカル原点)。
     * @param local_verts ローカル頂点配列 (CCW 凸)。
     * @param count 頂点数 (3..kMaxPolyVerts)。
     * @param mass 質量 (<=0 で静的)。
     * @param restitution 反発係数 [0,1]。
     * @param friction 摩擦係数。
     * @return ボディ index (頂点不足は静的扱いの index)。
     */
    u32 AddPolygon(FVec2 pos, const FVec2* local_verts, u32 count, f32 mass,
                   f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /**
     * センサ円を追加する (衝突応答せず素通り。OverlapBody で重なりを検出)。
     * @param pos 中心。
     * @param radius 半径。
     * @return ボディ index。
     */
    u32 AddSensorCircle(FVec2 pos, f32 radius) noexcept;

    /**
     * センサ AABB (トリガ域) を追加する。
     * @param center 中心。
     * @param half 半サイズ。
     * @return ボディ index。
     */
    u32 AddSensorAabb(FVec2 center, FVec2 half) noexcept;

    /**
     * 1 ステップ進める (重力積分 → 位置更新 → 接触解決 → 位置補正)。
     * @param dt 時間刻み (秒)。
     * @param gravity 重力加速度 (+Y=画面下なら下向きは正の Y)。
     */
    void Step(f32 dt, FVec2 gravity) noexcept;

    /** アクティブ (未削除) ボディ数。 */
    u32   Count() const noexcept { return m_ActiveCount; }

    /** i 番ボディの位置 (範囲外は原点)。 */
    FVec2 Position(u32 i) const noexcept { return i < m_Bodies.Size() ? m_Bodies[i].pos : FVec2{0.0f, 0.0f}; }

    /** i 番ボディの速度 (範囲外は 0)。 */
    FVec2 Velocity(u32 i) const noexcept { return i < m_Bodies.Size() ? m_Bodies[i].vel : FVec2{0.0f, 0.0f}; }

    /** i 番ボディの速度を設定する。 */
    void  SetVelocity(u32 i, FVec2 v) noexcept { if (i < m_Bodies.Size()) m_Bodies[i].vel = v; }

    /** i 番ボディの回転角 [rad] (範囲外は 0)。 */
    f32   Angle(u32 i) const noexcept { return i < m_Bodies.Size() ? m_Bodies[i].angle : 0.0f; }

    /** i 番ボディの角速度 [rad/s] (範囲外は 0)。 */
    f32   AngularVelocity(u32 i) const noexcept { return i < m_Bodies.Size() ? m_Bodies[i].ang_vel : 0.0f; }

    /** i 番ボディの角速度を設定する。 */
    void  SetAngularVelocity(u32 i, f32 w) noexcept { if (i < m_Bodies.Size()) m_Bodies[i].ang_vel = w; }

    /** i 番ボディの初期回転角 [rad] を設定する (有向ボックス OBB の向き)。生成直後に呼ぶ。 */
    void  SetAngle(u32 i, f32 a) noexcept { if (i < m_Bodies.Size()) m_Bodies[i].angle = a; }

    /** i 番ボディの減衰を設定する (linear / angular、1/s)。回転や速度を時間で自然に弱める。 */
    void  SetDamping(u32 i, f32 linear, f32 angular) noexcept {
        if (i < m_Bodies.Size()) { m_Bodies[i].lin_damp = (linear > 0.0f) ? linear : 0.0f;
                                   m_Bodies[i].ang_damp = (angular > 0.0f) ? angular : 0.0f; }
    }

    /** i 番ボディ (read-only、非アクティブも返る)。 */
    const FRigidBodyState* Body(u32 i) const noexcept { return i < m_Bodies.Size() ? &m_Bodies[i] : nullptr; }

    /** index のボディがアクティブ (未削除) か。 */
    bool IsActive(u32 i) const noexcept { return i < m_Bodies.Size() && m_Bodies[i].active; }

    /**
     * i 番ボディの連続衝突判定 (CCD) を有効/無効にする。
     *
     * @details
     * 既定は off。on にすると、そのボディは Step の位置積分でスウィープ判定され、1 ステップの
     * 移動量が大きくても静的形状を貫通せず接触面 (TOI) で止まる。弾丸・高速ボールなど薄い壁を
     * すり抜けやすい動的ボディに使う。CCD 対象は **静的ボディ (円 / 軸並行ボックス)** のみで、
     * 動的同士・回転静的・凸ポリゴン静的は離散判定にフォールバックする (本ボディは bounding 円で近似)。
     * @param i ボディ index。
     * @param on true で CCD 有効。
     */
    void SetCcd(u32 i, bool on) noexcept { if (i < m_Bodies.Size()) m_Bodies[i].ccd = on; }

    /** i 番ボディの CCD が有効か (範囲外は false)。 */
    bool IsCcd(u32 i) const noexcept { return i < m_Bodies.Size() && m_Bodies[i].ccd; }

    /**
     * index のボディを削除する (tombstone 化し、slot を再利用待ちにする)。
     *
     * @details index は安定 (詰め直さない) ので他ボディの index は無効化されない。
     * 削除後はシミュレーション・衝突・クエリ対象から外れ、次の Add* が slot を再利用する。
     * @param index 削除するボディ index。
     */
    void RemoveBody(u32 index) noexcept;

    // ----- 空間クエリ (視線 / 接地 / クリック選択 / 範囲) -----

    /** point を含む最初のアクティブボディ index を返す (無ければ -1)。 */
    i32 QueryPoint(FVec2 p) const noexcept;

    /**
     * origin から dir 方向へ max_dist までレイキャストし、最近接ヒットを返す。
     * @param origin レイ始点。
     * @param dir レイ方向 (内部で正規化、ゼロは miss)。
     * @param max_dist 最大距離。
     * @return 最近接ヒット (hit=false でなし)。
     */
    FRayHit2D Raycast(FVec2 origin, FVec2 dir, f32 max_dist) const noexcept;

    /**
     * 中心 center 半径 radius の円に重なるアクティブボディ index を out へ集める。
     * @param center クエリ円の中心。
     * @param radius クエリ円の半径。
     * @param out_indices 結果を書く配列。
     * @param cap out_indices の容量。
     * @return 重なったボディ数 (cap で頭打ち)。
     */
    u32 OverlapCircle(FVec2 center, f32 radius, u32* out_indices, u32 cap) const noexcept;

    /**
     * body[index] に重なっている他のアクティブボディを集める (センサのトリガ判定に使う)。
     * @param index 基準ボディ (センサ等)。
     * @param out_indices 結果を書く配列。
     * @param cap out_indices の容量。
     * @return 重なったボディ数 (自身は除く、cap で頭打ち)。
     */
    u32 OverlapBody(u32 index, u32* out_indices, u32 cap) const noexcept;

private:
    /** 速度反復回数 (逐次インパルス。積み重ね/同時接触の収束)。 */
    static constexpr u32 kVelIters = 8u;
    /** 位置反復回数 (NGS。めり込みを位置のみで除去、エネルギーを注入しない)。 */
    static constexpr u32 kPosIters = 3u;

    /** ボディ群 (静的・動的・tombstone 混在、index は安定)。 */
    TArray<FRigidBodyState> m_Bodies;

    /** 削除済み slot の再利用フリーリスト。 */
    TArray<u32> m_FreeList;

    /** アクティブ (未削除) ボディ数。 */
    u32 m_ActiveCount = 0u;

    /** state を新規 slot か再利用 slot に格納して index を返す。 */
    u32 AllocBody(const FRigidBodyState& state) noexcept;
};

} // namespace acs::game
