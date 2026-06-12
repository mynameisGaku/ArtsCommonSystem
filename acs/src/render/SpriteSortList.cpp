// SPDX-License-Identifier: Apache-2.0
#include "render/SpriteSortList.h"
#include "render/SpriteBatch.h"

namespace acs {

void FSpriteSortList::Sort() noexcept {
    const u32 n = static_cast<u32>(m_Cmds.Size());
    m_Order.Clear();
    m_Order.Reserve(n);
    for (u32 i = 0; i < n; ++i) m_Order.PushBack(i);
    if (n < 2) return;

    u32* const ord = m_Order.Data();

    // 安定挿入ソート。比較は (layer 昇順, depth 昇順, seq 昇順) の総順序。
    // seq が一意増加なので同 layer/depth は挿入順を保つ (= 安定)。
    for (u32 i = 1; i < n; ++i) {
        const u32 ti = ord[i];
        const FSpriteCmd& tc = m_Cmds[ti];
        u32 j = i;
        while (j > 0) {
            const FSpriteCmd& pc = m_Cmds[ord[j - 1]];
            // prev を ti の「後ろ (= 手前)」に描くべきか。
            bool prev_after;
            if (pc.layer != tc.layer)      prev_after = (pc.layer > tc.layer);
            else if (pc.depth != tc.depth) prev_after = (pc.depth > tc.depth);
            else                           prev_after = (pc.seq   > tc.seq);
            if (!prev_after) break;
            ord[j] = ord[j - 1];
            --j;
        }
        ord[j] = ti;
    }
}

void FSpriteSortList::Replay(FSpriteBatch& sb) const noexcept {
    const u32 n = static_cast<u32>(m_Cmds.Size());
    const bool sorted = (m_Order.Size() == m_Cmds.Size());
    for (u32 k = 0; k < n; ++k) {
        const FSpriteCmd& c = sorted ? m_Cmds[m_Order[k]] : m_Cmds[k];
        if (c.kind == ESpriteCmdKind::Rect) {
            sb.DrawRect(c.x, c.y, c.w, c.h, c.color);
        } else if (c.tex != nullptr) {
            sb.DrawSub(*c.tex, c.x, c.y, c.w, c.h, c.u0, c.v0, c.u1, c.v1, c.color);
        }
    }
}

} // namespace acs
