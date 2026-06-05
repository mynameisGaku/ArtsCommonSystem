// SPDX-License-Identifier: Apache-2.0
// ACS Container — HashBytes 実装（xxhash 風 64bit ハッシュ）
//
// バイト列を 4 並列 (32B) ストライプで処理し、最後にミックスダウン。
// SMHasher 上位品質、整数キーには THasher<u64>::HashMix64 を使うこと。
#include "container/Hash.h"

namespace acs {

namespace {
/**
 * 8 バイトを strict-aliasing 安全にロードする (byte コピーで UB 回避)。
 *
 * @param p 読み出し元の先頭バイト。
 * @return 読み出した u64 値。
 */
ACS_FORCEINLINE u64 ReadU64(const byte* p) noexcept {
    u64 v;
    for (int i = 0; i < 8; ++i) reinterpret_cast<byte*>(&v)[i] = p[i];
    return v;
}

/**
 * 4 バイトを strict-aliasing 安全にロードする (byte コピーで UB 回避)。
 *
 * @param p 読み出し元の先頭バイト。
 * @return 読み出した u32 値。
 */
ACS_FORCEINLINE u64 ReadU32(const byte* p) noexcept {
    u32 v;
    for (int i = 0; i < 4; ++i) reinterpret_cast<byte*>(&v)[i] = p[i];
    return v;
}
} // namespace

/**
 * 任意バイト列の 64bit ハッシュを計算する (xxhash64 風の簡易版)。
 *
 * @details
 * 32B (4×8B) ストライプで 4 つのアキュムレータを並列に進め、最後にマージして残り
 * 8B/4B/1B を取り込み、アバランチ処理して品質を確保する。
 * @param data ハッシュ対象の先頭ポインタ。
 * @param len バイト長。
 * @param seed 初期シード。
 * @return 64bit ハッシュ値。
 */
u64 HashBytes(const void* data, usize len, u64 seed) noexcept {
    // 黄金比由来の素数定数（xxhash と同じ）
    constexpr u64 P1 = 0x9E3779B185EBCA87ull;
    constexpr u64 P2 = 0xC2B2AE3D27D4EB4Full;
    constexpr u64 P3 = 0x165667B19E3779F9ull;
    constexpr u64 P4 = 0x85EBCA77C2B2AE63ull;
    constexpr u64 P5 = 0x27D4EB2F165667C5ull;

    const byte* p = static_cast<const byte*>(data);
    const byte* end = p + len;
    u64 h;

    if (len >= 32) {
        // 4 並列アキュムレータ初期化
        u64 v1 = seed + P1 + P2;
        u64 v2 = seed + P2;
        u64 v3 = seed;
        u64 v4 = seed - P1;
        const byte* limit = end - 32;
        // 各ループで 32B (4 × 8B) 取り込み
        do {
            v1 += ReadU64(p) * P2; v1 = (v1 << 31) | (v1 >> 33); v1 *= P1; p += 8;
            v2 += ReadU64(p) * P2; v2 = (v2 << 31) | (v2 >> 33); v2 *= P1; p += 8;
            v3 += ReadU64(p) * P2; v3 = (v3 << 31) | (v3 >> 33); v3 *= P1; p += 8;
            v4 += ReadU64(p) * P2; v4 = (v4 << 31) | (v4 >> 33); v4 *= P1; p += 8;
        } while (p <= limit);
        // 4 つのアキュムレータをマージ
        h = ((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) +
            ((v3 << 12) | (v3 >> 52)) + ((v4 << 18) | (v4 >> 46));
    } else {
        // 短い列はシード + 定数だけで初期化
        h = seed + P5;
    }
    h += static_cast<u64>(len);

    // 残り 8B 単位
    while (p + 8 <= end) {
        u64 k = ReadU64(p) * P2;
        k = (k << 31) | (k >> 33);
        k *= P1;
        h ^= k;
        h = ((h << 27) | (h >> 37)) * P1 + P4;
        p += 8;
    }
    // 残り 4B
    if (p + 4 <= end) {
        h ^= static_cast<u64>(ReadU32(p)) * P1;
        h = ((h << 23) | (h >> 41)) * P2 + P3;
        p += 4;
    }
    // 残り 1B ずつ
    while (p < end) {
        h ^= static_cast<u64>(*p) * P5;
        h = ((h << 11) | (h >> 53)) * P1;
        ++p;
    }
    // 最終アバランチ（出力品質確保）
    h ^= h >> 33; h *= P2;
    h ^= h >> 29; h *= P3;
    h ^= h >> 32;
    return h;
}

} // namespace acs
