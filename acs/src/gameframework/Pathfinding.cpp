// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — Pathfinding 実装
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

/** 対角移動のコスト (sqrt(2))。 */
static constexpr f32 kDiagCost = 1.41421356237f;

/** 直進移動 (4 方向) のコスト。 */
static constexpr f32 kStraightCost = 1.0f;

/** came_from の "未設定" 番兵 (u32 最大値)。実用上グリッド index と衝突しない。 */
static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

/** グリッドを width*height で全 cell walkable に初期化し、A* 一時バッファを確保する。 */
void FNavGrid::Init(u32 width, u32 height) noexcept {
    m_Width  = width;
    m_Height = height;
    const u32 count = width * height;
    m_Walkable.Clear();
    m_Walkable.Resize(count);
    for (u32 i = 0; i < count; ++i) m_Walkable[i] = 1;   // 全 cell walkable
    // A* 一時バッファも事前 reserve しておく (FindPath 初回の alloc を減らす)。
    _open.Clear();
    _closed.Clear();
    _closed.Resize(count);
    m_InOpen.Clear();
    m_InOpen.Resize(count);
    m_GScore.Clear();
    m_GScore.Resize(count);
    m_FScore.Clear();
    m_FScore.Resize(count);
    m_CameFrom.Clear();
    m_CameFrom.Resize(count);
}

/** cell の通行可否を設定する (範囲外は no-op)。 */
void FNavGrid::SetWalkable(u32 x, u32 y, bool walkable) noexcept {
    if (x >= m_Width || y >= m_Height) return;          // 範囲外は no-op
    m_Walkable[IndexOf(x, y)] = walkable ? 1 : 0;
}

/** cell の通行可否を返す (範囲外は通行不可扱い)。 */
bool FNavGrid::IsWalkable(u32 x, u32 y) const noexcept {
    if (x >= m_Width || y >= m_Height) return false;   // 範囲外は通行不可扱い
    return m_Walkable[IndexOf(x, y)] != 0;
}

/** 全 cell を walkable に戻す (グリッドサイズは保持)。 */
void FNavGrid::ClearWalls() noexcept {
    const u32 count = m_Width * m_Height;
    for (u32 i = 0; i < count; ++i) m_Walkable[i] = 1;
}

/** 対角許可なら Octile distance、なしなら Manhattan の heuristic を計算する。 */
f32 FNavGrid::Heuristic(u32 x, u32 y, u32 goal_x, u32 goal_y) const noexcept {
    // 符号付き距離を取るため一度 i64 経由で減算してから絶対値。
    const f32 dx = Abs(static_cast<f32>(static_cast<i64>(x) - static_cast<i64>(goal_x)));
    const f32 dy = Abs(static_cast<f32>(static_cast<i64>(y) - static_cast<i64>(goal_y)));
    if (m_AllowDiagonal) {
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

/** open list から f_score 最小のノードの位置を線形走査で返す。 */
usize FNavGrid::PopLowestF() noexcept {
    // 線形最小値走査の簡素実装。binary heap への置換は将来検討。
    const usize n = _open.Size();
    if (n == 0) return 0;
    usize best_pos = 0;
    f32   best_f   = m_FScore[_open[0]];
    for (usize i = 1; i < n; ++i) {
        const f32 f = m_FScore[_open[i]];
        if (f < best_f) {
            best_f   = f;
            best_pos = i;
        }
    }
    return best_pos;
}

/** came_from を goal から start まで遡り、逆順に並べて経路を再構築する。 */
void FNavGrid::Reconstruct(u32 start_idx, u32 goal_idx, TArray<FPathPoint>& out_path) const noexcept {
    // goal から came_from を辿って start まで遡り、out_path に逆順で push。
    // 最後に reverse して start → goal の順にする。
    out_path.Clear();
    u32 cur = goal_idx;
    // 安全弁 (循環参照などで無限ループしないよう、最大 width*height 回で打ち切り)。
    const u32 max_steps = m_Width * m_Height + 1u;
    u32 steps = 0;
    while (cur != kInvalidIndex && steps <= max_steps) {
        FPathPoint p;
        p.x = cur % m_Width;
        p.y = cur / m_Width;
        out_path.PushBack(p);
        if (cur == start_idx) break;
        cur = m_CameFrom[cur];
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

/** A* 本体。start から goal への最短経路を out_path に書き込む (成否を返す)。 */
bool FNavGrid::FindPath(u32 start_x, u32 start_y,
                       u32 goal_x,  u32 goal_y,
                       TArray<FPathPoint>& out_path) noexcept {
    out_path.Clear();

    // 早期失敗ガード
    if (m_Width == 0 || m_Height == 0)                return false;
    if (start_x >= m_Width || start_y >= m_Height)    return false;
    if (goal_x  >= m_Width || goal_y  >= m_Height)    return false;
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

    // 一時バッファのリセット
    // (Init で resize 済みのはずだが、念のためサイズ齟齬があれば調整)。
    const u32 count = m_Width * m_Height;
    if (_closed.Size()    != count) _closed.Resize(count);
    if (m_InOpen.Size()   != count) m_InOpen.Resize(count);
    if (m_GScore.Size()   != count) m_GScore.Resize(count);
    if (m_FScore.Size()   != count) m_FScore.Resize(count);
    if (m_CameFrom.Size() != count) m_CameFrom.Resize(count);
    for (u32 i = 0; i < count; ++i) {
        _closed[i]    = 0;
        m_InOpen[i]   = 0;
        m_GScore[i]   = 0.0f;
        m_FScore[i]   = 0.0f;
        m_CameFrom[i] = kInvalidIndex;
    }
    _open.Clear();

    // start を open へ
    m_GScore[start_idx]  = 0.0f;
    m_FScore[start_idx]  = Heuristic(start_x, start_y, goal_x, goal_y);
    m_InOpen[start_idx]  = 1;
    _open.PushBack(start_idx);

    // 隣接 offset 表
    // 4 方向 + (allow_diagonal なら 4 つ追加) = 最大 8。
    // dx, dy, cost の組を持つ。
    struct FNeighbor {
        i32 dx;
        i32 dy;
        f32 cost;
    };
    FNeighbor neighbors[8] = {
        { -1,  0, kStraightCost },
        {  1,  0, kStraightCost },
        {  0, -1, kStraightCost },
        {  0,  1, kStraightCost },
        { -1, -1, kDiagCost     },
        {  1, -1, kDiagCost     },
        { -1,  1, kDiagCost     },
        {  1,  1, kDiagCost     },
    };
    const u32 neighbor_count = m_AllowDiagonal ? 8u : 4u;

    // メインループ
    while (!_open.IsEmpty()) {
        // 1. open から f 最小を取り出す。
        const usize pos = PopLowestF();
        const u32 current = _open[pos];
        // RemoveAtSwap は順序保持しないが、open set の順序は不要 (どうせ毎回最小走査)。
        _open.RemoveAtSwap(pos);
        m_InOpen[current] = 0;

        // 2. ゴール到達?
        if (current == goal_idx) {
            Reconstruct(start_idx, goal_idx, out_path);
            return true;
        }

        // 3. closed へ。
        _closed[current] = 1;

        const u32 cx = current % m_Width;
        const u32 cy = current / m_Width;

        // 4. 隣接を展開。
        for (u32 n = 0; n < neighbor_count; ++n) {
            const i64 nx_i = static_cast<i64>(cx) + neighbors[n].dx;
            const i64 ny_i = static_cast<i64>(cy) + neighbors[n].dy;
            // 範囲外?
            if (nx_i < 0 || ny_i < 0) continue;
            const u32 nx = static_cast<u32>(nx_i);
            const u32 ny = static_cast<u32>(ny_i);
            if (nx >= m_Width || ny >= m_Height) continue;
            const u32 n_idx = IndexOf(nx, ny);
            // 通行不可 / closed ならスキップ。
            if (m_Walkable[n_idx] == 0) continue;
            if (_closed[n_idx] != 0)   continue;

            // tentative_g = current の g + 移動コスト
            const f32 tentative_g = m_GScore[current] + neighbors[n].cost;

            // open 未在籍 か、より良い経路を発見 か。
            const bool in_open_now = (m_InOpen[n_idx] != 0);
            if (!in_open_now || tentative_g < m_GScore[n_idx]) {
                m_CameFrom[n_idx] = current;
                m_GScore[n_idx]   = tentative_g;
                m_FScore[n_idx]   = tentative_g + Heuristic(nx, ny, goal_x, goal_y);
                if (!in_open_now) {
                    m_InOpen[n_idx] = 1;
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
