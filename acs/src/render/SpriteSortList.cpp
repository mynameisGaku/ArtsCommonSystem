// SPDX-License-Identifier: Apache-2.0
#include "render/SpriteSortList.h"
#include "render/SpriteBatch.h"
#include "memory/Memory.h"

namespace acs {

namespace {

/** layerを符号付き昇順の32bit fieldへ変換する。 */
constexpr u32 EncodeLayer(i32 layer) noexcept
{
    return static_cast<u32>(layer) ^ 0x80000000u;
}

/**
 * depthをIEEE 754の数値昇順fieldへ変換する。
 *
 * @details 符号付きzeroは同一keyとし、NaNは正の無限大へ寄せて提出順を保つ。
 */
u32 EncodeDepth(f32 depth) noexcept
{
    /** depthのIEEE 754 bit列。 */
    u32 bits = 0u;
    MemCopy(&bits, &depth, sizeof(bits));
    /** 符号を除いた指数と仮数。 */
    const u32 magnitude = bits & 0x7fffffffu;
    if (magnitude == 0u) bits = 0u;
    else if (magnitude > 0x7f800000u) bits = 0x7f800000u;
    return (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
}

/** sprite commandから優先順どおりの64bit keyを作る。 */
u64 BuildSortKey(const FSpriteCmd& command) noexcept
{
    /** layerとdepthを各32bitへ配置するlayout。 */
    using FLayout = TDrawPacketSortKeyLayout<32u, 32u>;
    return FLayout::Insert<0u>(EncodeLayer(command.layer)) | FLayout::Insert<1u>(EncodeDepth(command.depth));
}

} // namespace

void FSpriteSortList::Sort() noexcept {
    /** 並べるcommand数。 */
    const u32 n = static_cast<u32>(m_Cmds.Size());
    m_Order.Clear();
    m_LastSortPasses = 0u;
    m_LastSortItemVisits = 0u;
    if (!m_Order.TryResize(n) || !m_Scratch.TryResize(n) || !m_Keys.TryResize(n)) {
        m_Order.Clear();
        return;
    }
    for (u32 index = 0u; index < n; ++index) {
        m_Order[index] = index;
        m_Keys[index] = BuildSortKey(m_Cmds[index]);
    }
    m_LastSortItemVisits += n;
    if (n < 2u) return;

    /** 小配列でradix初期化を避ける境界。 */
    constexpr u32 kInsertionThreshold = 24u;
    if (n <= kInsertionThreshold) {
        for (u32 index = 1u; index < n; ++index) {
            /** 挿入するcommand番号。 */
            const u32 inserted = m_Order[index];
            /** 挿入する64bit key。 */
            const u64 inserted_key = m_Keys[inserted];
            /** 挿入先を探す位置。 */
            u32 position = index;
            while (position > 0u) {
                ++m_LastSortItemVisits;
                /** 直前commandの64bit key。 */
                const u64 previous_key = m_Keys[m_Order[position - 1u]];
                if (previous_key <= inserted_key) break;
                m_Order[position] = m_Order[position - 1u];
                --position;
            }
            m_Order[position] = inserted;
        }
        return;
    }

    /** 先頭keyとの差分bit集合。 */
    u64 varying_bits = 0u;
    /** 差分計算の基準key。 */
    const u64 first_key = m_Keys[0u];
    for (u32 index = 1u; index < n; ++index) varying_bits |= first_key ^ m_Keys[index];
    m_LastSortItemVisits += n;

    /** 現passの読み取りindex列。 */
    u32* source = m_Order.Data();
    /** 現passの書き込みindex列。 */
    u32* destination = m_Scratch.Data();
    for (u32 byte_index = 0u; byte_index < sizeof(u64); ++byte_index) {
        /** 現byteを抜き出すbit位置。 */
        const u32 shift = byte_index * 8u;
        if (((varying_bits >> shift) & 0xffu) == 0u) continue;
        /** byte値ごとの要素数。 */
        u32 counts[256]{};
        /** byte値ごとの次の書き込み位置。 */
        u32 offsets[256]{};
        for (u32 index = 0u; index < n; ++index) ++counts[(m_Keys[source[index]] >> shift) & 0xffu];
        /** ここまでに数えた要素数。 */
        u32 prefix = 0u;
        for (u32 bucket = 0u; bucket < 256u; ++bucket) {
            offsets[bucket] = prefix;
            prefix += counts[bucket];
        }
        for (u32 index = 0u; index < n; ++index) {
            /** 現要素が入るbyte bucket。 */
            const u32 bucket = static_cast<u32>((m_Keys[source[index]] >> shift) & 0xffu);
            destination[offsets[bucket]++] = source[index];
        }
        /** 次passで読み書きを入れ替える一時pointer。 */
        u32* const swap = source;
        source = destination;
        destination = swap;
        ++m_LastSortPasses;
        m_LastSortItemVisits += static_cast<u64>(n) * 2u;
    }
    if (source != m_Order.Data()) {
        for (u32 index = 0u; index < n; ++index) m_Order[index] = source[index];
        m_LastSortItemVisits += n;
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
