// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar J — CSaveArchive (低レベル `.acssave` バイナリ I/O)
// -----------------------------------------------------------------------------
// 役割:
//   ユーザー定義 POD を 1 つのファイル (`.acssave`) に「タグ付きバイナリ」で
//   読み書きする low-level クラス。`TSaveSlot<T>` の実装基盤として使う一方、
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
//     GetFileSizeEx / CloseHandle` を .cpp 内で直接呼ぶ (CFileSystem を経由
//     しない — このレイヤは整合性検証 + I/O を 1 つの atomic 単位に閉じたい
//     ため、薄い直接呼び出しが目的に合う)。
//
// 使い方:
//   // 書き込み
//   FPlayerProfile p = MakeProfile();
//   auto wr = CSaveArchive::WriteToFile(L"profile.acssave", 1u, &p, sizeof(p));
//   if (wr.IsErr()) { /* 報告 */ }
//
//   // 読み込み
//   FPlayerProfile p{};
//   u64 actual_size = 0;
//   auto rd = CSaveArchive::ReadFromFile(L"profile.acssave", &p, sizeof(p), 1u,
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
 * CSaveArchive の各 API が返すエラー subcode (FErrorCode.subcode に格納)。
 *
 * @details
 * 上位層が switch 分岐できるよう固定 u32 値を割り当てる。既存値の再利用は禁止。
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

    /** payload_size が安全上限を超えている。 */
    kSubPayloadTooLarge  = 8,

    /** header の申告サイズと実ファイルサイズが一致しない。 */
    kSubSizeMismatch     = 9,

    /** atomic write 用 suffix を含めるとパスが Win32 上限を超える。 */
    kSubPathTooLong      = 10,

    /** CRC 検証または atomic write path 用の一時領域を確保できなかった。 */
    kSubAllocationFailed = 11,

    /** null path/payload/output など API の事前条件違反。 */
    kSubInvalidArgument  = 12,
};

/**
 * 検証済み `.acssave` エンベロープの情報。
 *
 * `ValidateFile` は、単一のファイルオブジェクトのスナップショットから payload 全体を
 * 読み込み、CRC32 の一致を確認した後にだけこの値を返す。
 */
struct FSaveArchiveMetadata {
    u32 Version      = 0;
    u64 PayloadSize  = 0;
    u32 PayloadCrc32 = 0;
};

/**
 * POD を `.acssave` 1 ファイルにタグ付きバイナリで読み書きする低レベル I/O クラス。
 *
 * @details
 * ファイルは 24 バイト header (magic "ACSSAVE\0" + version + payload_size + payload の CRC32)
 * の後に payload バイト列が続く little-endian フォーマット。状態を持たない static 関数の
 * 集合体で、Win32 ファイル API を直接叩く。全 API noexcept でエラーは TResult で伝搬する。
 */
class CSaveArchive {
public:
    /** magic バイト列のサイズ (ASCII "ACSSAVE\0" の 8 バイト)。 */
    static constexpr usize kMagicSize  = 8;

    /** header の固定サイズ (magic 8 + version 4 + size 8 + crc 4)。 */
    static constexpr usize kHeaderSize = 24;

    /**
     * 1 ファイルで扱う payload の安全上限 (256 MiB)。
     *
     * @details
     * 読み込みは CRC 検証完了まで呼び出し側バッファを変更しないため同サイズの一時領域を使う。
     * 外部入力の申告値による過大確保を防ぐ上限でもある。
     */
    static constexpr u64 kMaxPayloadSize = 256ull * 1024ull * 1024ull;

    /** 終端 NUL を除く、対応可能な path の最大文字数。 */
    static constexpr usize kMaxPathChars = 32767;

    /** header 先頭に書く magic バイト列 "ACSSAVE\0" (比較用に公開)。 */
    static const u8 kMagicBytes[kMagicSize];

    /** インスタンス化禁止 (state を持たない static 関数の集合)。 */
    CSaveArchive()                              = delete;

    /** デストラクタも禁止 (非インスタンス)。 */
    ~CSaveArchive()                             = delete;

    /** コピー禁止。 */
    CSaveArchive(const CSaveArchive&)            = delete;

    /** ムーブ禁止。 */
    CSaveArchive(CSaveArchive&&)                 = delete;

    /** コピー代入も禁止。 */
    CSaveArchive& operator=(const CSaveArchive&) = delete;

    /** ムーブ代入も禁止。 */
    CSaveArchive& operator=(CSaveArchive&&)      = delete;

    /**
     * payload を `.acssave` 1 ファイル (header + payload + CRC32) に保存する。
     *
     * @details
     * 同一ディレクトリの一時ファイルへ全内容を書いて FlushFileBuffers/CloseHandle に成功した後、
     * MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) で置換する。書き込み・flush・置換のいずれかに
     * 失敗しても既存の保存ファイルは変更しない。一時ファイルは best-effort で削除する。
     * @param file_path 出力先パス (絶対 / 相対どちらでも可、wchar_t 終端)。
     * @param version payload に対応する schema バージョン (呼び出し側が定義)。
     * @param payload 書き出すバイト列の先頭 (payload_size > 0 なら nullptr 不可)。
     * @param payload_size payload のバイト数 (0 なら header のみ、kMaxPayloadSize 以下)。
     * @return 成功なら空の TResult。上限・パス長・open/write/flush/close/rename 失敗は対応 subcode。
     */
    static TResult<void> WriteToFile(const wchar_t* file_path,
                                    u32            version,
                                    const void*    payload,
                                    u64            payload_size) noexcept;

    /**
     * `.acssave` を検証し、payload を out_payload にコピーする。
     *
     * @details
     * magic → サイズ上限/完全一致 → version → out_capacity → payload → CRC の順で fail-fast する。
     * payload は一時領域へ読み、CRC と CloseHandle の成功後にだけ out_payload へ一括コピーするため、
     * どの失敗でも out_payload は変更しない。version 不一致時は
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
     * header の申告値は実ファイルサイズと完全一致し、kMaxPayloadSize 以下であることを検証済み。
     * 改竄された巨大値がそのまま返って呼び出し側が巨大確保することはない。
     * @param file_path 入力ファイルパス。
     * @return 成功なら header.payload_size、不在 / magic 不一致 / サイズ不整合 / io 失敗は対応 subcode。
     */
    static TResult<u64> PeekPayloadSize(const wchar_t* file_path) noexcept;

    /**
     * payload を公開・確保せず、save エンベロープ全体を検証する。
     *
     * 1 つの read handle を保持したまま magic、payload 上限、厳密なファイルサイズ、
     * CRC32 を検証する。この handle は同一ファイルへの上書きを拒否しつつ atomic replace は
     * 許可するため、安定した単一のファイルオブジェクトのスナップショットを参照できる。
     *
     * @param file_path NUL 終端 path。空または上限超過の path は拒否する。
     * @return 検証済み metadata。失敗時は `ReadFromFile` と共通の詳細 subcode。
     */
    static TResult<FSaveArchiveMetadata> ValidateFile(
        const wchar_t* file_path) noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSaveArchive = CSaveArchive;

} // namespace acs::game
