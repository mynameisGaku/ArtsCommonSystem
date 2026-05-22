// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L Phase 2 — Pathfinding (grid A*)
//
// 2D 格子状の通行可否マップ + A* 経路探索。タワーディフェンス / 敵の追跡 /
// クリック移動 RPG など、グリッドベースの AI 動作に汎用に使える。
//
// 使い方:
//   NavGrid nav;
//   nav.Init(32, 24);
//   nav.SetWalkable(5, 10, false);                // 壁を 1 マス置く
//   nav.SetAllowDiagonal(true);                   // 8 方向許可 (既定は 4 方向)
//
//   Array<NavGrid::PathPoint> path;
//   if (nav.FindPath(/*start=*/0, 0, /*goal=*/20, 15, path)) {
//       // path[0] = start, path[Size()-1] = goal の連続セル列
//   }
//
// 設計選択:
//   ・cost = 4 方向移動 1.0 / 対角 sqrt(2) ≈ 1.414。SetAllowDiagonal(true) で
//     対角を許可、その時は heuristic も Euclidean (より admissible) に切替。
//     4 方向のみの場合は Manhattan が optimal な heuristic。
//   ・open set は **priority queue 代わりの線形最小値走査** (Phase 2 最小実装)。
//     N=width*height が大きくなったら binary heap へ置換 (Phase 3+)。
//   ・closed flag は `acs::Array<u8>` (size = width*height) で O(1) lookup。
//   ・came_from / g_score も 1D 配列で持つ (index = y*width + x)。
//   ・STL 不使用 (<algorithm> 含む)、`acs::Array` のみ。
//
// 制限 (Phase 2):
//   ・cost は固定 (terrain weight 未対応)。Phase 3 で per-cell weight を導入予定。
//   ・対角通過時の "壁すり抜け" は許可 (両隣が壁でも対角通れる)。厳密化は将来。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

class NavGrid {
public:
    // 経路の 1 セルを表す座標ペア。
    struct PathPoint {
        u32 x = 0;
        u32 y = 0;
    };

    NavGrid() noexcept = default;
    ~NavGrid() noexcept = default;

    // 非コピー・非ムーブ — Scene / AI モジュールのメンバとして固定の場所に置く想定。
    NavGrid(const NavGrid&)            = delete;
    NavGrid& operator=(const NavGrid&) = delete;
    NavGrid(NavGrid&&)                 = delete;
    NavGrid& operator=(NavGrid&&)      = delete;

    // width * height のグリッドを「全 cell walkable」で初期化。
    // width=0 or height=0 を渡した場合はサイズ 0 として扱い、以降のクエリは no-op。
    void Init(u32 width, u32 height) noexcept;

    // 通行可否を設定。範囲外座標は静かに無視 (ソフトフェイル)。
    void SetWalkable(u32 x, u32 y, bool walkable) noexcept;

    // 通行可否を取得。範囲外座標は false を返す。
    bool IsWalkable(u32 x, u32 y) const noexcept;

    u32 Width()  const noexcept { return _width; }
    u32 Height() const noexcept { return _height; }

    // 対角移動の許可フラグ。既定は false (4 方向のみ)。
    void SetAllowDiagonal(bool allow) noexcept { _allow_diagonal = allow; }
    bool AllowDiagonal() const noexcept { return _allow_diagonal; }

    // A* 経路探索。
    //   ・成功時: out_path に start → goal の連続 cell を書き込み true 返す
    //             (out_path[0] == start, out_path.Back() == goal)。
    //   ・失敗時: out_path を Clear して false (範囲外座標 / 通行不可 / 到達不能)。
    //   ・start == goal は「長さ 1 の path」として成功扱い。
    bool FindPath(u32 start_x, u32 start_y,
                  u32 goal_x,  u32 goal_y,
                  Array<PathPoint>& out_path) noexcept;

    // 全 cell を walkable に戻す (グリッドサイズは保持)。
    void ClearWalls() noexcept;

private:
    // index 計算 (y * width + x)。範囲チェックは呼び出し側責務。
    u32 IndexOf(u32 x, u32 y) const noexcept { return y * _width + x; }

    // open list の中から f_score 最小のノードを線形走査して取り出す。
    // 見つかった場合はそのインデックスを返し、見つからなければ size を返す。
    usize PopLowestF() noexcept;

    // heuristic (h_score) を計算。対角許可なら Euclidean、なしなら Manhattan。
    f32 Heuristic(u32 x, u32 y, u32 goal_x, u32 goal_y) const noexcept;

    // 経路を再構築 (goal から came_from を辿って start まで戻り、逆順に並べる)。
    void Reconstruct(u32 start_idx, u32 goal_idx, Array<PathPoint>& out_path) const noexcept;

    // ---- 形状 ----
    u32       _width          = 0;
    u32       _height         = 0;
    bool      _allow_diagonal = false;

    // ---- 静的状態: 通行可否 (size = width*height) ----
    // 1 = walkable, 0 = blocked。
    Array<u8> _walkable;

    // ---- A* 一時バッファ (FindPath 呼び出し毎に reset) ----
    // 毎回 alloc しなくて済むようメンバに保持。
    Array<u32> _open;          // open set: 候補 cell index 列
    Array<u8>  _closed;        // closed flag (size = width*height)
    Array<u8>  _in_open;       // open set 在籍 flag (重複追加を防ぐ高速判定用)
    Array<f32> _g_score;       // start からの実コスト (size = width*height)
    Array<f32> _f_score;       // g + h (size = width*height)
    Array<u32> _came_from;     // 親 cell index (size = width*height)。0xFFFFFFFF = 未設定
};

} // namespace acs::game
