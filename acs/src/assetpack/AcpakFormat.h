// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Hash.h"
#include "foundation/Types.h"

namespace acs::assetpack {

/** `.acpak` 先頭の固定 8-byte magic。 */
inline constexpr u8 kAcpakMagic[8] = {'A', 'C', 'P', 'A', 'K', '\0', '\0', '\0'};

/** 現在の互換 format version。 */
inline constexpr u32 kAcpakVersion = 1u;

/** package の圧縮・暗号化 flag。 */
enum EAcpakFlags : u32 {
    /** フラグなし (= 生バイト + CRC32 のみ、v1 raw レイアウト)。 */
    AcpakFlagNone        = 0u,

    /** 各 file data が AES-256-GCM で暗号化されている。 */
    AcpakFlagEncrypted   = 1u << 0,

    /** 各 file data が LZ4 で圧縮されている。 */
    AcpakFlagCompressed  = 1u << 1,
};

/** archive 先頭の little-endian 固定 header。 */
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

/** header の disk 上固定 byte 数。 */
inline constexpr usize kAcpakHeaderDiskSize = 36;

/** 暗号化 entry が追加で持つ nonce と tag の byte 数。 */
inline constexpr usize kAcpakCipherFieldsDiskSize = 12u + 16u;

/** 1 つの pak に格納できる entry 数の防御的上限。 */
inline constexpr u32 kAcpakMaxFileCount = 1024u * 1024u;

/** 仮想パス 1 件の UTF-16 コード単位数上限 (NUL は含まない)。 */
inline constexpr u32 kAcpakMaxPathLength = 4096u;

/** Reader が 1 pak の仮想パス保持に使える最大バイト数。 */
inline constexpr usize kAcpakMaxPathPoolBytes = 256u * 1024u * 1024u;

/** Writer の出力先 OS パスに許す UTF-16 code unit 数 (NUL を含まない)。 */
inline constexpr usize kAcpakMaxOutputPathLength = 1023u;

/** `.acpak` 仮想 path が永続化可能な正規形かを返す。 */
inline bool IsCanonicalAcpakVirtualPath(const wchar_t* Path, usize Length) noexcept
{
    if (Path == nullptr || Length == 0u || Path[0] == L'/' || Path[Length - 1u] == L'/') {
        return false;
    }

    /** 現在の segment が始まる code unit 位置。 */
    usize SegmentStart = 0u;
    for (usize Index = 0u; Index < Length; ++Index) {
        /** 今回検証する UTF-16 code unit。 */
        const wchar_t Character = Path[Index];
        if (Character == L'\0' || Character < 0x20 || Character == L'\\' || Character == L':') {
            return false;
        }
        if (Character >= 0xD800 && Character <= 0xDBFF) {
            if (Index + 1u >= Length || Path[Index + 1u] < 0xDC00 || Path[Index + 1u] > 0xDFFF) {
                return false;
            }
            ++Index;
            continue;
        }
        if (Character >= 0xDC00 && Character <= 0xDFFF) {
            return false;
        }
        if (Character != L'/') continue;

        /** 区切り直前までの segment 長。 */
        const usize SegmentLength = Index - SegmentStart;
        if (SegmentLength == 0u || (SegmentLength == 1u && Path[SegmentStart] == L'.') || (SegmentLength == 2u && Path[SegmentStart] == L'.' && Path[SegmentStart + 1u] == L'.')) {
            return false;
        }
        SegmentStart = Index + 1u;
    }

    /** 最後の segment 長。 */
    const usize LastLength = Length - SegmentStart;
    return !((LastLength == 1u && Path[SegmentStart] == L'.') || (LastLength == 2u && Path[SegmentStart] == L'.' && Path[SegmentStart + 1u] == L'.'));
}

/** 正規形の UTF-16 path を完全一致規則のまま hash する。 */
inline u64 HashCanonicalAcpakVirtualPath(const wchar_t* Path, usize Length) noexcept
{
    return HashBytes(Path, Length * sizeof(wchar_t));
}

static_assert(sizeof(u8) == 1 && sizeof(u32) == 4 && sizeof(u64) == 8, "Fixed-width integer types broken");
static_assert(sizeof(((FAcpakHeader*)0)->magic) == 8, "FAcpakHeader::magic must be 8 bytes");

/** Reader が manifest から構築して Close まで保持する entry。 */
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

/** 仮想パスが空、不正な区切り、embedded NUL、`.` / `..` segment を含む。 */
inline constexpr u16 kAcpakSubBadPath          = 1314;

/** manifest 内または Writer pending list 内で仮想パスが重複している。 */
inline constexpr u16 kAcpakSubDuplicatePath    = 1315;

/** v1 で 0 固定の padding / reserved、または table 末尾に未知データがある。 */
inline constexpr u16 kAcpakSubBadSchema        = 1316;

/** Open 中にファイル identity / size / last-write timestamp が変化した。 */
inline constexpr u16 kAcpakSubFileChanged      = 1317;

/** 完成した一時ファイルを出力先へ原子的に置換できなかった。 */
inline constexpr u16 kAcpakSubAtomicReplace    = 1318;

} // namespace acs::assetpack
