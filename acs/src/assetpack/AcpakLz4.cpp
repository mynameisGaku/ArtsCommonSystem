// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — FAcpakLz4 実装 (自前 LZ4 block format)
// -----------------------------------------------------------------------------
// LZ4 block format の self-contained 実装。公式 reference (lz4.c) より単純な
// hash table ベースの "fast" 1 パススキャナ。圧縮率は公式の 80-90% 程度だが、
// 構造は素直で third_party 依存ゼロ。
//
// Compressor 構造:
//   ・src を 4 バイト anchor で hash → 16K エントリの hash table に index 保存。
//   ・現在位置で hash 衝突 (= 同 4 バイト) を検出したら match を伸ばす。
//   ・最長 match を見つけたら (anchor〜現在位置) の literals + offset + match
//     を 1 sequence として書き出す。
//   ・終端処理: 末尾 12 バイトは match を作らず、ぜんぶ literal で出す
//     (LZ4 仕様の LZ4_LAST_LITERALS = 5 + 12 マージン)。
//
// Decompressor 構造:
//   ・公式 LZ4_decompress_safe と同じく境界検査付き 1 パスループ。
//   ・各 sequence: token (1B) → literal_length 拡張 (0+B) → literals (Nbytes)
//                  → offset (2B LE) → match_length 拡張 (0+B) → 後方参照コピー
//   ・最後の sequence は match を持たない (literal だけで終わる)。
// =============================================================================
#include "assetpack/AcpakLz4.h"

#include "assetpack/AcpakFormat.h"   // kAcpakSub*
#include "memory/Memory.h"            // MemCopy / MemSet

namespace acs::assetpack {

namespace {

/** LZ4 の最低 match 長 (= 4)。token の low 4-bit は match_length - 4 を表す。 */
constexpr u32 kMinMatch        = 4;

/**
 * end-of-block 直前で literal でなければならない末尾バイト数 (= 5)。
 *
 * @details Compressor は match をこの位置より前で伸ばし切る (LZ4 仕様)。
 */
constexpr u32 kLastLiterals    = 5;

/**
 * 末尾で match 探索を打ち切るマージン (= 12 バイト)。
 *
 * @details
 * 最後の match の終端から end-of-block まで最低 12 バイトを literal とする
 * (LZ4 の LZ4_LAST_LITERALS 規約)。Compressor はこの 12 バイト手前で match
 * 探索を止める。
 */
constexpr u32 kMatchFindLimit  = 12;

/** 後方参照の最大 offset (= 65535、16bit LE)。1〜65535、0 は不正。 */
constexpr u32 kMaxOffset       = 65535;

/** Hash table のエントリ数 (= 16384、64KB)。LZ4 fast の典型値。 */
constexpr u32 kHashTableSize   = 1u << 14;

/** Hash table index 用のマスク (= kHashTableSize - 1)。 */
constexpr u32 kHashTableMask   = kHashTableSize - 1;

/**
 * 4 バイトを strict-aliasing 安全に u32 (LE) として読む。
 *
 * @param p 読み出し元 (4 バイト)。
 * @return 読み出した u32。
 */
inline u32 Read32LE(const u8* p) noexcept {
    u32 v = 0;
    MemCopy(&v, p, 4);
    return v;
}

/**
 * 2 バイトを u16 (LE) として読む。
 *
 * @param p 読み出し元 (2 バイト)。
 * @return 読み出した u16。
 */
inline u16 Read16LE(const u8* p) noexcept {
    u16 v = 0;
    MemCopy(&v, p, 2);
    return v;
}

/**
 * u16 を 2 バイト LE で書き出す。
 *
 * @param p 書き込み先 (2 バイト)。
 * @param v 書き込む u16。
 */
inline void Write16LE(u8* p, u16 v) noexcept {
    MemCopy(p, &v, 2);
}

/**
 * 4 バイトシーケンスを multiplicative hash で hash table index に変換する。
 *
 * @details FNV ベースの 32 → 14bit hash。衝突しやすいが LZ4 fast として実用的。
 * @param seq hash 対象の 4 バイトシーケンス。
 * @return hash table の index (0..kHashTableMask)。
 */
inline u32 HashSequence(u32 seq) noexcept {
    return ((seq * 2654435761u) >> (32 - 14)) & kHashTableMask;
}

} // namespace

/** 入力サイズから最悪ケースの圧縮出力サイズを算出する (LZ4 公式式)。 */
u32 FAcpakLz4::MaxCompressedSize(u32 input_size) noexcept {
    // LZ4 公式式: input + (input / 255) + 16。全 literal 化の最悪ケースで
    // 255 バイトごとに extension byte 1 + token 1 + 終端余裕 16 を確保。
    return input_size + (input_size / 255u) + 16u;
}

/** src を LZ4 block format で dst に圧縮し、圧縮後バイト数を返す。 */
TResult<u32> FAcpakLz4::Compress(const u8* src,
                               u32       src_size,
                               u8*       dst,
                               u32       dst_capacity) noexcept {
    // ---- 引数チェック ----------------------------------------------------
    if (dst == nullptr || dst_capacity == 0) {
        return ACS_ERR(Asset, kAcpakSubLz4BadInput,
                       "FAcpakLz4::Compress: dst is null / empty");
    }
    if (src == nullptr && src_size > 0) {
        return ACS_ERR(Asset, kAcpakSubLz4BadInput,
                       "FAcpakLz4::Compress: src is null but size > 0");
    }
    if (src_size == 0) {
        return TResult<u32>(OkInit, 0u);
    }

    // ---- 出力 cursor / 容量チェック helper ------------------------------
    u8* const dst_begin = dst;
    u8* const dst_end   = dst + dst_capacity;
    u8*       op        = dst;   // 出力 cursor

    // 容量不足を 1 箇所で検査する lambda。dst overflow は呼び出し側で
    // MaxCompressedSize を渡しているなら起きないが、defensive に常時検査。
    auto check_dst = [&](usize need) noexcept -> bool {
        return (op + need) <= dst_end;
    };

    // 短すぎる src は全 literal 1 sequence で出す。
    // LZ4 仕様で「最後の 5 バイトは必ず literal」「match 探索開始は
    // src + 1 から、終端 12 バイト手前まで」なので、src_size < kMatchFindLimit
    // + 1 (= 13) なら match を作らず literal only。
    if (src_size < kMinMatch + kMatchFindLimit) {
        // token + (lit_len_ext bytes) + literals
        const u32 lit_len = src_size;
        // 容量: 1 token + ceil(lit_len/255) ext + lit_len
        const u32 ext_bytes = (lit_len >= 15u) ? ((lit_len - 15u) / 255u + 1u) : 0u;
        if (!check_dst(1u + ext_bytes + lit_len)) {
            return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                           "FAcpakLz4::Compress: dst overflow (short input)");
        }
        // token: high 4-bit = literal_length (15 = "拡張あり")
        const u8 token = (lit_len >= 15u) ? (15u << 4) : static_cast<u8>(lit_len << 4);
        *op++ = token;
        if (lit_len >= 15u) {
            u32 remain = lit_len - 15u;
            while (remain >= 0xFFu) { *op++ = 0xFF; remain -= 0xFFu; }
            *op++ = static_cast<u8>(remain);
        }
        MemCopy(op, src, lit_len);
        op += lit_len;
        return TResult<u32>(OkInit, static_cast<u32>(op - dst_begin));
    }

    // ---- Hash table 初期化 (0 = 「未登録」相当として扱う) --------------
    // 値は src 先頭からの byte index + 1 (= 0 を sentinel に流用)。
    // しかし src の先頭バイトと衝突を区別できないので、別途 i32 + バイアスで
    // 「-1」を sentinel として表現する手もある。ここでは「u32 index + 1」を
    // 採用し、0 は「未登録」。
    static thread_local u32 hash_table[kHashTableSize];
    MemSet(hash_table, 0, sizeof(hash_table));

    // 終端から match 探索を打ち切る位置 (src 先頭からの byte index)。
    // src + match_end までは match 候補にできる。
    // src の最後の 12 バイトは literal 専用。
    const u32 match_end       = src_size - kMatchFindLimit;
    // 末尾の 5 バイトより前で match を伸ばし切る (LZ4 仕様)。
    const u32 match_limit_end = src_size - kLastLiterals;

    u32 anchor = 0;   // 直近 sequence の literal 開始 (= 前 match の終端 + 1)
    u32 ip     = 0;   // 現在の探索 cursor

    // 先頭 1 バイトは hash 登録だけ行って前進 (LZ4 fast は ip = 1 から)。
    {
        const u32 seq = Read32LE(src + ip);
        hash_table[HashSequence(seq)] = ip + 1u;
        ++ip;
    }

    while (ip < match_end) {
        // ---- 現在位置で 4 バイト hash を引き、過去位置を取得 -------------
        const u32 seq    = Read32LE(src + ip);
        const u32 h      = HashSequence(seq);
        const u32 ref_p1 = hash_table[h];   // 過去位置 + 1 (0 = 未登録)
        hash_table[h]    = ip + 1u;

        // 未登録 or offset 超過 or 4 バイト一致しない → 1 進めて再試行
        if (ref_p1 == 0) { ++ip; continue; }
        const u32 ref = ref_p1 - 1u;
        if (ip <= ref || (ip - ref) > kMaxOffset) { ++ip; continue; }
        if (Read32LE(src + ref) != seq) { ++ip; continue; }

        // ---- match 発見: 長さを伸ばす ----------------------------------
        // 最初の 4 バイトは確定済 (seq 比較で一致)。それ以降を 1 バイトずつ
        // (公式実装は 8 バイト単位だが単純化のため 1 バイト) 比較する。
        u32 match_len = kMinMatch;
        const u32 max_match = match_limit_end - ip;  // ip + match_len が
                                                     // match_limit_end 以下に
                                                     // 収まる範囲
        while (match_len < max_match &&
               src[ref + match_len] == src[ip + match_len]) {
            ++match_len;
        }

        // ---- sequence 書き出し: token + lit_ext + literals + offset + mat_ext
        const u32 literal_len = ip - anchor;
        const u16 offset      = static_cast<u16>(ip - ref);
        const u32 mat_excess  = match_len - kMinMatch;   // token に格納する値

        // 必要な容量を見積もって 1 度に確認する。
        const u32 lit_ext_bytes = (literal_len >= 15u)
                                      ? ((literal_len - 15u) / 255u + 1u) : 0u;
        const u32 mat_ext_bytes = (mat_excess  >= 15u)
                                      ? ((mat_excess  - 15u) / 255u + 1u) : 0u;
        const usize need = 1u + lit_ext_bytes + literal_len + 2u + mat_ext_bytes;
        if (!check_dst(need)) {
            return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                           "FAcpakLz4::Compress: dst overflow (mid sequence)");
        }

        // token byte
        const u8 lit_token = (literal_len >= 15u) ? 15u : static_cast<u8>(literal_len);
        const u8 mat_token = (mat_excess  >= 15u) ? 15u : static_cast<u8>(mat_excess);
        *op++ = static_cast<u8>((lit_token << 4) | mat_token);

        // literal length 拡張
        if (literal_len >= 15u) {
            u32 rem = literal_len - 15u;
            while (rem >= 0xFFu) { *op++ = 0xFF; rem -= 0xFFu; }
            *op++ = static_cast<u8>(rem);
        }

        // literals 本体
        if (literal_len > 0) {
            MemCopy(op, src + anchor, literal_len);
            op += literal_len;
        }

        // offset (2B LE)
        Write16LE(op, offset);
        op += 2;

        // match length 拡張
        if (mat_excess >= 15u) {
            u32 rem = mat_excess - 15u;
            while (rem >= 0xFFu) { *op++ = 0xFF; rem -= 0xFFu; }
            *op++ = static_cast<u8>(rem);
        }

        // ---- 後処理: ip を match の末尾に進め、その手前 2 ポジションを
        //              hash table に登録しておくと圧縮率が少し上がる ------
        ip += match_len;
        anchor = ip;
        if (ip < match_end) {
            const u32 hh = HashSequence(Read32LE(src + ip - 2u));
            hash_table[hh] = ip - 2u + 1u;
        }
    }

    // ---- 残り literal を最後の sequence として書き出す ------------------
    const u32 trailing = src_size - anchor;
    {
        const u32 lit_ext_bytes = (trailing >= 15u)
                                      ? ((trailing - 15u) / 255u + 1u) : 0u;
        const usize need = 1u + lit_ext_bytes + trailing;
        if (!check_dst(need)) {
            return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                           "FAcpakLz4::Compress: dst overflow (trailing literals)");
        }
        const u8 lit_token = (trailing >= 15u) ? 15u : static_cast<u8>(trailing);
        *op++ = static_cast<u8>(lit_token << 4);
        if (trailing >= 15u) {
            u32 rem = trailing - 15u;
            while (rem >= 0xFFu) { *op++ = 0xFF; rem -= 0xFFu; }
            *op++ = static_cast<u8>(rem);
        }
        if (trailing > 0) {
            MemCopy(op, src + anchor, trailing);
            op += trailing;
        }
    }

    return TResult<u32>(OkInit, static_cast<u32>(op - dst_begin));
}

/** LZ4 block format の src を境界検査付きで dst に解凍し、解凍後バイト数を返す。 */
TResult<u32> FAcpakLz4::Decompress(const u8* src,
                                 u32       src_size,
                                 u8*       dst,
                                 u32       dst_capacity) noexcept {
    if (src_size == 0) {
        return TResult<u32>(OkInit, 0u);
    }
    if (src == nullptr || dst == nullptr) {
        return ACS_ERR(Asset, kAcpakSubLz4BadInput,
                       "FAcpakLz4::Decompress: src/dst is null");
    }
    if (dst_capacity == 0) {
        return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                       "FAcpakLz4::Decompress: dst_capacity == 0");
    }

    const u8* const src_begin = src;
    const u8* const src_end   = src + src_size;
    u8* const       dst_begin = dst;
    u8* const       dst_end   = dst + dst_capacity;

    const u8* ip = src;
    u8*       op = dst;

    while (ip < src_end) {
        // ---- token 1 バイト ---------------------------------------------
        const u8  token   = *ip++;
        u32       lit_len = (token >> 4) & 0x0Fu;
        u32       mat_len = (token      ) & 0x0Fu;   // = match_length - 4

        // ---- literal length 拡張 ----------------------------------------
        if (lit_len == 15u) {
            while (true) {
                if (ip >= src_end) {
                    return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                                   "FAcpakLz4::Decompress: truncated literal_length");
                }
                const u8 b = *ip++;
                lit_len += b;
                if (b != 0xFF) break;
                // u32 overflow ガード (悪意のあるストリームに対する防御)
                if (lit_len > 0x10000000u) {
                    return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                                   "FAcpakLz4::Decompress: literal_length overflow");
                }
            }
        }

        // ---- literals コピー --------------------------------------------
        if (lit_len > 0) {
            // 残り src バイトと残り dst バイトが十分か検査
            if (static_cast<usize>(src_end - ip) < lit_len) {
                return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                               "FAcpakLz4::Decompress: src exhausted in literals");
            }
            if (static_cast<usize>(dst_end - op) < lit_len) {
                return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                               "FAcpakLz4::Decompress: dst exhausted in literals");
            }
            MemCopy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        // ---- 最後の sequence の判定 --------------------------------------
        // 最後の sequence は literal のみで match を含まない。
        // ip == src_end になっていたらそこで終了。
        if (ip == src_end) {
            break;
        }

        // ---- offset (2B LE) ---------------------------------------------
        if (static_cast<usize>(src_end - ip) < 2u) {
            return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                           "FAcpakLz4::Decompress: src exhausted before offset");
        }
        const u16 offset = Read16LE(ip);
        ip += 2;
        if (offset == 0) {
            return ACS_ERR(Asset, kAcpakSubLz4BadOffset,
                           "FAcpakLz4::Decompress: offset == 0");
        }
        // dst 内の参照位置 = op - offset。dst_begin より前を指してはならない。
        if (static_cast<u32>(op - dst_begin) < offset) {
            return ACS_ERR(Asset, kAcpakSubLz4BadOffset,
                           "FAcpakLz4::Decompress: offset beyond dst start");
        }
        const u8* match_p = op - offset;

        // ---- match length 拡張 ------------------------------------------
        if (mat_len == 15u) {
            while (true) {
                if (ip >= src_end) {
                    return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                                   "FAcpakLz4::Decompress: truncated match_length");
                }
                const u8 b = *ip++;
                mat_len += b;
                if (b != 0xFF) break;
                if (mat_len > 0x10000000u) {
                    return ACS_ERR(Asset, kAcpakSubLz4SrcOverflow,
                                   "FAcpakLz4::Decompress: match_length overflow");
                }
            }
        }
        // 最終的な match 長 = mat_len + kMinMatch (= 4)
        const u32 full_match = mat_len + kMinMatch;

        // dst overflow check
        if (static_cast<usize>(dst_end - op) < full_match) {
            return ACS_ERR(Asset, kAcpakSubLz4DstOverflow,
                           "FAcpakLz4::Decompress: dst exhausted in match");
        }

        // ---- match コピー (overlap 対応の byte-wise copy) ----------------
        // offset == 1 のとき (= run-length encoding) や offset < match_length の
        // とき、書き込みが参照に追いつくため MemCopy 系は使えない。バイト単位で
        // forward copy する。
        for (u32 i = 0; i < full_match; ++i) {
            op[i] = match_p[i];
        }
        op += full_match;
    }

    return TResult<u32>(OkInit, static_cast<u32>(op - dst_begin));
}

} // namespace acs::assetpack
