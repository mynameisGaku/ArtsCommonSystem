// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — SaveSlot<T> / SaveArchive (Phase 1 スケルトン)
//
// 役割:
//   ユーザー定義の trivially-copyable な POD/struct T を 1 個のファイルに保存し、
//   後で復元する「セーブスロット」抽象。タイトル画面の continue/new game 判定、
//   オプション設定、進捗データ等、いわゆる "save data" 全般の土台となる。
//
// 使い方 (将来の典型例):
//   struct PlayerProfile {
//       acs::u32 hi_score;
//       acs::u32 play_count;
//       acs::f32 master_volume;
//   };
//   static_assert(__is_trivially_copyable(PlayerProfile));
//
//   acs::game::SaveSlot<PlayerProfile> slot;
//   slot.Init(L"user/profile.sav");
//   if (slot.Exists()) {
//       auto r = slot.Load();
//       if (r) profile = r.Value();
//   }
//   slot.Save(profile);
//
// 設計方針:
//   ・耐障害性: Save は temp ファイル (".tmp" suffix) に書き出した後、
//     atomic rename で本ファイルに置き換える。電源断やプロセスクラッシュで
//     書きかけのデータを残さない (古い slot が必ず生き残る or 新 slot に
//     完全に置き換わるかの 2 状態のみ)。
//   ・タグ付きバイナリ: 先頭 8 バイトに magic + version を埋め、
//     fread で読んだバイナリが「ACS の save data か」「同じ schema か」を
//     即判定できる。version mismatch は IO error カテゴリで返し、上位層が
//     旧データの破棄 or migration を選択する。
//   ・スキーマ進化耐性: T を直接 memcpy で書く current design は schema 固定
//     な T 専用。将来 schema を変更したい場合は SaveArchive 経由で field-by-field
//     に書き出す path に切り替えること (Phase 2)。
//   ・例外なし: 全 noexcept、エラーは Result<T, ErrorCode> で伝搬する。
//   ・STL 不使用: container 依存も無し。ファイル I/O は acs::FileSystem に委譲。
//
// Phase 1 (本実装) の範囲:
//   ・API シェイプ確定 + magic/version layout 確定 + ファイル名 (.sav / .sav.tmp)
//     決定 + Exists/Delete のみ実装。
//   ・Save / Load / SaveArchive の中身は ACS_ERR(IO,..) を返す stub。
//     実 I/O 接続は Phase 2 で行う (FileSystem::WriteAllBytes / ReadAllBytes
//     + rename + crc32)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "foundation/Log.h"

namespace acs::game {

// =============================================================================
// SaveArchive — タグ付きバイナリの低レベル format 定義
// -----------------------------------------------------------------------------
// SaveSlot<T> が書き出す .sav ファイルの bit-precise layout。
// 将来 SaveArchive を field-by-field writer/reader に拡張する余地を残すため、
// 「format 定数」を集約するクラスとして header に同梱しておく。
//
// File layout (Phase 2 実装時の target):
//   offset  size  field         説明
//   ------  ----  -----------   ------------------------------------------------
//   0x00    4     magic         'ACSV' = 0x56534341 (little-endian)。
//                               最初の 4 バイトで他フォーマットと識別する。
//   0x04    4     version       u32。schema 進化に使う。Phase 2 開始時 = 1。
//   0x08    4     payload_size  u32。data 部のバイト数 (= sizeof(T))。
//                               読み込み側の sanity check 用。
//   0x0C    4     reserved      u32 = 0。align(8) と将来拡張用 padding。
//   0x10    N     data          T を memcpy したバイト列 (N = sizeof(T))。
//   0x10+N  4     crc32_footer  u32。data 部 (offset 0x10..0x10+N) を CRC-32
//                               (poly = 0xEDB88320, init = 0xFFFFFFFF, xorout =
//                               0xFFFFFFFF) で計算した値。改竄/破損検出用。
//
// 全フィールドは little-endian、host 側もすべて little-endian 前提
// (ACS 対応プラットフォームは Win/x64 と将来の ARM64 = little-endian only)。
// =============================================================================
class SaveArchive {
public:
    // 'ACSV' を little-endian で読んだ値: 'A'=0x41, 'C'=0x43, 'S'=0x53, 'V'=0x56
    static constexpr u32 kMagic   = 0x56534341u;
    static constexpr u32 kVersion = 1u;

    // header = magic + version + payload_size + reserved
    static constexpr usize kHeaderSize = 16;

    // footer = crc32
    static constexpr usize kFooterSize = 4;

    // T を 1 個書き込むときの総ファイルサイズ
    static constexpr usize FileSizeFor(usize payload_size) noexcept {
        return kHeaderSize + payload_size + kFooterSize;
    }

    // 共通エラー subcode (Result<...,ErrorCode> の ErrorCode.subcode に入る)。
    // 上位層が switch で分岐できるよう enum 風に固定値を割り当てる。
    enum SubCode : u16 {
        kSub_NotInitialized = 1,  // Init() 未呼出で Save/Load した
        kSub_FileNotFound   = 2,  // Load 時に .sav が無い
        kSub_BadMagic       = 3,  // 先頭 4 バイトが 'ACSV' でない
        kSub_BadVersion     = 4,  // version が範囲外 (上位互換 / 旧形式)
        kSub_BadSize        = 5,  // ファイル長が想定外
        kSub_BadCrc         = 6,  // CRC mismatch (破損 or 改竄)
        kSub_IOFailure      = 7,  // 下層 FileSystem からのエラー
        kSub_NotImplemented = 99, // Phase 1 stub
    };
};

// =============================================================================
// SaveSlot<T> — 単一 POD T 用のセーブスロット
// -----------------------------------------------------------------------------
// T は trivially_copyable な struct を想定する (現状 static_assert は付けて
// いない; Phase 2 で acs::IsTriviallyCopyableV を導入して有効化する予定)。
// =============================================================================
template<typename T>
class SaveSlot {
public:
    SaveSlot() noexcept = default;
    ~SaveSlot() noexcept = default;

    SaveSlot(const SaveSlot&)            = delete;
    SaveSlot& operator=(const SaveSlot&) = delete;

    // Init: 1 slot に対応するファイルパスを設定する。
    //  ・file_path は呼び出し側が寿命を保証する static / member な wchar_t 列。
    //  ・このクラスはコピーを取らず、ポインタだけを保持する (STL 不使用方針)。
    //  ・file_path が nullptr の場合は「未初期化」状態のまま戻る。
    void Init(const wchar_t* file_path) noexcept {
        _file_path = file_path;
    }

    // Save: data を atomic に保存する。
    //  Phase 1 実装: ACS_ERR(IO, kSub_NotImplemented) を返す stub。
    //  Phase 2 実装: temp file (".tmp") 書き出し → fsync → rename。
    Result<void> Save(const T& data) noexcept;

    // Load: ファイルから読み出して T を返す。
    //  Phase 1 実装: ACS_ERR(IO, kSub_NotImplemented) を返す stub。
    Result<T> Load() noexcept;

    // Exists: ファイルがあるかだけを判定する。
    //  未初期化 (Init 未呼出) なら常に false。
    bool Exists() const noexcept;

    // Delete: ファイルを削除する。
    //  未初期化 / ファイルが既に無い場合は成功扱い (べき等)。
    Result<void> Delete() noexcept;

    // 内部状態用 getter (テスト・診断用)。
    const wchar_t* FilePath() const noexcept { return _file_path; }

private:
    // 未初期化の場合 nullptr。Init で設定される。
    const wchar_t* _file_path = nullptr;
};

// =============================================================================
// 実装 (template なので header 末尾に置く)
// -----------------------------------------------------------------------------
// 詳細ロジックは SaveSlot.cpp 側の非テンプレートヘルパに移譲し、
// テンプレート部はそれを呼ぶだけにして includer 側の compile cost を抑える。
// =============================================================================

namespace detail {

// SaveSlot.cpp 側に置く非テンプレートヘルパの宣言。
// テンプレート化された SaveSlot<T> がここを呼ぶ形にして、
// T ごとにオブジェクトコードが膨らまないようにする。
Result<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                const void*    payload,
                                usize          payload_size) noexcept;

Result<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                void*          payload_out,
                                usize          payload_size) noexcept;

bool         SaveSlot_Exists(const wchar_t* file_path) noexcept;
Result<void> SaveSlot_Delete(const wchar_t* file_path) noexcept;

} // namespace detail

template<typename T>
Result<void> SaveSlot<T>::Save(const T& data) noexcept {
    return detail::SaveSlot_SaveBytes(_file_path,
                                      static_cast<const void*>(&data),
                                      sizeof(T));
}

template<typename T>
Result<T> SaveSlot<T>::Load() noexcept {
    T out{};
    auto r = detail::SaveSlot_LoadBytes(_file_path,
                                        static_cast<void*>(&out),
                                        sizeof(T));
    if (r.IsErr()) return r.Error();
    return Result<T>(OkInit, static_cast<T&&>(out));
}

template<typename T>
bool SaveSlot<T>::Exists() const noexcept {
    return detail::SaveSlot_Exists(_file_path);
}

template<typename T>
Result<void> SaveSlot<T>::Delete() noexcept {
    return detail::SaveSlot_Delete(_file_path);
}

} // namespace acs::game
