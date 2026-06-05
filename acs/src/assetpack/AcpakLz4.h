// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — LZ4 block format 自前実装 (third_party 依存ゼロ)
// -----------------------------------------------------------------------------
// `.acpak` v2 で各エントリを圧縮するための自己完結 LZ4 実装。LZ4 は LZ77 系の
// 「リテラル + 後方参照 (offset 1〜65535、length ≥4)」を 1 バイトトークンで
// 表現する高速圧縮形式。
//
// なぜ自前実装か:
//   ・ACS の third_party 方針は「OS 同梱以外原則 NG」(FAssetPack.md §9)。
//   ・LZ4 block format は仕様が公開されており、200 LOC 程度で正しく実装可能。
//   ・出荷ビルドで他人の MIT/BSD ライセンス文を vendor に追加したくない。
//   ・公式実装の極限速度 (5 GB/s+) は必要ない — 起動時の数 MB を 100 MB/s で
//     展開できれば充分。本実装は単純なテーブル探索で実用速度を目指す。
//
// ブロックフォーマット (公式仕様):
//   sequence := token + (lit_len_extension)* + literals + offset + (mat_len_ext)*
//     ┌───────────────┐
//     │  token (1B)   │  high 4-bit = literal_length (0-15)
//     │               │  low  4-bit = match_length - 4 (0-15)
//     ├───────────────┤
//     │ lit_len_ext   │  literal_length==15 のとき続く: 0xFF が並び、最後の
//     │  (0+ bytes)   │  非 0xFF バイトで合計を確定 (e.g. 17 = "15 + 0xFF" 不可
//     │               │  だが "15 + 0x02" は OK)。実際は 0xFF を len-15 < 0xFF
//     │               │  になるまで足す。
//     ├───────────────┤
//     │ literals      │  literal_length バイトをそのままコピー
//     ├───────────────┤
//     │ offset (2B)   │  little-endian、後方参照の距離 (1-65535)。0 は禁止。
//     │               │  最後の sequence は match を含まない (literal のみ)。
//     ├───────────────┤
//     │ mat_len_ext   │  match_length-4 == 15 のとき続く: lit_len と同じ規約
//     │  (0+ bytes)   │
//     └───────────────┘
//
// 終端条件:
//   ・最後の sequence は literal のみで match を含まない。
//   ・最後の match から end-of-block までの距離: ≥ 12 バイト (LZ4_LAST_LITERALS)。
//   ・最後の 5 バイトは literal でなければならない。
//   ・最低圧縮ブロックサイズ 13 バイト未満は素通し (= 全 literal)。
//
// 安全性:
//   ・Decompress は全ループで境界 (src_size / dst_capacity) を検査し、不正
//     ブロック (truncated / 過大 offset / 過大 length) で out-of-bounds を
//     起こさないこと。LZ4 公式の `LZ4_decompress_safe` 相当の堅牢性を目指す。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::assetpack {

/** LZ4 解凍中に src cursor が入力範囲を超えた (truncated ブロック)。 */
inline constexpr u16 kAcpakSubLz4SrcOverflow = 1320;

/** LZ4 出力が dst_capacity を超えた。 */
inline constexpr u16 kAcpakSubLz4DstOverflow = 1321;

/** LZ4 match offset が 0 / dst 範囲外を指す (不正ブロック)。 */
inline constexpr u16 kAcpakSubLz4BadOffset   = 1322;

/** LZ4 入力が nullptr / サイズ不整合。 */
inline constexpr u16 kAcpakSubLz4BadInput    = 1323;

/**
 * LZ4 block format の自前実装を提供する static 関数群。
 *
 * @details third_party 依存ゼロ。インスタンス化はできない (ctor delete)。
 */
class FAcpakLz4 {
public:
    /** インスタンス化禁止 (全 API は static)。 */
    FAcpakLz4() = delete;

    /**
     * 入力サイズから最悪ケースの圧縮出力サイズを算出する (LZ4 公式式)。
     *
     * @details
     * = input_size + ceil(input_size / 255) + 16。圧縮できないデータ
     * (ランダム / 既圧縮) でも必ずこの範囲に収まる。
     * @param input_size 圧縮前のバイト数。
     * @return 圧縮出力に必要な最大バイト数。
     */
    static u32 MaxCompressedSize(u32 input_size) noexcept;

    /**
     * src を LZ4 block format で dst に圧縮する。
     *
     * @details
     * src_size == 0 のときは Ok(0) (空ブロック)。dst_capacity は
     * MaxCompressedSize(src_size) 以上を推奨。src_size が 13 バイト未満の場合は
     * 全 literal で素通しする。失敗時は kAcpakSubLz4BadInput (src/dst nullptr 等)
     * / kAcpakSubLz4DstOverflow (dst_capacity 不足)。
     * @param src 圧縮元バイト列。
     * @param src_size src のバイト数。
     * @param dst 圧縮先バッファ。
     * @param dst_capacity dst の容量バイト数。
     * @return 実際の圧縮後バイト数、失敗ならエラー。
     */
    static TResult<u32> Compress(const u8* src,
                                u32       src_size,
                                u8*       dst,
                                u32       dst_capacity) noexcept;

    /**
     * LZ4 block format の src を dst に解凍する。
     *
     * @details
     * src_size == 0 のときは Ok(0) (空ブロック)。dst_capacity は元データサイズ
     * 以上必要。全ループで境界検査を行い、不正入力で OOB を起こさない (LZ4 公式の
     * LZ4_decompress_safe 相当)。呼び出し側は戻り値が
     * FAcpakFileEntry::size_uncompressed と一致するか検証すること。失敗時は
     * kAcpakSubLz4SrcOverflow (入力が途中で尽きた) /
     * kAcpakSubLz4DstOverflow (出力が dst を超えた) /
     * kAcpakSubLz4BadOffset (match offset が不正)。
     * @param src 圧縮済みバイト列。
     * @param src_size src のバイト数。
     * @param dst 解凍先バッファ。
     * @param dst_capacity dst の容量バイト数。
     * @return 実際の解凍後バイト数、失敗ならエラー。
     */
    static TResult<u32> Decompress(const u8* src,
                                  u32       src_size,
                                  u8*       dst,
                                  u32       dst_capacity) noexcept;
};

} // namespace acs::assetpack
