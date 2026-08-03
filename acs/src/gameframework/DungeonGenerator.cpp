// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (Genre Kit: Roguelike) — BSP ダンジョン生成 実装
//
// アルゴリズム (古典 BSP roguelike):
//   1. 全領域を Wall で塗る。
//   2. ルート partition = グリッド全体。スタックで DFS 分割:
//        ・min_partition_size 以下、もしくは depth ≥ max_depth ならリーフ確定。
//        ・縦横どちらかを選び (アスペクト比偏りを避けるためバランス調整)、
//          中央付近で 2 つの子 partition に分ける。
//   3. 各リーフ partition に 1 個 room (min_room_size..max_room_size) を
//      ランダム配置 (partition 内に余白を残す)。
//   4. 兄弟リーフ間を L 字廊下で繋ぐ (中心 → 中心、横→縦 or 縦→横)。
//      これを再帰的に登りながら処理することで全体が連結になる。
//   5. ランダムな部屋に Stairs を 1 個置く。
//
// 兄弟 connect 戦略:
//   各 internal node に「自身のサブツリー代表 room index」を持たせ、
//   分割した左右子の代表同士を廊下で繋ぐ → 親自身の代表も決まる、を
//   ボトムアップで行う。これで N 部屋 - 1 本の廊下で連結する。
#include "gameframework/DungeonGenerator.h"
#include "gameframework/Random.h"

namespace acs::game {

namespace {

/**
 * BSP 分割の中間表現 (Generate 実行中だけ存在する partition ノード)。
 *
 * @details partition 矩形は閉区間 [x0,x1] x [y0,y1] (両端含む) で表す。
 */
struct FBspNode {
    /** partition 左端 x 座標 (両端含む)。 */
    u32 x0 = 0;

    /** partition 上端 y 座標 (両端含む)。 */
    u32 y0 = 0;

    /** partition 右端 x 座標 (両端含む)。 */
    u32 x1 = 0;

    /** partition 下端 y 座標 (両端含む)。 */
    u32 y1 = 0;

    /** BSP 木の深さ (root が 0)。 */
    u32 depth = 0;

    /** 左子ノードの index (0xFFFFFFFF = 子なし)。TArray<BspNode> 中の位置。 */
    u32 left  = 0xFFFFFFFFu;

    /** 右子ノードの index (0xFFFFFFFF = 子なし)。TArray<BspNode> 中の位置。 */
    u32 right = 0xFFFFFFFFu;

    /** このサブツリーの代表 room 番号 (廊下接続で使う)。0xFFFFFFFF = まだ無い。 */
    u32 rep_room = 0xFFFFFFFFu;
};

/** 無効インデックス / 「子なし」「代表未確定」を表す哨兵値。 */
constexpr u32 kInvalidIdx = 0xFFFFFFFFu;

/**
 * partition の幅を返す (両端含む閉区間)。
 *
 * @param n 対象 partition ノード。
 * @return 幅 (x1 - x0 + 1)。
 */
inline u32 PartitionW(const FBspNode& n) noexcept { return n.x1 - n.x0 + 1u; }

/**
 * partition の高さを返す (両端含む閉区間)。
 *
 * @param n 対象 partition ノード。
 * @return 高さ (y1 - y0 + 1)。
 */
inline u32 PartitionH(const FBspNode& n) noexcept { return n.y1 - n.y0 + 1u; }

} // namespace

void CDungeonGenerator::Clear() noexcept {
    m_Grid.Reset();
    m_Rooms.Reset();
    m_Width  = 0;
    m_Height = 0;
    m_Seed   = 0;
}

ETileKind CDungeonGenerator::At(u32 x, u32 y) const noexcept {
    if (x >= m_Width || y >= m_Height) return ETileKind::Wall;
    return m_Grid[static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x)];
}

void CDungeonGenerator::SetTile(u32 x, u32 y, ETileKind kind) noexcept {
    if (x >= m_Width || y >= m_Height) return;
    m_Grid[static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x)] = kind;
}

const FRoom* CDungeonGenerator::GetRoom(u32 index) const noexcept {
    if (index >= m_Rooms.Num()) return nullptr;
    return &m_Rooms[index];
}

const FRoom* CDungeonGenerator::AllRooms(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Rooms.Num());
    if (out_count == 0u) return nullptr;
    return m_Rooms.GetData();
}

void CDungeonGenerator::GetRoomCenter(u32 room_index, u32& out_x, u32& out_y) const noexcept {
    if (room_index >= m_Rooms.Num()) { out_x = 0; out_y = 0; return; }
    const FRoom& r = m_Rooms[room_index];
    out_x = r.x + r.w / 2u;
    out_y = r.y + r.h / 2u;
}

bool CDungeonGenerator::IsWalkable(u32 x, u32 y) const noexcept {
    const ETileKind k = At(x, y);
    return k == ETileKind::Floor || k == ETileKind::Door
        || k == ETileKind::Corridor || k == ETileKind::Stairs;
}

u32 CDungeonGenerator::FindRandomFloor(u32& out_x, u32& out_y) const noexcept {
    out_x = 0; out_y = 0;
    if (m_Width == 0u || m_Height == 0u) return 0u;
    // 仕様: 試行回数 100。const メソッドなので一時 FRandom をローカルに作る
    // (seed は最後の Generate と FindRandomFloor 呼び出し回数で軽く撹拌)。
    FRandom r(static_cast<u64>(m_Seed) ^ 0xA5A5A5A5A5A5A5A5ULL);
    for (u32 attempt = 1; attempt <= 100u; ++attempt) {
        const u32 x = r.NextU32() % m_Width;
        const u32 y = r.NextU32() % m_Height;
        if (At(x, y) == ETileKind::Floor) {
            out_x = x; out_y = y;
            return attempt;
        }
    }
    return 0u;
}

void CDungeonGenerator::Generate(const FDungeonGenConfig& config) noexcept {
    // 1. 設定 sanitize: 不正値はサイレントに安全な既定にフォールバック。
    u32 width  = config.width  != 0u ? config.width  : 64u;
    u32 height = config.height != 0u ? config.height : 48u;
    // グリッドが極小すぎると room すら作れないので 8x8 を最小として保証。
    if (width  < 8u) width  = 8u;
    if (height < 8u) height = 8u;

    u32 min_room = config.min_room_size != 0u ? config.min_room_size : 4u;
    u32 max_room = config.max_room_size != 0u ? config.max_room_size : 10u;
    if (min_room < 3u) min_room = 3u;             // 廊下接続できる最小
    if (max_room < min_room) max_room = min_room;

    u32 min_partition = config.min_partition_size != 0u ? config.min_partition_size : 8u;
    // partition は最低でも (min_room + 壁余白 2) は要る。
    const u32 partition_floor = min_room + 2u;
    if (min_partition < partition_floor) min_partition = partition_floor;

    u32 max_depth = config.max_depth != 0u ? config.max_depth : 5u;
    if (max_depth > 16u) max_depth = 16u;          // 2^16 leaves は明らかにやりすぎ

    // 2. グリッド確保 + Wall で塗る
    m_Width  = width;
    m_Height = height;
    m_Grid.Reset();
    const usize cells = static_cast<usize>(width) * static_cast<usize>(height);
    m_Grid.SetNum(cells);
    for (usize i = 0; i < cells; ++i) m_Grid[i] = ETileKind::Wall;
    m_Rooms.Reset();
    m_Seed = config.seed;

    FRandom rng(static_cast<u64>(config.seed));

    // 3. BSP 分割: 全 partition を TArray に積み、index で親子関係を保持する。
    // ルートは画面全体 (端の壁余白を確保するため [1, w-2] x [1, h-2])。
    TArray<FBspNode> nodes;
    nodes.Reserve(64u);
    {
        FBspNode root{};
        root.x0 = 1u;
        root.y0 = 1u;
        root.x1 = width  - 2u;
        root.y1 = height - 2u;
        root.depth = 0u;
        nodes.Add(root);
    }

    // 反復的 BSP: stack に「これから分割を試みるノード index」を積む。
    TArray<u32> work;
    work.Reserve(64u);
    work.Add(0u);

    while (!work.IsEmpty()) {
        const u32 idx = work[work.Num() - 1u];
        work.Pop();
        FBspNode node = nodes[idx]; // コピー (後で書き戻す)

        const u32 w = PartitionW(node);
        const u32 h = PartitionH(node);

        // リーフ判定: depth 上限 or どちらかが min_partition 未満なら分割しない。
        const bool too_deep   = node.depth >= max_depth;
        const bool too_small  = (w < min_partition * 2u) && (h < min_partition * 2u);
        if (too_deep || too_small) {
            // リーフ確定 → このノードは子なしのまま (rep_room は room 配置時に決まる)
            continue;
        }

        // 縦か横かを選ぶ: 細長いほうを優先 (3:2 以上偏ったら長辺方向で割る)。
        // 比較は整数のままで OK (h*3 vs w*3 は使わない、明示的に長辺判定)。
        bool split_vertically; // true = 垂直分割 (左右に割る)
        if (w >= h * 3u / 2u) {
            split_vertically = true;
        } else if (h >= w * 3u / 2u) {
            split_vertically = false;
        } else {
            split_vertically = rng.NextBool(0.5f);
        }

        // 選んだ方向が割れないなら反転 (= 両方割れないならリーフ)。
        if (split_vertically && w < min_partition * 2u) split_vertically = false;
        if (!split_vertically && h < min_partition * 2u) split_vertically = true;
        if (split_vertically) {
            if (w < min_partition * 2u) continue; // 結局割れない
        } else {
            if (h < min_partition * 2u) continue;
        }

        // 分割位置: [min_partition, dim - min_partition] からランダム。
        FBspNode left{};
        FBspNode right{};
        left.depth  = node.depth + 1u;
        right.depth = node.depth + 1u;

        if (split_vertically) {
            // x 方向で分割
            const u32 lo = node.x0 + min_partition - 1u;     // 左側の右端候補 (両端含む)
            const u32 hi = node.x1 - min_partition;          // 同じく
            // lo > hi は too_small で弾かれているはずだが防御
            u32 split_x = lo;
            if (hi > lo) split_x = lo + (rng.NextU32() % (hi - lo + 1u));
            left.x0 = node.x0;  left.y0 = node.y0;
            left.x1 = split_x;  left.y1 = node.y1;
            right.x0 = split_x + 1u; right.y0 = node.y0;
            right.x1 = node.x1;      right.y1 = node.y1;
        } else {
            // y 方向で分割
            const u32 lo = node.y0 + min_partition - 1u;
            const u32 hi = node.y1 - min_partition;
            u32 split_y = lo;
            if (hi > lo) split_y = lo + (rng.NextU32() % (hi - lo + 1u));
            left.x0 = node.x0; left.y0 = node.y0;
            left.x1 = node.x1; left.y1 = split_y;
            right.x0 = node.x0;      right.y0 = split_y + 1u;
            right.x1 = node.x1;      right.y1 = node.y1;
        }

        const u32 left_idx  = static_cast<u32>(nodes.Num());
        nodes.Add(left);
        const u32 right_idx = static_cast<u32>(nodes.Num());
        nodes.Add(right);
        node.left  = left_idx;
        node.right = right_idx;
        nodes[idx] = node;

        // 子も分割対象にする
        work.Add(left_idx);
        work.Add(right_idx);
    }

    // 4. リーフに room を配置: 全ノードを線形走査し、子なしのノードがリーフ。
    for (u32 i = 0; i < nodes.Num(); ++i) {
        FBspNode& n = nodes[i];
        if (n.left != kInvalidIdx || n.right != kInvalidIdx) continue;

        const u32 pw = PartitionW(n);
        const u32 ph = PartitionH(n);
        // partition 内に最低 1 セルの壁余白を残すよう、最大辺は dim - 2 まで。
        const u32 max_w = pw >= 3u ? pw - 2u : pw;
        const u32 max_h = ph >= 3u ? ph - 2u : ph;
        u32 rw = min_room;
        u32 rh = min_room;
        if (max_w >= min_room) {
            const u32 cap = max_w < max_room ? max_w : max_room;
            rw = min_room + (rng.NextU32() % (cap - min_room + 1u));
        }
        if (max_h >= min_room) {
            const u32 cap = max_h < max_room ? max_h : max_room;
            rh = min_room + (rng.NextU32() % (cap - min_room + 1u));
        }
        // partition 内のランダム位置 (右下に rw / rh 残す)
        const u32 free_x = pw > rw ? pw - rw : 0u;
        const u32 free_y = ph > rh ? ph - rh : 0u;
        const u32 ox = free_x > 0u ? (rng.NextU32() % (free_x + 1u)) : 0u;
        const u32 oy = free_y > 0u ? (rng.NextU32() % (free_y + 1u)) : 0u;
        const u32 rx = n.x0 + ox;
        const u32 ry = n.y0 + oy;

        // 部屋を Floor で塗る。grid 範囲は config の sanitize で確保済み。
        FRoom room{};
        room.x  = rx;
        room.y  = ry;
        room.w  = rw;
        room.h  = rh;
        room.id = static_cast<u32>(m_Rooms.Num());
        for (u32 yy = ry; yy < ry + rh; ++yy) {
            const usize row = static_cast<usize>(yy) * static_cast<usize>(m_Width);
            for (u32 xx = rx; xx < rx + rw; ++xx) {
                m_Grid[row + static_cast<usize>(xx)] = ETileKind::Floor;
            }
        }
        n.rep_room = room.id;
        m_Rooms.Add(room);
    }

    // 5. 兄弟 leaf 間に廊下を引く。
    // ノードを「深い順」に走査することでボトムアップ処理を実現する。
    // BspNode を depth でソートする代わりに、depth ごとに 2 周走査する。
    // (max_depth は 16 上限なので O(N * max_depth) は十分高速)
    for (u32 d = max_depth; d > 0u; --d) {
        const u32 target_depth = d - 1u;     // 親側 (子は depth = d)
        for (u32 i = 0; i < nodes.Num(); ++i) {
            FBspNode& n = nodes[i];
            if (n.depth != target_depth) continue;
            if (n.left == kInvalidIdx || n.right == kInvalidIdx) continue;

            const FBspNode& L = nodes[n.left];
            const FBspNode& R = nodes[n.right];
            // 両子のサブツリーに room が無いなら何もしない (理論上は無いはず)。
            if (L.rep_room == kInvalidIdx || R.rep_room == kInvalidIdx) {
                // 片方だけでも代表として親に上げる
                if (L.rep_room != kInvalidIdx) n.rep_room = L.rep_room;
                else if (R.rep_room != kInvalidIdx) n.rep_room = R.rep_room;
                continue;
            }

            // 各代表の中心を結ぶ L 字廊下を描く。
            const FRoom& ra = m_Rooms[L.rep_room];
            const FRoom& rb = m_Rooms[R.rep_room];
            const u32 ax = ra.x + ra.w / 2u;
            const u32 ay = ra.y + ra.h / 2u;
            const u32 bx = rb.x + rb.w / 2u;
            const u32 by = rb.y + rb.h / 2u;

            // 横→縦 / 縦→横 をランダム選択
            const bool horizontal_first = rng.NextBool(0.5f);

            const u32 mid_x = horizontal_first ? bx : ax;
            const u32 mid_y = horizontal_first ? ay : by;

            // 水平区間 [min(ax, mid_x), max(ax, mid_x)] in row = ay or by
            // 垂直区間 [min(ay, mid_y), max(ay, mid_y)] in col = mid_x or ax
            const u32 horiz_y = horizontal_first ? ay : by;
            u32 hx0 = horizontal_first ? ax : ax;
            u32 hx1 = horizontal_first ? mid_x : mid_x;
            if (hx0 > hx1) { const u32 t = hx0; hx0 = hx1; hx1 = t; }

            const u32 vert_x = horizontal_first ? mid_x : ax;
            u32 vy0 = horizontal_first ? mid_y : ay;
            u32 vy1 = horizontal_first ? by    : mid_y;
            if (vy0 > vy1) { const u32 t = vy0; vy0 = vy1; vy1 = t; }

            // 廊下を Corridor で塗る。既に Floor (部屋内) のセルは上書きしない
            // (部屋の床を Corridor に変えてしまうと描画 / IsWalkable は同じだが
            //  Room 範囲との整合性で見栄えが悪い)。
            for (u32 x = hx0; x <= hx1 && x < m_Width; ++x) {
                if (horiz_y >= m_Height) break;
                const usize idx2 = static_cast<usize>(horiz_y) * static_cast<usize>(m_Width)
                                  + static_cast<usize>(x);
                if (m_Grid[idx2] == ETileKind::Wall) m_Grid[idx2] = ETileKind::Corridor;
            }
            for (u32 y = vy0; y <= vy1 && y < m_Height; ++y) {
                if (vert_x >= m_Width) break;
                const usize idx2 = static_cast<usize>(y) * static_cast<usize>(m_Width)
                                  + static_cast<usize>(vert_x);
                if (m_Grid[idx2] == ETileKind::Wall) m_Grid[idx2] = ETileKind::Corridor;
            }

            // 親の代表はランダムに左右どちらかから引き継ぐ
            n.rep_room = rng.NextBool(0.5f) ? L.rep_room : R.rep_room;
        }
    }

    // 6. Stairs を 1 個置く。
    // 仕様: ランダムな部屋に階段。最後の部屋固定だとプレイヤーが学習してしまうので
    //       random pick が望ましい。部屋が無い (極小 grid で leaf に room が
    //       入らなかった) ケースは no-op。
    if (m_Rooms.Num() > 0u) {
        const u32 pick = rng.NextU32() % static_cast<u32>(m_Rooms.Num());
        const FRoom& r = m_Rooms[pick];
        const u32 sx = r.x + r.w / 2u;
        const u32 sy = r.y + r.h / 2u;
        if (sx < m_Width && sy < m_Height) {
            m_Grid[static_cast<usize>(sy) * static_cast<usize>(m_Width)
                + static_cast<usize>(sx)] = ETileKind::Stairs;
        }
    }

    // BspNode 一時配列 / work stack はスコープ脱出で破棄される。
}

} // namespace acs::game
