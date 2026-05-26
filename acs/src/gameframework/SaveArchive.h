// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar J — SaveArchive (低レベル `.acssave` バイナリ I/O)
// -----------------------------------------------------------------------------
// 役割:
//   ユーザー定義 POD を 1 つのファイル (`.acssave`) に「タグ付きバイナリ」で
//   読み書きする low-level クラス。`SaveSlot<T>` の実装基盤として使う一方、
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
//   auto wr = SaveArchive::WriteToFile(L"profile.acssave", 1u, &p, sizeof(p));
//   if (wr.IsErr()) { /* 報告 */ }
//
//   // 読み込み
//   PlayerProfile p{};
//   u64 actual_size = 0;
//   auto rd = SaveArchive::ReadFromFile(L"profile.acssave", &p, sizeof(p), 1u,
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

// -----------------------------------------------------------------------------
// SaveArchive エラー subcode (FErrorCode.subcode に格納)
// -----------------------------------------------------------------------------
// 上位層が switch 分岐できるよう、固定 u32 値を割り当てる。
// 値域は 1..7 (Phase 1)。後段で増やす場合も既存値の再利用は禁止する。
enum class ESaveArchiveSubCode : u32 {
    kSubBadMagic         = 1,  // 先頭 8 バイトが "ACSSAVE\0" でない
    kSubVersionMismatch  = 2,  // header.version が想定外の値 (将来予約)
    kSubBufferTooSmall   = 3,  // ReadFromFile の out_capacity < payload_size
    kSubChecksumFail     = 4,  // CRC32 mismatch (破損 or 改竄)
    kSubFileNotFound     = 5,  // open 時に file が無い (ERROR_FILE_NOT_FOUND)
    kSubIoError          = 6,  // 下層 Win32 I/O 失敗 (read/write/seek)
    kSubMigrationNeeded  = 7,  // header.version != expected_version (migrate 要求)
};

// -----------------------------------------------------------------------------
// SaveArchive — `.acssave` バイナリ I/O 一括クラス
// -----------------------------------------------------------------------------
class SaveArchive {
public:
    // ---- フォーマット定数 ------------------------------------------------
    // magic は ASCII "ACSSAVE\0" の 8 バイト。kMagicBytes はそのバイト列を
    // 公開する (header_buf 比較用)。
    static constexpr usize kMagicSize  = 8;
    static constexpr usize kHeaderSize = 24;  // magic(8) + version(4) + size(8) + crc(4)

    // C 文字列リテラル "ACSSAVE" は実装側 (.cpp) で直接参照する。
    // header に書く 8 バイトはこの定数経由で公開しておく。
    static const u8 kMagicBytes[kMagicSize];

    // ---- 非インスタンス: コピー / ムーブ禁止 ------------------------------
    SaveArchive()                              = delete;
    ~SaveArchive()                             = delete;
    SaveArchive(const SaveArchive&)            = delete;
    SaveArchive(SaveArchive&&)                 = delete;
    SaveArchive& operator=(const SaveArchive&) = delete;
    SaveArchive& operator=(SaveArchive&&)      = delete;

    // ---- 書き込み: payload を `.acssave` 1 ファイルに保存 ------------------
    // file_path   : 出力先 (絶対 / 相対どちらでも可、wchar_t 終端)
    // version     : payload に対応する schema バージョン (呼び出し側が定義)
    // payload     : 書き出すバイト列の先頭 (nullptr 不可、payload_size == 0
    //               の場合は header のみが書かれる)
    // payload_size: payload のバイト数 (u64)
    //
    // 戻り値:
    //   Ok              — 全データの書き込みに成功
    //   Err(IO,...)     — file open / write / seek / close いずれかが失敗
    //                     (subcode は ESaveArchiveSubCode::kSubIoError)
    //
    // 失敗時のファイル状態:
    //   既存ファイルは CreateFileW(CREATE_ALWAYS) で truncate 後に書くため、
    //   途中失敗するとファイルは中途半端な状態で残る可能性がある。
    //   atomic rename が必要なら呼び出し側で tmp file → rename を組むこと
    //   (SaveSlot 上位層で実装する)。
    static TResult<void> WriteToFile(const wchar_t* file_path,
                                    u32            version,
                                    const void*    payload,
                                    u64            payload_size) noexcept;

    // ---- 読み込み: `.acssave` を検証し、payload を out_payload にコピー ----
    // file_path        : 入力ファイル (wchar_t 終端)
    // out_payload      : 読み込み先バッファ (nullptr 不可)
    // out_capacity     : out_payload のサイズ (バイト)。
    //                    実 payload_size <= out_capacity が必要、それ未満なら
    //                    kSubBufferTooSmall を返す。
    // expected_version : 呼び出し側が期待する version。一致しない場合は
    //                    kSubMigrationNeeded を返し、out_payload_size には
    //                    実 payload_size が入る (= migrate の手がかりとして
    //                    使える)。
    // out_payload_size : 出力。読めた payload のバイト数 (header 由来)。
    //                    エラー時の意味は subcode ごとに以下:
    //                      kSubBufferTooSmall  : 実 size を返す (allocate 再試行用)
    //                      kSubMigrationNeeded : 実 size を返す
    //                      その他              : 不定 (0 に初期化される)
    //
    // 戻り値:
    //   Ok(actual_version)             — 全検証 + コピー成功。返値は header.version。
    //   Err(IO/Asset, ...)             — magic / crc / io 失敗
    //   Err(Asset, kSubMigrationNeeded) — version 不一致 (out_payload にはコピー
    //                                     しない、out_payload_size のみ設定)
    static TResult<u32> ReadFromFile(const wchar_t* file_path,
                                    void*          out_payload,
                                    u64            out_capacity,
                                    u32            expected_version,
                                    u64&           out_payload_size) noexcept;

    // ---- header のみ peek ------------------------------------------------
    // ファイルを開いて先頭 kHeaderSize バイトだけ読み、検証して version を返す。
    // ファイルが存在しない / magic 不一致 / io 失敗時は対応する subcode を返す。
    //
    // 用途: タイトル画面で「セーブデータの形式が古い」表示を出すための事前判定。
    static TResult<u32> PeekVersion(const wchar_t* file_path) noexcept;

    // ---- payload_size のみ peek -----------------------------------------
    // PeekVersion と同様、header のみ読んで payload_size を返す。
    // 用途: payload を読み込む前に buffer サイズを allocate する。
    static TResult<u64> PeekPayloadSize(const wchar_t* file_path) noexcept;
};

} // namespace acs::game
