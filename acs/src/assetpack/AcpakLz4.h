// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — LZ4 block format 自前実装 (third_party 依存ゼロ)
// -----------------------------------------------------------------------------
// `.acpak` v2 で各エントリを圧縮するための自己完結 LZ4 実装。LZ4 は LZ77 系の
// 「リテラル + 後方参照 (offset 1〜65535、length ≥4)」を 1 バイトトークンで
// 表現する高速圧縮形式。
//
// なぜ自前実装か:
//   ・ACS の third_party 方針は「OS 同梱以外原則 NG」(AssetPack.md §9)。
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

// ---- FErrorCode subcode (AcpakCrypto と隣接、1320 番台) -------------------
inline constexpr u16 kAcpakSubLz4SrcOverflow = 1320; // src cursor が範囲外
inline constexpr u16 kAcpakSubLz4DstOverflow = 1321; // dst capacity 超過
inline constexpr u16 kAcpakSubLz4BadOffset   = 1322; // offset 0 / dst 範囲外参照
inline constexpr u16 kAcpakSubLz4BadInput    = 1323; // 入力 nullptr / size_t 異常

// ---- AcpakLz4 (static 関数群、インスタンス化しない) ----------------------
class AcpakLz4 {
public:
    AcpakLz4() = delete;

    // 入力サイズから最悪ケースの圧縮出力サイズを算出する (LZ4 公式式)。
    // = input_size + ceil(input_size / 255) + 16
    // 圧縮できないデータ (= ランダム / 既圧縮) でも必ずこの範囲に収まる。
    static u32 MaxCompressedSize(u32 input_size) noexcept;

    // src (src_size バイト) を dst (dst_capacity バイト) に圧縮する。
    //   ・src_size == 0 のときは Ok(0) (空ブロック)。
    //   ・dst_capacity は MaxCompressedSize(src_size) 以上を推奨。
    //   ・src_size が 13 バイト未満の場合は素通し (全 literal) で書く。
    //   ・成功時の戻り値は実際の圧縮後バイト数 (dst の先頭からそのバイト数)。
    // 主なエラー:
    //   ・ACS_ERR(Asset, kAcpakSubLz4BadInput,    ...) — src/dst nullptr 等
    //   ・ACS_ERR(Asset, kAcpakSubLz4DstOverflow, ...) — dst_capacity 不足
    static TResult<u32> Compress(const u8* src,
                                u32       src_size,
                                u8*       dst,
                                u32       dst_capacity) noexcept;

    // src (src_size バイト) を dst (dst_capacity バイト) に解凍する。
    //   ・src_size == 0 のときは Ok(0) (空ブロック)。
    //   ・dst_capacity は元データサイズ以上必要。実際の解凍後サイズが
    //     返り値となる (呼び出し側は AcpakFileEntry::size_uncompressed と
    //     一致するか検証する)。
    //   ・全ループで境界検査を行い、不正入力で OOB を起こさない。
    // 主なエラー:
    //   ・ACS_ERR(Asset, kAcpakSubLz4SrcOverflow, ...) — 入力が途中で尽きた
    //   ・ACS_ERR(Asset, kAcpakSubLz4DstOverflow, ...) — 出力が dst を超えた
    //   ・ACS_ERR(Asset, kAcpakSubLz4BadOffset,   ...) — match offset が不正
    static TResult<u32> Decompress(const u8* src,
                                  u32       src_size,
                                  u8*       dst,
                                  u32       dst_capacity) noexcept;
};

} // namespace acs::assetpack
