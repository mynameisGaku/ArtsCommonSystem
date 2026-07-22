// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — Pathfinding (grid A*)
//
// 2D 格子状の通行可否マップ + A* 経路探索。タワーディフェンス / 敵の追跡 /
// クリック移動 RPG など、グリッドベースの AI 動作に汎用に使える。
//
// 使い方:
//   FNavGrid nav;
//   nav.Init(32, 24);
//   nav.SetWalkable(5, 10, false);                // 壁を 1 マス置く
//   nav.SetAllowDiagonal(true);                   // 8 方向許可 (既定は 4 方向)
//
//   TArray<FNavGrid::FPathPoint> path;
//   if (nav.FindPath(/*start=*/0, 0, /*goal=*/20, 15, path)) {
//       // path[0] = start, path[Size()-1] = goal の連続セル列
//   }
//
// 設計選択:
//   ・cost = 4 方向移動 1.0 / 対角 sqrt(2) ≈ 1.414。SetAllowDiagonal(true) で
//     対角を許可、その時は heuristic も Euclidean (より admissible) に切替。
//     4 方向のみの場合は Manhattan が optimal な heuristic。
//   ・open set は **priority queue 代わりの線形最小値走査**。
//     N=width*height が大きくなったら binary heap へ置換する。
//   ・closed flag は `acs::TArray<u8>` (size = width*height) で O(1) lookup。
//   ・came_from / g_score も 1D 配列で持つ (index = y*width + x)。
//   ・STL 不使用 (<algorithm> 含む)、`acs::TArray` のみ。
//
// 制限:
//   ・cost は固定 (terrain weight 未対応)。
//   ・対角通過時の "壁すり抜け" は許可 (両隣が壁でも対角通れる)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 2D 格子の通行可否マップと A* 経路探索を提供するナビゲーショングリッド。
 *
 * @details
 * width*height の cell を 1D 配列で管理し、4 方向 (cost 1.0) または 8 方向
 * (対角 cost sqrt(2)) で A* を解く。open set は線形最小値走査、closed / g / f /
 * came_from も 1D 配列で持つ。一時バッファはメンバに保持し FindPath 毎に reset する。
 */
class FNavGrid {
public:
    /**
     * 経路の 1 セルを表すグリッド座標。
     */
    struct FPathPoint {
        /** セルの x 座標。 */
        u32 x = 0;

        /** セルの y 座標。 */
        u32 y = 0;
    };

    /** 空状態で構築する (グリッドは Init で確保)。 */
    FNavGrid() noexcept = default;

    /** 破棄する。 */
    ~FNavGrid() noexcept = default;

    /** コピー禁止 (FScene / AI モジュールのメンバとして固定の場所に置く想定)。 */
    FNavGrid(const FNavGrid&)            = delete;

    /** コピー代入も禁止。 */
    FNavGrid& operator=(const FNavGrid&) = delete;

    /** ムーブ禁止。 */
    FNavGrid(FNavGrid&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNavGrid& operator=(FNavGrid&&)      = delete;

    /**
     * width * height のグリッドを全 cell walkable で初期化する。
     *
     * @details width=0 or height=0 はサイズ 0 として扱い、以降のクエリは no-op。A* 一時バッファも事前確保する。
     * @param width グリッドの横セル数。
     * @param height グリッドの縦セル数。
     */
    void Init(u32 width, u32 height) noexcept;

    /**
     * cell の通行可否を設定する。
     *
     * @details 範囲外座標は静かに無視する (ソフトフェイル)。
     * @param x cell の x 座標。
     * @param y cell の y 座標。
     * @param walkable 通行可能なら true。
     */
    void SetWalkable(u32 x, u32 y, bool walkable) noexcept;

    /**
     * cell の通行可否を取得する。
     *
     * @param x cell の x 座標。
     * @param y cell の y 座標。
     * @return 通行可能なら true (範囲外は false)。
     */
    bool IsWalkable(u32 x, u32 y) const noexcept;

    /**
     * グリッドの横セル数を返す。
     *
     * @return Init で渡された width。
     */
    u32 Width()  const noexcept { return m_Width; }

    /**
     * グリッドの縦セル数を返す。
     *
     * @return Init で渡された height。
     */
    u32 Height() const noexcept { return m_Height; }

    /**
     * 対角移動の許可フラグを設定する。
     *
     * @param allow true で 8 方向移動を許可 (既定 false = 4 方向のみ)。
     */
    void SetAllowDiagonal(bool allow) noexcept { m_AllowDiagonal = allow; }

    /**
     * 対角移動が許可されているかを返す。
     *
     * @return 対角許可なら true。
     */
    bool AllowDiagonal() const noexcept { return m_AllowDiagonal; }

    /**
     * A* で start から goal への経路を探索する。
     *
     * @details
     * 成功時は out_path に start → goal の連続 cell を書き込む (out_path[0]==start、
     * out_path.Back()==goal)。失敗時は out_path を Clear して false (範囲外座標 /
     * 通行不可 / 到達不能)。start == goal は長さ 1 の path として成功扱い。
     * @param start_x 始点の x 座標。
     * @param start_y 始点の y 座標。
     * @param goal_x 終点の x 座標。
     * @param goal_y 終点の y 座標。
     * @param out_path 探索結果の経路を書き込む出力先。
     * @return 経路が見つかれば true。
     */
    bool FindPath(u32 start_x, u32 start_y,
                  u32 goal_x,  u32 goal_y,
                  TArray<FPathPoint>& out_path) noexcept;

    /** 全 cell を walkable に戻す (グリッドサイズは保持)。 */
    void ClearWalls() noexcept;

private:
    /**
     * (x, y) を 1D 配列 index に変換する。
     *
     * @details 範囲チェックは呼び出し側の責務。
     * @param x cell の x 座標。
     * @param y cell の y 座標。
     * @return y * width + x。
     */
    u32 IndexOf(u32 x, u32 y) const noexcept { return y * m_Width + x; }

    /**
     * open list から f_score 最小のノードを線形走査で見つける。
     *
     * @return open list 内の最小 f の位置 (open が空なら 0)。
     */
    usize PopLowestF() noexcept;

    /**
     * heuristic (h_score) を計算する。
     *
     * @details 対角許可なら Octile distance、なしなら Manhattan (どちらも admissible)。
     * @param x 現在 cell の x 座標。
     * @param y 現在 cell の y 座標。
     * @param goal_x 終点の x 座標。
     * @param goal_y 終点の y 座標。
     * @return 推定残コスト。
     */
    f32 Heuristic(u32 x, u32 y, u32 goal_x, u32 goal_y) const noexcept;

    /**
     * came_from を辿って経路を再構築する。
     *
     * @details goal から came_from を遡って start まで集め、逆順に並べて start → goal にする。
     * @param start_idx 始点の cell index。
     * @param goal_idx 終点の cell index。
     * @param out_path 再構築した経路を書き込む出力先。
     */
    void Reconstruct(u32 start_idx, u32 goal_idx, TArray<FPathPoint>& out_path) const noexcept;

    /** グリッドの横セル数。 */
    u32       m_Width          = 0;

    /** グリッドの縦セル数。 */
    u32       m_Height         = 0;

    /** 対角移動の許可フラグ (既定 false)。 */
    bool      m_AllowDiagonal = false;

    /** cell の通行可否 (size = width*height、1=walkable / 0=blocked)。 */
    TArray<u8> m_Walkable;

    /** A* の open set (候補 cell index 列)。 */
    TArray<u32> _open;

    /** A* の closed flag (size = width*height)。 */
    TArray<u8>  _closed;

    /** open set 在籍 flag (重複追加を防ぐ高速判定用)。 */
    TArray<u8>  m_InOpen;

    /** start からの実コスト g (size = width*height)。 */
    TArray<f32> m_GScore;

    /** 推定総コスト f = g + h (size = width*height)。 */
    TArray<f32> m_FScore;

    /** 親 cell index (size = width*height、0xFFFFFFFF = 未設定)。 */
    TArray<u32> m_CameFrom;
};

} // namespace acs::game
