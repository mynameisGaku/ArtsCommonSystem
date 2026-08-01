// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — TSaveSlot<T> (`.acssave` 経由の単一 POD 永続化)
//
// 役割:
//   ユーザー定義の trivially-copyable な POD/struct T を 1 個のファイルに保存し、
//   後で復元する「セーブスロット」抽象。タイトル画面の continue/new game 判定、
//   オプション設定、進捗データ等、いわゆる "save data" 全般の土台となる。
//
// 使い方 (典型例):
//   struct FPlayerProfile {
//       acs::u32 hi_score;
//       acs::u32 play_count;
//       acs::f32 master_volume;
//   };
//   static_assert(__is_trivially_copyable(FPlayerProfile));
//
//   acs::game::TSaveSlot<FPlayerProfile> slot;
//   slot.Init(L"user/profile.acssave");
//   if (slot.Exists()) {
//       auto r = slot.Load();
//       if (r) profile = r.Value();
//   }
//   slot.Save(profile);
//
// 設計方針:
//   ・**バイナリ format**: CSaveArchive (`.acssave`、24B header + payload + crc) に
//     委譲する。詳細は gameframework/SaveArchive.h を参照。
//   ・**スキーマ進化耐性**: T を直接 memcpy で書く current design は schema 固定
//     な T 専用。CSaveArchive の version パラメータを使って schema 変更を検知できる
//     (デフォルト version=1)。version 不一致時は FErrorCode.subcode に
//     ESaveArchiveSubCode::kSubMigrationNeeded が入る。
//   ・**例外なし**: 全 noexcept、エラーは TResult<T, FErrorCode> で伝搬する。
//   ・**STL 不使用**: container 依存も無し。ファイル I/O は CSaveArchive 経由
//     (Win32 直叩き) に委譲。
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "foundation/Log.h"
#include "foundation/TypeTraits.h"

#include "gameframework/SaveArchive.h"

namespace acs::game {

/**
 * 単一の trivially-copyable な POD/struct T を 1 ファイルに永続化するセーブスロット。
 *
 * @details
 * `.acssave` バイナリ形式 (CSaveArchive、24B header + payload + crc) に保存・復元を
 * 委譲する。タイトル画面の continue/new game 判定、オプション設定、進捗データ等の
 * 土台となる。T は trivially_copyable かつ CSaveArchive の payload 安全上限以下で
 * あることをコンパイル時に検証する。例外なし (全 noexcept)、STL 不使用、
 * ファイルパスは非所有ポインタで保持する。
 * @tparam T 永続化する trivially-copyable な POD/struct 型。
 */
template<typename T>
class TSaveSlot {
public:
    static_assert(IsTriviallyCopyableV<T>,
                  "TSaveSlot<T> requires a trivially-copyable payload type");
    static_assert(sizeof(T) <= CSaveArchive::kMaxPayloadSize,
                  "TSaveSlot<T> payload exceeds FSaveArchive safety limit");

    /** 空状態で構築する (ファイルパス未設定)。 */
    TSaveSlot() noexcept = default;

    /** checked owned path の確保に指定 allocator を使う。 */
    explicit TSaveSlot(FAllocator& allocator) noexcept
        : m_OwnedPath(allocator) {}

    /** 破棄する。TryInit で所有したパスも自動解放する。 */
    ~TSaveSlot() noexcept = default;

    /** コピー禁止。 */
    TSaveSlot(const TSaveSlot&)            = delete;

    /** コピー代入も禁止。 */
    TSaveSlot& operator=(const TSaveSlot&) = delete;

    /**
     * 1 slot に対応するファイルパスを設定する。
     *
     * @details
     * file_path は呼び出し側が寿命を保証する static / member な wchar_t 列。本クラスは
     * コピーを取らずポインタだけを保持する (STL 不使用方針)。file_path が nullptr の
     * 場合は「未初期化」状態のまま戻る。
     * @param file_path このスロットが扱うファイルへの wide パス (非所有)。
     */
    void Init(const wchar_t* file_path) noexcept {
        if (file_path == m_OwnedPath.Data() && file_path != nullptr) {
            m_FilePath = file_path;
            return;
        }
        m_OwnedPath.ReleaseStorage();
        m_FilePath = file_path;
    }

    /**
     * 検証済みパスをコピー所有して初期化する。
     *
     * legacy `Init` と異なり、呼び出し側の文字列は一時バッファでもよい。空パス、
     * 長すぎるパス、OOM は明示エラーになり、失敗時は以前のパスと ownership を
     * 変更しない。
     */
    TResult<void> TryInit(const wchar_t* file_path) noexcept {
        if (file_path == nullptr || file_path[0] == L'\0') {
            return ACS_ERR(
                IO,
                static_cast<u16>(ESaveArchiveSubCode::kSubInvalidArgument),
                "TSaveSlot::TryInit: path is null or empty");
        }

        usize path_chars = 0;
        while (path_chars <= CSaveArchive::kMaxPathChars &&
               file_path[path_chars] != L'\0') {
            ++path_chars;
        }
        if (path_chars > CSaveArchive::kMaxPathChars) {
            return ACS_ERR(
                IO,
                static_cast<u16>(ESaveArchiveSubCode::kSubPathTooLong),
                "TSaveSlot::TryInit: path exceeds safety limit");
        }

        TArray<wchar_t> staged(*m_OwnedPath.GetAllocator());
        if (!staged.TryResize(path_chars + 1u)) {
            return ACS_ERR(
                Memory,
                static_cast<u16>(ESaveArchiveSubCode::kSubAllocationFailed),
                "TSaveSlot::TryInit: owned path allocation failed");
        }
        for (usize i = 0; i <= path_chars; ++i) {
            staged[i] = file_path[i];
        }

        m_OwnedPath = static_cast<TArray<wchar_t>&&>(staged);
        m_FilePath = m_OwnedPath.Data();
        return Ok();
    }

    /**
     * data を `.acssave` 形式で保存する (CSaveArchive::WriteToFile 経由)。
     *
     * @details
     * version は呼び出し側が schema 進化を判定するためのタグ。schema を変えたら version
     * を増やすと、旧データ読み込み時に ESaveArchiveSubCode::kSubMigrationNeeded が返って
     * migrate しやすい。書き込みは CSaveArchive の同一ディレクトリ一時ファイルと
     * atomic replace を継承し、途中失敗でも既存slotを保持する。
     * @param data 保存する T の値。
     * @param version スキーマバージョンタグ (既定 1)。
     * @return 成功なら空の TResult、未初期化 / 書き込み失敗ならエラー。
     */
    TResult<void> Save(const T& data, u32 version = 1u) noexcept;

    /**
     * ファイルから読み出して T を返す (CSaveArchive::ReadFromFile 経由)。
     *
     * @details
     * expected_version != header.version の場合は
     * Err(Asset, ESaveArchiveSubCode::kSubMigrationNeeded) を返す。型サイズ不一致、
     * CRC不一致、I/O失敗を含む全失敗で値は返さない。呼び出し側は
     * CSaveArchive::PeekVersion で旧 version を取り直して migrate するパスに分岐できる。
     * @param expected_version 期待するスキーマバージョン (既定 1)。
     * @return 成功なら読み出した T、未初期化 / 検証失敗 / version 不一致ならエラー。
     */
    TResult<T> Load(u32 expected_version = 1u) noexcept;

    /**
     * ファイルが存在するかだけを判定する。
     *
     * @return ファイルがあれば true、未初期化 (Init 未呼出) なら常に false。
     */
    bool Exists() const noexcept;

    /**
     * ファイルを削除する (べき等)。
     *
     * @details 未初期化はエラー、ファイルが既に無い場合は成功扱い。
     * @return 成功なら空の TResult、未初期化 / 削除失敗ならエラー。
     */
    TResult<void> Delete() noexcept;

    /**
     * 設定済みのファイルパスを返す (テスト・診断用)。
     *
     * @return Init で渡されたファイルパス (未初期化なら nullptr)。
     */
    const wchar_t* FilePath() const noexcept { return m_FilePath; }

    /** 現在のパスが TryInit により所有されているかを返す。 */
    bool IsPathOwned() const noexcept {
        return m_FilePath != nullptr && m_FilePath == m_OwnedPath.Data();
    }

private:
    /** TryInit が transactional に設定する optional owned path。 */
    TArray<wchar_t> m_OwnedPath {};

    /** 扱うファイルへの wide パス (非所有、未初期化なら nullptr)。 */
    const wchar_t* m_FilePath = nullptr;
};

namespace detail {

/**
 * SaveSlot.cpp 側に置く非テンプレート保存ヘルパ (payload を `.acssave` へ書く)。
 *
 * @details
 * テンプレート化された TSaveSlot<T> がここを呼ぶ形にして、T ごとにオブジェクトコードが
 * 膨らまないようにする。中身は CSaveArchive::WriteToFile への薄いラッパ。
 * @param file_path 書き込み先の wide パス (nullptr なら未初期化エラー)。
 * @param version スキーマバージョンタグ。
 * @param payload 書き込むペイロード先頭ポインタ。
 * @param payload_size payload のバイト長。
 * @return 成功なら空の TResult、未初期化 / 書き込み失敗ならエラー。
 */
TResult<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                u32            version,
                                const void*    payload,
                                usize          payload_size) noexcept;

/**
 * SaveSlot.cpp 側に置く非テンプレート読み込みヘルパ (`.acssave` から payload を読む)。
 *
 * @details
 * CSaveArchive::ReadFromFile への薄いラッパで、header.payload_size が payload_size と
 * 完全一致することも追加検証する。一時bufferへ読み込んで検証後にだけpayload_outへ
 * 反映するため、全失敗で出力は不変。version 不一致は kSubMigrationNeeded で伝搬する。
 * @param file_path 読み込み元の wide パス (nullptr なら未初期化エラー)。
 * @param expected_version 期待するスキーマバージョン。
 * @param payload_out 読み込んだ payload の書き込み先。
 * @param payload_size payload_out が受け取れるバイト長 (= sizeof(T))。
 * @return 成功なら空の TResult、未初期化 / 検証失敗 / version 不一致ならエラー。
 */
TResult<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                u32            expected_version,
                                void*          payload_out,
                                usize          payload_size) noexcept;

/**
 * ファイルが存在するかを判定する (FFileSystem::Exists 委譲)。
 *
 * @param file_path 判定する wide パス。
 * @return ファイルがあれば true、未初期化 (nullptr) なら false。
 */
bool         SaveSlot_Exists(const wchar_t* file_path) noexcept;

/**
 * ファイルを削除する (べき等、FFileSystem::Delete 委譲)。
 *
 * @param file_path 削除する wide パス (nullptr なら未初期化エラー)。
 * @return 成功なら空の TResult、未初期化 / 削除失敗ならエラー。
 */
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept;

} // namespace detail

/** Save の実装 (detail::SaveSlot_SaveBytes へ T のバイト列を委譲)。 */
template<typename T>
TResult<void> TSaveSlot<T>::Save(const T& data, u32 version) noexcept {
    return detail::SaveSlot_SaveBytes(m_FilePath,
                                      version,
                                      static_cast<const void*>(&data),
                                      sizeof(T));
}

/** Load の実装 (detail::SaveSlot_LoadBytes で読み出した値を T として返す)。 */
template<typename T>
TResult<T> TSaveSlot<T>::Load(u32 expected_version) noexcept {
    T out{};
    auto r = detail::SaveSlot_LoadBytes(m_FilePath,
                                        expected_version,
                                        static_cast<void*>(&out),
                                        sizeof(T));
    if (r.IsErr()) return r.Error();
    return TResult<T>(OkInit, static_cast<T&&>(out));
}

/** Exists の実装 (detail::SaveSlot_Exists へ委譲)。 */
template<typename T>
bool TSaveSlot<T>::Exists() const noexcept {
    return detail::SaveSlot_Exists(m_FilePath);
}

/** Delete の実装 (detail::SaveSlot_Delete へ委譲)。 */
template<typename T>
TResult<void> TSaveSlot<T>::Delete() noexcept {
    return detail::SaveSlot_Delete(m_FilePath);
}

} // namespace acs::game
