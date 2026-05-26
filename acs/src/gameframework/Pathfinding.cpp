// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L Phase 2 — Pathfinding 実装
//
// A* の流れ (標準形):
//   1. start を open set に追加、g_score[start] = 0、f_score[start] = h(start, goal)
//   2. open set から f_score 最小の node を取り出す (= current)
//   3. current == goal なら経路再構築して終了
//   4. current を closed に移し、隣接 cell を全部チェック:
//        ・通行不可 / closed ならスキップ
//        ・tentative_g = g_score[current] + edge_cost
//        ・既知の g_score より小さければ came_from / g / f を更新し open に追加
//   5. open set が空になったら到達不能 → 失敗
//
// 隣接 cell の定義 (offset 表):
//   4 方向 (allow_diagonal = false):
//       (-1, 0) (+1, 0) ( 0,-1) ( 0,+1)   全部 cost = 1.0
//   8 方向 (allow_diagonal = true):
//       上記 4 つ + (-1,-1) (+1,-1) (-1,+1) (+1,+1)   対角は cost = sqrt(2) ≈ 1.4142
#include "gameframework/Pathfinding.h"
#include "math/Math.h"

namespace acs::game {

// 対角移動コスト (sqrt(2))。constexpr ではなく inline static でリテラル化。
static constexpr f32 kDiagCost = 1.41421356237f;
static constexpr f32 kStraightCost = 1.0f;
// came_from の "未設定" 番兵。u32 最大値。グリッドサイズは width*height で
// 実用上ここまで届かないので衝突しない。
static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

// ---- Init / Setter / Getter ----------------------------------------------------

void FNavGrid::Init(u32 width, u32 height) noexcept {
    _width  = width;
    _height = height;
    const u32 count = width * height;
    _walkable.Clear();
    _walkable.Resize(count);
    for (u32 i = 0; i < count; ++i) _walkable[i] = 1;   // 全 cell walkable
    // A* 一時バッファも事前 reserve しておく (FindPath 初回の alloc を減らす)。
    _open.Clear();
    _closed.Clear();
    _closed.Resize(count);
    _in_open.Clear();
    _in_open.Resize(count);
    _g_score.Clear();
    _g_score.Resize(count);
    _f_score.Clear();
    _f_score.Resize(count);
    _came_from.Clear();
    _came_from.Resize(count);
}

void FNavGrid::SetWalkable(u32 x, u32 y, bool walkable) noexcept {
    if (x >= _width || y >= _height) return;          // 範囲外は no-op
    _walkable[IndexOf(x, y)] = walkable ? 1 : 0;
}

bool FNavGrid::IsWalkable(u32 x, u32 y) const noexcept {
    if (x >= _width || y >= _height) return false;   // 範囲外は通行不可扱い
    return _walkable[IndexOf(x, y)] != 0;
}

void FNavGrid::ClearWalls() noexcept {
    const u32 count = _width * _height;
    for (u32 i = 0; i < count; ++i) _walkable[i] = 1;
}

// ---- Heuristic -----------------------------------------------------------------

f32 FNavGrid::Heuristic(u32 x, u32 y, u32 goal_x, u32 goal_y) const noexcept {
    // 符号付き距離を取るため一度 i64 経由で減算してから絶対値。
    const f32 dx = Abs(static_cast<f32>(static_cast<i64>(x) - static_cast<i64>(goal_x)));
    const f32 dy = Abs(static_cast<f32>(static_cast<i64>(y) - static_cast<i64>(goal_y)));
    if (_allow_diagonal) {
        // Octile distance: 対角移動を考慮した admissible heuristic。
        //   octile = max(dx,dy) + (sqrt(2)-1) * min(dx,dy)
        // Euclidean より tight だが overestimate しない (= optimal を保証)。
        const f32 mn = dx < dy ? dx : dy;
        const f32 mx = dx < dy ? dy : dx;
        return mx + (kDiagCost - 1.0f) * mn;
    }
    // Manhattan: 4 方向の場合 optimal。
    return dx + dy;
}

// ---- Open set 操作 -------------------------------------------------------------

usize FNavGrid::PopLowestF() noexcept {
    // 線形最小値走査 (Phase 2 簡素実装)。binary heap への置換は Phase 3+。
    const usize n = _open.Size();
    if (n == 0) return 0;
    usize best_pos = 0;
    f32   best_f   = _f_score[_open[0]];
    for (usize i = 1; i < n; ++i) {
        const f32 f = _f_score[_open[i]];
        if (f < best_f) {
            best_f   = f;
            best_pos = i;
        }
    }
    return best_pos;
}

// ---- Reconstruct ---------------------------------------------------------------

void FNavGrid::Reconstruct(u32 start_idx, u32 goal_idx, TArray<FPathPoint>& out_path) const noexcept {
    // goal から came_from を辿って start まで遡り、out_path に逆順で push。
    // 最後に reverse して start → goal の順にする。
    out_path.Clear();
    u32 cur = goal_idx;
    // 安全弁 (循環参照などで無限ループしないよう、最大 width*height 回で打ち切り)。
    const u32 max_steps = _width * _height + 1u;
    u32 steps = 0;
    while (cur != kInvalidIndex && steps <= max_steps) {
        FPathPoint p;
        p.x = cur % _width;
        p.y = cur / _width;
        out_path.PushBack(p);
        if (cur == start_idx) break;
        cur = _came_from[cur];
        ++steps;
    }
    // 逆順反転 (start → goal にする)。
    const usize n = out_path.Size();
    for (usize i = 0; i < n / 2; ++i) {
        FPathPoint tmp        = out_path[i];
        out_path[i]          = out_path[n - 1 - i];
        out_path[n - 1 - i]  = tmp;
    }
}

// ---- A* 本体 -------------------------------------------------------------------

bool FNavGrid::FindPath(u32 start_x, u32 start_y,
                       u32 goal_x,  u32 goal_y,
                       TArray<FPathPoint>& out_path) noexcept {
    out_path.Clear();

    // ----- 早期失敗ガード -----
    if (_width == 0 || _height == 0)                return false;
    if (start_x >= _width || start_y >= _height)    return false;
    if (goal_x  >= _width || goal_y  >= _height)    return false;
    if (!IsWalkable(start_x, start_y))              return false;
    if (!IsWalkable(goal_x,  goal_y))               return false;

    const u32 start_idx = IndexOf(start_x, start_y);
    const u32 goal_idx  = IndexOf(goal_x,  goal_y);

    // start == goal の特殊ケース: 長さ 1 の path として成功。
    if (start_idx == goal_idx) {
        FPathPoint p;
        p.x = start_x;
        p.y = start_y;
        out_path.PushBack(p);
        return true;
    }

    // ----- 一時バッファのリセット -----
    // (Init で resize 済みのはずだが、念のためサイズ齟齬があれば調整)。
    const u32 count = _width * _height;
    if (_closed.Size()    != count) _closed.Resize(count);
    if (_in_open.Size()   != count) _in_open.Resize(count);
    if (_g_score.Size()   != count) _g_score.Resize(count);
    if (_f_score.Size()   != count) _f_score.Resize(count);
    if (_came_from.Size() != count) _came_from.Resize(count);
    for (u32 i = 0; i < count; ++i) {
        _closed[i]    = 0;
        _in_open[i]   = 0;
        _g_score[i]   = 0.0f;
        _f_score[i]   = 0.0f;
        _came_from[i] = kInvalidIndex;
    }
    _open.Clear();

    // ----- start を open へ -----
    _g_score[start_idx]  = 0.0f;
    _f_score[start_idx]  = Heuristic(start_x, start_y, goal_x, goal_y);
    _in_open[start_idx]  = 1;
    _open.PushBack(start_idx);

    // ----- 隣接 offset 表 -----
    // 4 方向 + (allow_diagonal なら 4 つ追加) = 最大 8。
    // dx, dy, cost の組を持つ。
    struct Neighbor {
        i32 dx;
        i32 dy;
        f32 cost;
    };
    Neighbor neighbors[8] = {
        { -1,  0, kStraightCost },
        {  1,  0, kStraightCost },
        {  0, -1, kStraightCost },
        {  0,  1, kStraightCost },
        { -1, -1, kDiagCost     },
        {  1, -1, kDiagCost     },
        { -1,  1, kDiagCost     },
        {  1,  1, kDiagCost     },
    };
    const u32 neighbor_count = _allow_diagonal ? 8u : 4u;

    // ----- メインループ -----
    while (!_open.IsEmpty()) {
        // 1. open から f 最小を取り出す。
        const usize pos = PopLowestF();
        const u32 current = _open[pos];
        // RemoveAtSwap は順序保持しないが、open set の順序は不要 (どうせ毎回最小走査)。
        _open.RemoveAtSwap(pos);
        _in_open[current] = 0;

        // 2. ゴール到達?
        if (current == goal_idx) {
            Reconstruct(start_idx, goal_idx, out_path);
            return true;
        }

        // 3. closed へ。
        _closed[current] = 1;

        const u32 cx = current % _width;
        const u32 cy = current / _width;

        // 4. 隣接を展開。
        for (u32 n = 0; n < neighbor_count; ++n) {
            const i64 nx_i = static_cast<i64>(cx) + neighbors[n].dx;
            const i64 ny_i = static_cast<i64>(cy) + neighbors[n].dy;
            // 範囲外?
            if (nx_i < 0 || ny_i < 0) continue;
            const u32 nx = static_cast<u32>(nx_i);
            const u32 ny = static_cast<u32>(ny_i);
            if (nx >= _width || ny >= _height) continue;
            const u32 n_idx = IndexOf(nx, ny);
            // 通行不可 / closed ならスキップ。
            if (_walkable[n_idx] == 0) continue;
            if (_closed[n_idx] != 0)   continue;

            // tentative_g = current の g + 移動コスト
            const f32 tentative_g = _g_score[current] + neighbors[n].cost;

            // open 未在籍 か、より良い経路を発見 か。
            const bool in_open_now = (_in_open[n_idx] != 0);
            if (!in_open_now || tentative_g < _g_score[n_idx]) {
                _came_from[n_idx] = current;
                _g_score[n_idx]   = tentative_g;
                _f_score[n_idx]   = tentative_g + Heuristic(nx, ny, goal_x, goal_y);
                if (!in_open_now) {
                    _in_open[n_idx] = 1;
                    _open.PushBack(n_idx);
                }
                // 既に open にいる場合は f を上書きするだけで OK (次の PopLowestF が拾う)。
            }
        }
    }

    // open set が空になっても goal に届かなかった → 到達不能。
    out_path.Clear();
    return false;
}

} // namespace acs::game
