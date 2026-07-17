// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar J — FSaveArchive (低レベル `.acssave` バイナリ I/O)
// -----------------------------------------------------------------------------
// 役割:
//   ユーザー定義 POD を 1 つのファイル (`.acssave`) に「タグ付きバイナリ」で
//   読み書きする low-level クラス。`FSaveSlot<T>` の実装基盤として使う一方、
//   テンプレートに依存したくない呼び出し側 (CRT を呼ばないツール、可変長
//   payload を扱いたい未来の SaveSlotV2 など) からも直接利用できる。
//
//   ファイルフォーマット (`.acssave`、binary only、すべて little-endian):
//
//     offset  size  field           説明
//     ------  ----  -------------   --------------------------------------------
//     0x00    8     magic           ASCII "ACSSAVE\0" (NUL 終端含む 8 バイト)。
//                                   先頭 8 バイトで他フォーマットと識別する。
//     0x08    4     version         u32。schema 進化に使う。呼び出し側が定義。
//     0x0C    8     payload_size    u64。後続 payload 部のバイト数 (uncompressed)。
//     0x14    4     crc32           u32。payload 部 (offset 0x18..) のみを
//                                   CRC-32 (poly 0xEDB88320, init/xorout
//                                   0xFFFFFFFF) で計算した値。改竄/破損検知用。
//     0x18    N     payload_bytes   sizeof(T) のバイト列 (= N = payload_size)。
//
//   header 部 (24 バイト) は固定サイズで `kHeaderSize` として公開する。
//
// 設計方針:
//   ・**例外なし**: 全 API noexcept、エラーは TResult<T, FErrorCode> で伝搬。
//   ・**STL 不使用**: <string> / <vector> / <fstream> 等は include しない。
//     <cstdint>, <cstddef> 経由 (foundation/Types.h) のみ。
//   ・**static のみ・非インスタンス**: 状態を持たない無名関数の集合体として
//     振る舞う。コピー / ムーブともに明示的に削除する。
//   ・**Win32 直叩き**: `CreateFileW / ReadFile / WriteFile / SetFilePointerEx /
//     GetFileSizeEx / CloseHandle` を .cpp 内で直接呼ぶ (FileSystem を経由
//     しない — このレイヤは整合性検証 + I/O を 1 つの atomic 単位に閉じたい
//     ため、薄い直接呼び出しが目的に合う)。
//
// 使い方:
//   // 書き込み
//   PlayerProfile p = MakeProfile();
//   auto wr = FSaveArchive::WriteToFile(L"profile.acssave", 1u, &p, sizeof(p));
//   if (wr.IsErr()) { /* 報告 */ }
//
//   // 読み込み
//   PlayerProfile p{};
//   u64 actual_size = 0;
//   auto rd = FSaveArchive::ReadFromFile(L"profile.acssave", &p, sizeof(p), 1u,
//                                       actual_size);
//   if (rd.IsErr()) {
//       if (rd.Error().subcode ==
//           static_cast<u16>(ESaveArchiveSubCode::kSubMigrationNeeded)) {
//           // 旧 version のファイル — migration 処理に分岐
//       }
//   }
//
// エラー subcode (FErrorCode.subcode に入る):
//   ESaveArchiveSubCode を参照。kSubMigrationNeeded は「読めたが version が
//   違う」を示し、呼び出し側が migrate しやすいよう non-fatal な扱いを意図する。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * FSaveArchive の各 API が返すエラー subcode (FErrorCode.subcode に格納)。
 *
 * @details
 * 上位層が switch 分岐できるよう固定 u32 値 (1..7) を割り当てる。既存値の再利用は禁止。
 * kSubMigrationNeeded は「読めたが version が違う」を示す non-fatal な扱いを意図する。
 */
enum class ESaveArchiveSubCode : u32 {
    /** 先頭 8 バイトが "ACSSAVE\0" でない。 */
    kSubBadMagic         = 1,

    /** header.version が想定外の値 (予約)。 */
    kSubVersionMismatch  = 2,

    /** ReadFromFile の out_capacity < payload_size。 */
    kSubBufferTooSmall   = 3,

    /** CRC32 mismatch (破損 or 改竄)。 */
    kSubChecksumFail     = 4,

    /** open 時に file が無い (ERROR_FILE_NOT_FOUND)。 */
    kSubFileNotFound     = 5,

    /** 下層 Win32 I/O 失敗 (read/write/seek)。 */
    kSubIoError          = 6,

    /** header.version != expected_version (migrate 要求)。 */
    kSubMigrationNeeded  = 7,
};

/**
 * POD を `.acssave` 1 ファイルにタグ付きバイナリで読み書きする低レベル I/O クラス。
 *
 * @details
 * ファイルは 24 バイト header (magic "ACSSAVE\0" + version + payload_size + payload の CRC32)
 * の後に payload バイト列が続く little-endian フォーマット。状態を持たない static 関数の
 * 集合体で、Win32 ファイル API を直接叩く。全 API noexcept でエラーは TResult で伝搬する。
 */
class FSaveArchive {
public:
    /** magic バイト列のサイズ (ASCII "ACSSAVE\0" の 8 バイト)。 */
    static constexpr usize kMagicSize  = 8;

    /** header の固定サイズ (magic 8 + version 4 + size 8 + crc 4)。 */
    static constexpr usize kHeaderSize = 24;

    /** header 先頭に書く magic バイト列 "ACSSAVE\0" (比較用に公開)。 */
    static const u8 kMagicBytes[kMagicSize];

    /** インスタンス化禁止 (state を持たない static 関数の集合)。 */
    FSaveArchive()                              = delete;

    /** デストラクタも禁止 (非インスタンス)。 */
    ~FSaveArchive()                             = delete;

    /** コピー禁止。 */
    FSaveArchive(const FSaveArchive&)            = delete;

    /** ムーブ禁止。 */
    FSaveArchive(FSaveArchive&&)                 = delete;

    /** コピー代入も禁止。 */
    FSaveArchive& operator=(const FSaveArchive&) = delete;

    /** ムーブ代入も禁止。 */
    FSaveArchive& operator=(FSaveArchive&&)      = delete;

    /**
     * payload を `.acssave` 1 ファイル (header + payload + CRC32) に保存する。
     *
     * @details
     * CreateFileW(CREATE_ALWAYS) で既存ファイルを truncate して書くため、途中失敗すると
     * ファイルは中途半端な状態で残り得る。atomic rename が必要なら呼び出し側 (FSaveSlot 上位層)
     * で tmp file → rename を組む。
     * @param file_path 出力先パス (絶対 / 相対どちらでも可、wchar_t 終端)。
     * @param version payload に対応する schema バージョン (呼び出し側が定義)。
     * @param payload 書き出すバイト列の先頭 (payload_size > 0 なら nullptr 不可)。
     * @param payload_size payload のバイト数 (0 なら header のみ書かれる)。
     * @return 成功なら空の TResult、open / write / close 失敗で kSubIoError。
     */
    static TResult<void> WriteToFile(const wchar_t* file_path,
                                    u32            version,
                                    const void*    payload,
                                    u64            payload_size) noexcept;

    /**
     * `.acssave` を検証し、payload を out_payload にコピーする。
     *
     * @details
     * magic → version → out_capacity → payload → CRC の順で fail-fast する。version 不一致時は
     * out_payload へコピーせず kSubMigrationNeeded を返し、out_payload_size に実 size を入れて
     * migrate の手がかりとする。kSubBufferTooSmall 時も out_payload_size に実 size を返す。
     * @param file_path 入力ファイルパス (wchar_t 終端)。
     * @param out_payload 読み込み先バッファ (out_capacity > 0 なら nullptr 不可)。
     * @param out_capacity out_payload のサイズ (payload_size 未満なら kSubBufferTooSmall)。
     * @param expected_version 呼び出し側が期待する version。
     * @param out_payload_size 出力。読めた payload のバイト数 (header 由来、エラー時は subcode 依存)。
     * @return 成功なら header.version、magic / crc / io 失敗や version 不一致は対応 subcode。
     */
    static TResult<u32> ReadFromFile(const wchar_t* file_path,
                                    void*          out_payload,
                                    u64            out_capacity,
                                    u32            expected_version,
                                    u64&           out_payload_size) noexcept;

    /**
     * header だけを読み、検証して version を返す。
     *
     * @details タイトル画面で「セーブデータの形式が古い」表示を出す事前判定に使う。
     * @param file_path 入力ファイルパス。
     * @return 成功なら header.version、不在 / magic 不一致 / io 失敗は対応 subcode。
     */
    static TResult<u32> PeekVersion(const wchar_t* file_path) noexcept;

    /**
     * header だけを読み、payload_size を返す。
     *
     * @details
     * payload を読み込む前に buffer サイズを allocate する用途。確保に使われる前提なので、
     * header の申告値は実ファイルサイズと照合済み (申告 > 実サイズ - header は kSubIoError)。
     * 改竄された巨大値がそのまま返って呼び出し側が巨大確保することはない。
     * @param file_path 入力ファイルパス。
     * @return 成功なら header.payload_size、不在 / magic 不一致 / サイズ不整合 / io 失敗は対応 subcode。
     */
    static TResult<u64> PeekPayloadSize(const wchar_t* file_path) noexcept;
};

} // namespace acs::game
