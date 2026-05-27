// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (puzzle / match-3) — FMatchGrid 実装
#include "gameframework/MatchGrid.h"

namespace acs::game {

namespace {

// 範囲外 Get / 初期状態の安全なフォールバック値。
// const-ref 返却なので static lifetime が必要。
const GridCell kEmptyCell{};

inline bool IsAdjacent(u32 x1, u32 y1, u32 x2, u32 y2) noexcept {
    // 4-neighborhood: 横 or 縦に距離 1
    const u32 dx = (x1 > x2) ? (x1 - x2) : (x2 - x1);
    const u32 dy = (y1 > y2) ? (y1 - y2) : (y2 - y1);
    return (dx == 1u && dy == 0u) || (dx == 0u && dy == 1u);
}

} // namespace

// ---------------------------------------------------------------------------
// 初期化 / 基本アクセス
// ---------------------------------------------------------------------------
void FMatchGrid::Init(u32 width, u32 height, u32 color_count) noexcept {
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (color_count == 0) color_count = 1;
    if (color_count > 255u) color_count = 255u;  // u8 範囲制限

    m_Width        = width;
    m_Height       = height;
    m_ColorCount  = color_count;
    m_ChainLevel  = 0;
    m_TotalCleared = 0;

    const usize n = static_cast<usize>(width) * static_cast<usize>(height);
    m_Cells.Clear();
    m_Cells.Resize(n);   // GridCell は trivially-constructible → 0 埋め (empty=true)
    for (usize i = 0; i < n; ++i) {
        m_Cells[i] = GridCell{};  // 明示的に既定値 (color=0/special=0/empty=true)
    }
}

const GridCell& FMatchGrid::Get(u32 x, u32 y) const noexcept {
    if (x >= m_Width || y >= m_Height) return kEmptyCell;
    return m_Cells[Idx(x, y)];
}

void FMatchGrid::Set(u32 x, u32 y, u8 color, ESpecialKind special) noexcept {
    if (x >= m_Width || y >= m_Height) return;
    GridCell& c = m_Cells[Idx(x, y)];
    if (color == 0u) {
        c.color   = 0u;
        c.special = static_cast<u8>(ESpecialKind::Normal);
        c.empty   = true;
    } else {
        c.color   = color;
        c.special = static_cast<u8>(special);
        c.empty   = false;
    }
}

void FMatchGrid::ClearAll() noexcept {
    const usize n = m_Cells.Size();
    for (usize i = 0; i < n; ++i) m_Cells[i] = GridCell{};
}

void FMatchGrid::ResetChain() noexcept {
    m_ChainLevel   = 0;
    m_TotalCleared = 0;
}

void FMatchGrid::SetOnClearCallback(ClearCallback cb, void* user) noexcept {
    m_OnClear      = cb;
    m_OnClearUser = user;
}

// ---------------------------------------------------------------------------
// FillRandom (no-match invariant)
// ---------------------------------------------------------------------------
u8 FMatchGrid::PickColorAvoidingMatch(u32 x, u32 y, FRandom& rng) const noexcept {
    // 候補色 1..color_count から、左 2 個 / 上 2 個が同色になる色を弾く。
    // color_count == 1 の場合は invariant を諦める (常に同色になる)。
    const u8 left1 = (x >= 1u) ? m_Cells[Idx(x - 1u, y)].color : 0u;
    const u8 left2 = (x >= 2u) ? m_Cells[Idx(x - 2u, y)].color : 0u;
    const u8 up1   = (y >= 1u) ? m_Cells[Idx(x, y - 1u)].color : 0u;
    const u8 up2   = (y >= 2u) ? m_Cells[Idx(x, y - 2u)].color : 0u;

    const bool ban_horiz = (left1 != 0u && left1 == left2);
    const bool ban_vert  = (up1   != 0u && up1   == up2);

    // 通常の rolling: 1..color_count から 1 個。被ったら別色にずらす。
    u8 c = static_cast<u8>(1u + (rng.NextU32() % m_ColorCount));
    // 最悪でも color_count 個試せば被らない色が見つかる (color_count >= 2 のとき)。
    // color_count == 1 では何も避けられないのでそのまま返す。
    if (m_ColorCount >= 2u) {
        for (u32 guard = 0; guard < m_ColorCount; ++guard) {
            const bool conflict_h = ban_horiz && (c == left1);
            const bool conflict_v = ban_vert  && (c == up1);
            if (!conflict_h && !conflict_v) break;
            // 次の色へ (1..color_count を循環)
            c = static_cast<u8>(1u + (c % m_ColorCount));
        }
    }
    return c;
}

void FMatchGrid::FillRandom(u32 seed) noexcept {
    // seed==0 ならグローバル、それ以外は新規ローカル FRandom。
    // (ヘッダ API 上 seed は u32 だが、内部 FRandom は u64 seed なので拡張して渡す)。
    FRandom local(static_cast<u64>(seed));
    FRandom& rng = (seed == 0u) ? FRandom::Global() : local;

    for (u32 y = 0; y < m_Height; ++y) {
        for (u32 x = 0; x < m_Width; ++x) {
            const u8 c = PickColorAvoidingMatch(x, y, rng);
            GridCell& cell = m_Cells[Idx(x, y)];
            cell.color   = c;
            cell.special = static_cast<u8>(ESpecialKind::Normal);
            cell.empty   = (c == 0u);
        }
    }
}

// ---------------------------------------------------------------------------
// マッチ検出 (3+ 連続を水平 / 垂直 2 パス走査)
// ---------------------------------------------------------------------------
u32 FMatchGrid::DetectMatches(MatchInfo* out_matches, u32 max_matches) const noexcept {
    u32 count = 0;

    // 水平 (各行で left→right に同色 run を畳む)
    for (u32 y = 0; y < m_Height; ++y) {
        u32 run_start = 0;
        u8  run_color = 0;
        u32 run_len   = 0;
        for (u32 x = 0; x < m_Width; ++x) {
            const GridCell& c = m_Cells[Idx(x, y)];
            const u8 col = c.empty ? 0u : c.color;
            if (col != 0u && col == run_color) {
                ++run_len;
            } else {
                if (run_len >= 3u && run_color != 0u) {
                    if (out_matches != nullptr && count < max_matches) {
                        out_matches[count] = MatchInfo{run_start, y, run_len, true, run_color};
                    }
                    ++count;
                }
                run_color = col;
                run_start = x;
                run_len   = (col != 0u) ? 1u : 0u;
            }
        }
        // 行末の run を flush
        if (run_len >= 3u && run_color != 0u) {
            if (out_matches != nullptr && count < max_matches) {
                out_matches[count] = MatchInfo{run_start, y, run_len, true, run_color};
            }
            ++count;
        }
    }

    // 垂直 (各列で top→bottom に同色 run を畳む)
    for (u32 x = 0; x < m_Width; ++x) {
        u32 run_start = 0;
        u8  run_color = 0;
        u32 run_len   = 0;
        for (u32 y = 0; y < m_Height; ++y) {
            const GridCell& c = m_Cells[Idx(x, y)];
            const u8 col = c.empty ? 0u : c.color;
            if (col != 0u && col == run_color) {
                ++run_len;
            } else {
                if (run_len >= 3u && run_color != 0u) {
                    if (out_matches != nullptr && count < max_matches) {
                        out_matches[count] = MatchInfo{x, run_start, run_len, false, run_color};
                    }
                    ++count;
                }
                run_color = col;
                run_start = y;
                run_len   = (col != 0u) ? 1u : 0u;
            }
        }
        if (run_len >= 3u && run_color != 0u) {
            if (out_matches != nullptr && count < max_matches) {
                out_matches[count] = MatchInfo{x, run_start, run_len, false, run_color};
            }
            ++count;
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// swap 試行 (マッチ非発生時は戻す)
// ---------------------------------------------------------------------------
bool FMatchGrid::TrySwap(u32 x1, u32 y1, u32 x2, u32 y2) noexcept {
    if (x1 >= m_Width || y1 >= m_Height) return false;
    if (x2 >= m_Width || y2 >= m_Height) return false;
    if (!IsAdjacent(x1, y1, x2, y2))   return false;

    GridCell& a = m_Cells[Idx(x1, y1)];
    GridCell& b = m_Cells[Idx(x2, y2)];
    // 空 cell は swap 対象外 (落下中とみなす)。
    if (a.empty || b.empty) return false;

    // swap
    GridCell tmp = a;
    a = b;
    b = tmp;

    // マッチ判定: 1 個でも検出されれば valid。
    const u32 cnt = DetectMatches(nullptr, 0u);
    if (cnt == 0u) {
        // 戻す
        GridCell back = a;
        a = b;
        b = back;
        return false;
    }

    // 連鎖を解消して chain を進める
    ResolveAllMatches();
    return true;
}

// ---------------------------------------------------------------------------
// 個別消去 + special 効果 (visited で再入防止)
// ---------------------------------------------------------------------------
void FMatchGrid::ClearOne(u32 x, u32 y, TArray<u8>& visited) noexcept {
    if (x >= m_Width || y >= m_Height) return;
    const usize i = Idx(x, y);
    if (visited[i]) return;
    visited[i] = 1u;

    GridCell& c = m_Cells[i];
    if (c.empty) return;

    const u8 color           = c.color;
    const ESpecialKind sp     = static_cast<ESpecialKind>(c.special);

    // 先に「空」にしてから special を波及させる (再入時の二重カウント防止)。
    c.color   = 0u;
    c.special = static_cast<u8>(ESpecialKind::Normal);
    c.empty   = true;

    ++m_TotalCleared;
    if (m_OnClear != nullptr) {
        m_OnClear(m_OnClearUser, x, y, color, sp);
    }

    // special 波及
    if (sp != ESpecialKind::Normal) {
        ApplySpecialEffect(x, y, color, sp, visited);
    }
}

void FMatchGrid::ApplySpecialEffect(u32 x, u32 y, u8 color, ESpecialKind sp,
                                   TArray<u8>& visited) noexcept {
    switch (sp) {
    case ESpecialKind::Bomb: {
        // 3x3 範囲 (中心は既に消去済み、visited フラグで弾かれる)
        const u32 x0 = (x >= 1u) ? (x - 1u) : 0u;
        const u32 y0 = (y >= 1u) ? (y - 1u) : 0u;
        const u32 x1 = (x + 1u < m_Width)  ? (x + 1u) : (m_Width  - 1u);
        const u32 y1 = (y + 1u < m_Height) ? (y + 1u) : (m_Height - 1u);
        for (u32 yy = y0; yy <= y1; ++yy) {
            for (u32 xx = x0; xx <= x1; ++xx) {
                ClearOne(xx, yy, visited);
            }
        }
        break;
    }
    case ESpecialKind::Lightning: {
        // 同行 + 同列
        for (u32 xx = 0; xx < m_Width;  ++xx) ClearOne(xx, y, visited);
        for (u32 yy = 0; yy < m_Height; ++yy) ClearOne(x, yy, visited);
        break;
    }
    case ESpecialKind::Rainbow: {
        // 盤面の同色全消去 (Rainbow が起動した時点の color を対象)。
        // color==0 (理論上発生しない) のときは no-op。
        if (color == 0u) break;
        for (u32 yy = 0; yy < m_Height; ++yy) {
            for (u32 xx = 0; xx < m_Width; ++xx) {
                const GridCell& cc = m_Cells[Idx(xx, yy)];
                if (!cc.empty && cc.color == color) {
                    ClearOne(xx, yy, visited);
                }
            }
        }
        break;
    }
    case ESpecialKind::Normal:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 重力 / 補充
// ---------------------------------------------------------------------------
u32 FMatchGrid::ApplyGravity() noexcept {
    u32 moved = 0;
    // 列ごとに下から走査。空 cell より上の最初の非空を見つけて下に落とす。
    for (u32 x = 0; x < m_Width; ++x) {
        // write pointer: 列の下から積み上げる位置。
        // 下から上に向かって走査し、非空をその場で保持、空はスキップしつつ
        // 上から非空を引き寄せる方式。
        i32 write_y = static_cast<i32>(m_Height) - 1;
        for (i32 read_y = static_cast<i32>(m_Height) - 1; read_y >= 0; --read_y) {
            GridCell& src = m_Cells[Idx(x, static_cast<u32>(read_y))];
            if (!src.empty) {
                if (read_y != write_y) {
                    GridCell& dst = m_Cells[Idx(x, static_cast<u32>(write_y))];
                    dst = src;
                    src = GridCell{};  // 空にする
                    ++moved;
                }
                --write_y;
            }
        }
        // write_y より上は空になっている (落下の結果)
    }
    return moved;
}

u32 FMatchGrid::RefillFromTop() noexcept {
    u32 filled = 0;
    if (m_ColorCount == 0u) return 0;
    // 上端の空 cell をランダム色で埋める (1 step)。
    // 落下 + refill のサイクルを ResolveAllMatches で回す前提なので、
    // ここでは「最上段が空なら埋める」だけで十分 (gravity が次サイクルで下に流す)。
    FRandom& rng = FRandom::Global();
    for (u32 x = 0; x < m_Width; ++x) {
        GridCell& c = m_Cells[Idx(x, 0u)];
        if (c.empty) {
            c.color   = static_cast<u8>(1u + (rng.NextU32() % m_ColorCount));
            c.special = static_cast<u8>(ESpecialKind::Normal);
            c.empty   = false;
            ++filled;
        }
    }
    return filled;
}

// ---------------------------------------------------------------------------
// resolve サイクル (detect → clear → gravity → refill を stable まで反復)
// ---------------------------------------------------------------------------
u32 FMatchGrid::ResolveAllMatches() noexcept {
    const u32 cleared_before = m_TotalCleared;
    const usize n = m_Cells.Size();

    // 一時バッファ: マッチ検出結果 (最大 = 行マッチ + 列マッチ ≒ w+h 個程度)。
    // 余裕を持って w*h を上限にする (実際は遥かに少ない)。
    TArray<MatchInfo> matches;
    matches.Resize(n);

    // visited は ClearOne 再入防止用 (cascade ループ間で reset)。
    TArray<u8> visited;
    visited.Resize(n);

    // 連鎖が止まるまで (= マッチ検出 0) ループ。
    // 安全ガード: 理論上 width*height 回以内には必ず止まる (毎ループ少なくとも
    // 3 cell 減るため)。それでも暴走防止に固定上限を設ける。
    const u32 safety_limit = (m_Width * m_Height) + 8u;
    for (u32 iter = 0; iter < safety_limit; ++iter) {
        const u32 m = DetectMatches(matches.Data(), static_cast<u32>(matches.Size()));
        if (m == 0u) break;

        // visited を 0 クリアして消去フェーズ
        for (usize i = 0; i < n; ++i) visited[i] = 0u;

        // 各マッチ範囲を ClearOne
        for (u32 mi = 0; mi < m && mi < static_cast<u32>(matches.Size()); ++mi) {
            const MatchInfo& info = matches[mi];
            if (info.horizontal) {
                for (u32 k = 0; k < info.length; ++k) {
                    ClearOne(info.start_x + k, info.start_y, visited);
                }
            } else {
                for (u32 k = 0; k < info.length; ++k) {
                    ClearOne(info.start_x, info.start_y + k, visited);
                }
            }
        }

        // 重力 + 補充を fix-point (最大 height 回 → height step で必ず落ちきる)。
        for (u32 g = 0; g < m_Height; ++g) {
            const u32 moved = ApplyGravity();
            if (moved == 0u) break;
        }
        // 上端から補充 → 再度落下を回し、列が満杯になるまで反復。
        for (u32 r = 0; r < m_Height; ++r) {
            const u32 filled = RefillFromTop();
            if (filled == 0u) break;
            // 落とす
            for (u32 g = 0; g < m_Height; ++g) {
                const u32 moved = ApplyGravity();
                if (moved == 0u) break;
            }
        }

        ++m_ChainLevel;
    }

    return m_TotalCleared - cleared_before;
}

} // namespace acs::game
