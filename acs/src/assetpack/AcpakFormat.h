// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — `.acpak` v1 ファイルフォーマット bit-precise 定義
// -----------------------------------------------------------------------------
// `.acpak` は ACS が「アセット流出のカジュアル防止 + 完全性検証」を行うため
// の独自アーカイブフォーマット。Phase 1 では「raw bytes + CRC32 検証」の最低
// 限のレイアウトを定義し、Phase 2 で AES-256-GCM (Windows CNG) + LZ4 圧縮を
// 同フレーム内で追加する。
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
//   0x24     -    (Phase 2 拡張領域)
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
//     SaveSlot.h の "ACSV" は 4 バイト magic だが、AssetPack はマウント検査が
//     より頻繁・高負荷なので 8 バイトの強い signature を採る。
//   ・file_table は「ファイルデータの後 (末尾)」に配置する。これにより
//     Writer はストリーミング書き込みできる (AddFile 中はテーブルをメモリ上に
//     貯め、Finalize で末尾に書き出す)。
//   ・CRC32 は size_uncompressed バイト (= 復号 + 解凍後の生データ) に対して
//     計算する。Phase 1 では size_stored == size_uncompressed なので
//     stored 側の CRC を計算してもよいが、Phase 2 で圧縮/暗号化が入っても
//     検証仕様が変わらないよう「uncompressed バイトに対する CRC」と固定する。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs::assetpack {

// ---- magic / version 定数 -------------------------------------------------

// `.acpak` ファイル先頭 8 バイト = "ACPAK\0\0\0" (リテラルバイト列)。
// Reader は CreateFileW 直後にこの 8 バイトと一致するかを最初に検査する。
// reinterpret_cast<u64>(...) のような alias 越え比較は strict-aliasing を
// 破るため避け、必ず memcmp / バイト列比較で検査すること。
inline constexpr u8 kAcpakMagic[8] = {
    'A', 'C', 'P', 'A', 'K', '\0', '\0', '\0'
};

// 現在のフォーマットバージョン。Phase 1 = 1。Phase 2 でも 1 を保ち、
// flags の bit を追加することで互換を保つ予定 (= reader は flags の未知 bit
// を見つけた場合のみ NotImplemented を返す)。互換破壊する変更を行うときに
// だけ version を 2 に上げる。
inline constexpr u32 kAcpakVersion = 1u;

// ---- フラグ (header.flags の bitfield) -----------------------------------
// Phase 2 で Encrypted / Compressed 共に実装済。順序は **compress-then-encrypt**
// (AssetPack.md §5)。読み出し時は **decrypt-then-decompress** の逆順。
// Reader/Writer は flags を見て pipeline を組み立てる。
enum EAcpakFlags : u32 {
    AcpakFlagNone        = 0u,
    AcpakFlagEncrypted   = 1u << 0,  // 各 file data が AES-256-GCM 暗号化 (Phase 2)
    AcpakFlagCompressed  = 1u << 1,  // 各 file data が LZ4 圧縮 (Phase 2)
};

// ---- ヘッダ (固定 40 バイト) ---------------------------------------------
// アーカイブ先頭にそのまま書き込まれる POD 構造体。
//   ・全フィールド little-endian
//   ・filed_table_offset の前に padding u32 を入れて u64 を 8B 境界に揃える
//     (一部 ARM プロセッサは unaligned u64 read で fault を起こすため)
//   ・reinterpret_cast による直接読込は禁止 — Reader/Writer は明示的に
//     バイト列を memcpy で読み出す (Hash.cpp と同じ流儀)
struct FAcpakHeader {
    u8  magic[8];             // = kAcpakMagic
    u32 version;              // = kAcpakVersion
    u32 flags;                // EAcpakFlags bitfield
    u32 file_count;           // file table 内の entry 数
    u32 padding;              // = 0 (file_table_offset を 8B 境界に乗せる)
    u64 file_table_offset;    // アーカイブ先頭からの絶対オフセット
    u32 reserved;             // = 0 (Phase 2 拡張用)
};

// バイト境界が想定通りであることを静的に保証する。
// header.magic[8] + version(4) + flags(4) + file_count(4) + padding(4)
//   = 24 → file_table_offset (8) は 24 % 8 == 0 で揃う
//   = 32 + reserved(4) = 36
//   = align(8) で末尾に 4B 詰めて 40 にはせず、reserved を 4B のままにする。
//
// ディスク上は 36 バイトを書く (sizeof(FAcpakHeader) は処理系の構造体パディング
// で 40 になり得るため、I/O は kAcpakHeaderDiskSize で明示的に行う)。
inline constexpr usize kAcpakHeaderDiskSize = 36;

// file table 内の各 entry が暗号化フラグ立ち時に追加で持つバイト数。
// = cipher_nonce(12) + cipher_tag(16) = 28
// Reader/Writer は header.flags & AcpakFlagEncrypted のときだけこの 28 バイトを
// 読み書きする。v1 (flags=0) では 0 バイト = レイアウト変更なし。
inline constexpr usize kAcpakCipherFieldsDiskSize = 12u + 16u;

static_assert(sizeof(u8) == 1 && sizeof(u32) == 4 && sizeof(u64) == 8,
              "Fixed-width integer types broken");
static_assert(sizeof(((FAcpakHeader*)0)->magic) == 8,
              "FAcpakHeader::magic must be 8 bytes");

// ---- ファイルエントリ (Reader が file table から構築する in-memory 表現) -
// アーカイブ上のレイアウトとは形が異なる (path はディスク上では path_len +
// wchar_t[path_len] の可変長で、ここでは「Reader 内の文字列 pool を指す
// const wchar_t*」として保持する)。
//
// 文字列の寿命:
//   ・Reader が Open した時点で文字列 pool を確保し、Close するまでこの
//     ポインタは有効。Close 後にアクセスするのは UB。
//
// 暗号化フィールド (Phase 2 拡張):
//   ・cipher_nonce / cipher_tag は AcpakFlagEncrypted が立った pak でのみ
//     ディスクから読み込まれる。flags=0 (v1) の pak ではゼロクリアされる。
//   ・AES-256-GCM の規格上、nonce は 96bit (12B)、tag は 128bit (16B) 固定。
struct FAcpakFileEntry {
    const wchar_t* path;              // Reader 内文字列 pool への参照
    u64            offset;            // アーカイブ先頭からの絶対オフセット
    u64            size_uncompressed; // 復号 + 解凍後のバイト数
    u64            size_stored;       // アーカイブ上の生バイト数
    u32            crc32;             // size_uncompressed バイトに対する CRC32
    u8             cipher_nonce[12];  // AES-256-GCM nonce (encrypted pak のみ)
    u8             cipher_tag[16];    // AES-256-GCM 認証タグ (encrypted pak のみ)
};

// ---- エラーコード subcode (ErrCategory::IO / ErrCategory::FAsset 配下) ----
// FSaveSlot (1-99) / SteamworksBridge (1001-1099) / WorkshopBridge (1101-1199)
// / AssetPack stub (1200 番台) と subcode 空間が重ならないよう、AssetPack 実装
// は 1300 番台を使う。
inline constexpr u16 kAcpakSubBadMagic         = 1301; // 先頭 8 バイトが ACPAK でない
inline constexpr u16 kAcpakSubBadVersion       = 1302; // version が kAcpakVersion でない
inline constexpr u16 kAcpakSubBadSize          = 1303; // ファイル長が想定外 / 範囲外
inline constexpr u16 kAcpakSubBadCrc           = 1304; // CRC mismatch (破損 or 改竄)
inline constexpr u16 kAcpakSubBadFlags         = 1305; // 未知 flags bit が立っている
inline constexpr u16 kAcpakSubNotImplemented   = 1306; // (Phase 1 残留) encrypted/compressed
                                                       // Phase 2 で実装済のため未使用。
                                                       // 将来別の未実装機能用に再利用可。
inline constexpr u16 kAcpakSubNotOpen          = 1307; // Open() 前の API 呼び出し
inline constexpr u16 kAcpakSubNotFound         = 1308; // path / index が pak 内に無い
inline constexpr u16 kAcpakSubBufferTooSmall   = 1309; // ReadFile の out_buffer が小さすぎ
inline constexpr u16 kAcpakSubAlreadyOpen      = 1310; // Writer::Open 二重呼び出し
inline constexpr u16 kAcpakSubNotFinalized     = 1311; // Finalize 前に Close (内部用)
inline constexpr u16 kAcpakSubIOFailure        = 1312; // Win32 I/O 失敗 (os_error 参照)
inline constexpr u16 kAcpakSubOutOfMemory      = 1313; // 文字列 pool / entry array 確保失敗

} // namespace acs::assetpack
