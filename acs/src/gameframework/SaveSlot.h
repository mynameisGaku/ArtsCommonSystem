// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — FSaveSlot<T> (`.acssave` 経由の単一 POD 永続化)
//
// 役割:
//   ユーザー定義の trivially-copyable な POD/struct T を 1 個のファイルに保存し、
//   後で復元する「セーブスロット」抽象。タイトル画面の continue/new game 判定、
//   オプション設定、進捗データ等、いわゆる "save data" 全般の土台となる。
//
// 使い方 (典型例):
//   struct PlayerProfile {
//       acs::u32 hi_score;
//       acs::u32 play_count;
//       acs::f32 master_volume;
//   };
//   static_assert(__is_trivially_copyable(PlayerProfile));
//
//   acs::game::FSaveSlot<PlayerProfile> slot;
//   slot.Init(L"user/profile.acssave");
//   if (slot.Exists()) {
//       auto r = slot.Load();
//       if (r) profile = r.Value();
//   }
//   slot.Save(profile);
//
// 設計方針:
//   ・**バイナリ format**: FSaveArchive (`.acssave`、24B header + payload + crc) に
//     委譲する。詳細は gameframework/FSaveArchive.h を参照。
//   ・**スキーマ進化耐性**: T を直接 memcpy で書く current design は schema 固定
//     な T 専用。FSaveArchive の version パラメータを使って schema 変更を検知できる
//     (デフォルト version=1)。version 不一致時は FErrorCode.subcode に
//     ESaveArchiveSubCode::kSubMigrationNeeded が入る。
//   ・**例外なし**: 全 noexcept、エラーは TResult<T, FErrorCode> で伝搬する。
//   ・**STL 不使用**: container 依存も無し。ファイル I/O は FSaveArchive 経由
//     (Win32 直叩き) に委譲。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "foundation/Log.h"

#include "gameframework/SaveArchive.h"

namespace acs::game {

/**
 * 単一の trivially-copyable な POD/struct T を 1 ファイルに永続化するセーブスロット。
 *
 * @details
 * `.acssave` バイナリ形式 (FSaveArchive、24B header + payload + crc) に保存・復元を
 * 委譲する。タイトル画面の continue/new game 判定、オプション設定、進捗データ等の
 * 土台となる。T は trivially_copyable な struct を想定する (現状 static_assert は
 * 付けていない; acs::IsTriviallyCopyableV を導入して有効化できる)。例外なし
 * (全 noexcept)、STL 不使用、ファイルパスは非所有ポインタで保持する。
 * @tparam T 永続化する trivially-copyable な POD/struct 型。
 */
template<typename T>
class FSaveSlot {
public:
    /** 空状態で構築する (ファイルパス未設定)。 */
    FSaveSlot() noexcept = default;

    /** 破棄する (ファイルパスを非所有で持つだけなので no-op)。 */
    ~FSaveSlot() noexcept = default;

    /** コピー禁止。 */
    FSaveSlot(const FSaveSlot&)            = delete;

    /** コピー代入も禁止。 */
    FSaveSlot& operator=(const FSaveSlot&) = delete;

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
        m_FilePath = file_path;
    }

    /**
     * data を `.acssave` 形式で保存する (FSaveArchive::WriteToFile 経由)。
     *
     * @details
     * version は呼び出し側が schema 進化を判定するためのタグ。schema を変えたら version
     * を増やすと、旧データ読み込み時に ESaveArchiveSubCode::kSubMigrationNeeded が返って
     * migrate しやすい。atomic 性は提供しない (CREATE_ALWAYS で truncate write)。電源断
     * 耐性が必要な場合は tmp file + Rename を呼出側で組むこと。
     * @param data 保存する T の値。
     * @param version スキーマバージョンタグ (既定 1)。
     * @return 成功なら空の TResult、未初期化 / 書き込み失敗ならエラー。
     */
    TResult<void> Save(const T& data, u32 version = 1u) noexcept;

    /**
     * ファイルから読み出して T を返す (FSaveArchive::ReadFromFile 経由)。
     *
     * @details
     * expected_version != header.version の場合は
     * Err(Asset, ESaveArchiveSubCode::kSubMigrationNeeded) を返す。呼び出し側は
     * FSaveArchive::PeekVersion で旧 version を取り直して migrate するパスに分岐できる。
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

private:
    /** 扱うファイルへの wide パス (非所有、未初期化なら nullptr)。 */
    const wchar_t* m_FilePath = nullptr;
};

namespace detail {

/**
 * SaveSlot.cpp 側に置く非テンプレート保存ヘルパ (payload を `.acssave` へ書く)。
 *
 * @details
 * テンプレート化された FSaveSlot<T> がここを呼ぶ形にして、T ごとにオブジェクトコードが
 * 膨らまないようにする。中身は FSaveArchive::WriteToFile への薄いラッパ。
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
 * FSaveArchive::ReadFromFile への薄いラッパで、header.payload_size が payload_size と
 * 完全一致することも追加検証する。version 不一致は kSubMigrationNeeded で伝搬する。
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
 * ファイルが存在するかを判定する (FileSystem::Exists 委譲)。
 *
 * @param file_path 判定する wide パス。
 * @return ファイルがあれば true、未初期化 (nullptr) なら false。
 */
bool         SaveSlot_Exists(const wchar_t* file_path) noexcept;

/**
 * ファイルを削除する (べき等、FileSystem::Delete 委譲)。
 *
 * @param file_path 削除する wide パス (nullptr なら未初期化エラー)。
 * @return 成功なら空の TResult、未初期化 / 削除失敗ならエラー。
 */
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept;

} // namespace detail

/** Save の実装 (detail::SaveSlot_SaveBytes へ T のバイト列を委譲)。 */
template<typename T>
TResult<void> FSaveSlot<T>::Save(const T& data, u32 version) noexcept {
    return detail::SaveSlot_SaveBytes(m_FilePath,
                                      version,
                                      static_cast<const void*>(&data),
                                      sizeof(T));
}

/** Load の実装 (detail::SaveSlot_LoadBytes で読み出した値を T として返す)。 */
template<typename T>
TResult<T> FSaveSlot<T>::Load(u32 expected_version) noexcept {
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
bool FSaveSlot<T>::Exists() const noexcept {
    return detail::SaveSlot_Exists(m_FilePath);
}

/** Delete の実装 (detail::SaveSlot_Delete へ委譲)。 */
template<typename T>
TResult<void> FSaveSlot<T>::Delete() noexcept {
    return detail::SaveSlot_Delete(m_FilePath);
}

} // namespace acs::game
