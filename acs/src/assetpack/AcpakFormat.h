// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — `.acpak` v1 ファイルフォーマット bit-precise 定義
// -----------------------------------------------------------------------------
// `.acpak` は ACS が「アセット流出のカジュアル防止 + 完全性検証」を行うため
// の独自アーカイブフォーマット。「raw bytes + CRC32 検証」のレイアウトに加え、
// AES-256-GCM (Windows CNG) 暗号化 + LZ4 圧縮を同フレーム内で扱える。
//
// ヘッダ + ファイルデータ + ファイルテーブル の 3 セクション構成:
//
//   offset  size  field                  説明
//   ------  ----  ---------------------  -----------------------------------
//   0x00     8    magic                  "ACPAK\0\0\0" (kAcpakMagic)
//   0x08     4    version                u32 = kAcpakVersion (= 1)
//   0x0C     4    flags                  u32 (EAcpakFlags bitfield)
//   0x10     4    file_count             u32
//   0x14     4    padding                u32 = 0 (align(8) で file_table_offset
//                                                を 8 バイト境界に乗せる)
//   0x18     8    file_table_offset      u64 (アーカイブ先頭からのオフセット)
//   0x20     4    reserved               u32 = 0
//   0x24     -    (拡張領域)
//
//   [file 0 data] ... [file N-1 data]   (各 entry の offset/size に基づき配置)
//
//   [file table]:
//     for each file:
//       path_len            : u32 (UTF-16 ワイド文字数、NUL 含まず)
//       path                : wchar_t[path_len] (UTF-16LE、NUL 含まず)
//       offset              : u64 (アーカイブ先頭からのオフセット)
//       size_uncompressed   : u64 (復号 + 解凍後のバイト数)
//       size_stored         : u64 (アーカイブ上の生バイト数 = 圧縮+暗号化済サイズ)
//       crc32               : u32 (size_uncompressed バイトに対する CRC32、
//                                  poly=0xEDB88320, init=0xFFFFFFFF, xorout=
//                                  0xFFFFFFFF)
//       cipher_nonce[12]    : (only if header.flags & AcpakFlagEncrypted)
//                              AES-256-GCM 用 per-file nonce。CSPRNG で生成。
//       cipher_tag[16]      : (only if header.flags & AcpakFlagEncrypted)
//                              AES-256-GCM 認証タグ。Encrypt 時に書き出し、
//                              Decrypt 時に検証。
//
//   下位互換: header.flags & AcpakFlagEncrypted == 0 のときは cipher_nonce /
//   cipher_tag を file table に書かない (= v1 raw レイアウトと完全一致)。
//   FAcpakReader::Open は flags を見て分岐するので、v1 で作成した .acpak は
//   v2 ライブラリでもそのまま読める。
//
// 全数値は little-endian、host 側もすべて little-endian 前提 (ACS 対応プラット
// フォームは Win/x64 と将来の ARM64 = little-endian only)。
//
// 設計上の注記:
//   ・wchar_t = UTF-16 (Windows convention) を採用。Win32 CreateFileW と直結
//     できるため、`.acpak` 内のパス仕様もそのまま wchar_t* で扱う。
//   ・magic は人間可読 8 バイト ("ACPAK\0\0\0")。version はそれと独立した u32。
//     FSaveSlot.h の "ACSV" は 4 バイト magic だが、FAssetPack はマウント検査が
//     より頻繁・高負荷なので 8 バイトの強い signature を採る。
//   ・file_table は「ファイルデータの後 (末尾)」に配置する。これにより
//     Writer はストリーミング書き込みできる (AddFile 中はテーブルをメモリ上に
//     貯め、Finalize で末尾に書き出す)。
//   ・CRC32 は size_uncompressed バイト (= 復号 + 解凍後の生データ) に対して
//     計算する。圧縮/暗号化の有無に関わらず検証仕様が変わらないよう
//     「uncompressed バイトに対する CRC」と固定する。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs::assetpack {

/**
 * `.acpak` ファイル先頭 8 バイトの magic = "ACPAK\0\0\0"。
 *
 * @details
 * Reader は CreateFileW 直後にこの 8 バイトと一致するかを最初に検査する。
 * reinterpret_cast<u64>(...) のような alias 越え比較は strict-aliasing を
 * 破るため避け、必ず memcmp / バイト列比較で検査する。
 */
inline constexpr u8 kAcpakMagic[8] = {
    'A', 'C', 'P', 'A', 'K', '\0', '\0', '\0'
};

/**
 * 現在のフォーマットバージョン (= 1)。
 *
 * @details
 * flags の bit を追加することで後方互換を保つ (reader は flags の未知 bit を
 * 見つけた場合のみエラーを返す)。互換破壊する変更を行うときにだけ 2 に上げる。
 */
inline constexpr u32 kAcpakVersion = 1u;

/**
 * header.flags の bitfield。pipeline は compress-then-encrypt 順。
 *
 * @details
 * 書き込みは compress-then-encrypt、読み出しはその逆順の
 * decrypt-then-decompress。Reader/Writer は flags を見て pipeline を組み立てる。
 */
enum EAcpakFlags : u32 {
    /** フラグなし (= 生バイト + CRC32 のみ、v1 raw レイアウト)。 */
    AcpakFlagNone        = 0u,

    /** 各 file data が AES-256-GCM で暗号化されている。 */
    AcpakFlagEncrypted   = 1u << 0,

    /** 各 file data が LZ4 で圧縮されている。 */
    AcpakFlagCompressed  = 1u << 1,
};

/**
 * アーカイブ先頭に書き込まれる固定長ヘッダ POD。
 *
 * @details
 * 全フィールド little-endian。file_table_offset の前に padding u32 を入れて
 * u64 を 8B 境界に揃える (一部 ARM プロセッサは unaligned u64 read で fault を
 * 起こすため)。reinterpret_cast による直接読込は禁止し、Reader/Writer は明示的に
 * バイト列を memcpy で読み出す (Hash.cpp と同じ流儀)。ディスク I/O は
 * kAcpakHeaderDiskSize (= 36) を用いて行う。
 */
struct FAcpakHeader {
    /** magic = kAcpakMagic ("ACPAK\0\0\0")。 */
    u8  magic[8];

    /** フォーマットバージョン = kAcpakVersion。 */
    u32 version;

    /** EAcpakFlags bitfield (encrypted / compressed)。 */
    u32 flags;

    /** file table 内の entry 数。 */
    u32 file_count;

    /** = 0。file_table_offset を 8B 境界に乗せるためのパディング。 */
    u32 padding;

    /** アーカイブ先頭から file table までの絶対オフセット。 */
    u64 file_table_offset;

    /** = 0。将来拡張用の予約フィールド。 */
    u32 reserved;
};

/**
 * ヘッダのディスク上サイズ (= 36 バイト)。
 *
 * @details
 * magic(8) + version(4) + flags(4) + file_count(4) + padding(4) = 24 で
 * file_table_offset(8) は 8B 境界に揃い、+8 = 32、+ reserved(4) = 36。
 * sizeof(FAcpakHeader) は処理系の構造体パディングで 40 になり得るため、I/O は
 * 必ずこの定数を使って 36 バイトで読み書きする。
 */
inline constexpr usize kAcpakHeaderDiskSize = 36;

/**
 * 暗号化フラグ立ち時に各 entry が file table に追加で持つバイト数 (= 28)。
 *
 * @details
 * cipher_nonce(12) + cipher_tag(16) = 28。Reader/Writer は
 * header.flags & AcpakFlagEncrypted のときだけこの 28 バイトを読み書きする。
 * v1 (flags=0) では 0 バイト = レイアウト変更なし。
 */
inline constexpr usize kAcpakCipherFieldsDiskSize = 12u + 16u;

static_assert(sizeof(u8) == 1 && sizeof(u32) == 4 && sizeof(u64) == 8,
              "Fixed-width integer types broken");
static_assert(sizeof(((FAcpakHeader*)0)->magic) == 8,
              "FAcpakHeader::magic must be 8 bytes");

/**
 * Reader が file table から構築する in-memory のファイルエントリ表現。
 *
 * @details
 * アーカイブ上のレイアウトとは形が異なる (path はディスク上では path_len +
 * wchar_t[path_len] の可変長だが、ここでは Reader 内の文字列 pool を指す
 * const wchar_t* として保持する)。path は Reader が Open した時点から Close
 * までだけ有効で、Close 後のアクセスは UB。cipher_nonce / cipher_tag は
 * AcpakFlagEncrypted が立った pak でのみディスクから読み込まれ、flags=0 (v1) の
 * pak ではゼロクリアされる。AES-256-GCM 規格上 nonce は 96bit (12B)、tag は
 * 128bit (16B) 固定。
 */
struct FAcpakFileEntry {
    /** Reader 内文字列 pool への参照 (Close まで有効)。 */
    const wchar_t* path;

    /** アーカイブ先頭からこのファイルデータまでの絶対オフセット。 */
    u64            offset;

    /** 復号 + 解凍後のバイト数。 */
    u64            size_uncompressed;

    /** アーカイブ上の生バイト数 (圧縮 + 暗号化後のサイズ)。 */
    u64            size_stored;

    /** size_uncompressed バイトに対する CRC32。 */
    u32            crc32;

    /** AES-256-GCM の per-file nonce (encrypted pak のみ有効、それ以外は 0)。 */
    u8             cipher_nonce[12];

    /** AES-256-GCM 認証タグ (encrypted pak のみ有効、それ以外は 0)。 */
    u8             cipher_tag[16];
};

// FAssetPack の subcode は ErrCategory::IO / ErrCategory::Asset 配下で
// 1300 番台を使う。FSaveSlot (1-99) / FSteamworksBridge (1001-1099) /
// FWorkshopBridge (1101-1199) / FAssetPack stub (1200 番台) とは重ならない。

/** 先頭 8 バイトが kAcpakMagic でない (= .acpak でない)。 */
inline constexpr u16 kAcpakSubBadMagic         = 1301;

/** version が kAcpakVersion でない。 */
inline constexpr u16 kAcpakSubBadVersion       = 1302;

/** ファイル長 / オフセットが想定外 or 範囲外。 */
inline constexpr u16 kAcpakSubBadSize          = 1303;

/** CRC32 mismatch (破損 or 改竄)。 */
inline constexpr u16 kAcpakSubBadCrc           = 1304;

/** header に未知 flags bit が立っている。 */
inline constexpr u16 kAcpakSubBadFlags         = 1305;

/** 未実装機能を呼び出した。 */
inline constexpr u16 kAcpakSubNotImplemented   = 1306;

/** Open() 前に API を呼び出した。 */
inline constexpr u16 kAcpakSubNotOpen          = 1307;

/** path / index が pak 内に存在しない。 */
inline constexpr u16 kAcpakSubNotFound         = 1308;

/** ReadFile の out_buffer が小さすぎる。 */
inline constexpr u16 kAcpakSubBufferTooSmall   = 1309;

/** Writer::Open の二重呼び出し。 */
inline constexpr u16 kAcpakSubAlreadyOpen      = 1310;

/** Finalize 前に Close した (内部用)。 */
inline constexpr u16 kAcpakSubNotFinalized     = 1311;

/** Win32 I/O 失敗 (詳細は os_error を参照)。 */
inline constexpr u16 kAcpakSubIOFailure        = 1312;

/** 文字列 pool / entry array の確保失敗 (OOM)。 */
inline constexpr u16 kAcpakSubOutOfMemory      = 1313;

} // namespace acs::assetpack
