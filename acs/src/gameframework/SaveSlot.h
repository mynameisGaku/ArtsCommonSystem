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

// =============================================================================
// FSaveSlot<T> — 単一 POD T 用のセーブスロット
// -----------------------------------------------------------------------------
// T は trivially_copyable な struct を想定する (現状 static_assert は付けて
// いない; Phase 2 で acs::IsTriviallyCopyableV を導入して有効化する予定)。
// =============================================================================
template<typename T>
class FSaveSlot {
public:
    FSaveSlot() noexcept = default;
    ~FSaveSlot() noexcept = default;

    FSaveSlot(const FSaveSlot&)            = delete;
    FSaveSlot& operator=(const FSaveSlot&) = delete;

    // Init: 1 slot に対応するファイルパスを設定する。
    //  ・file_path は呼び出し側が寿命を保証する static / member な wchar_t 列。
    //  ・このクラスはコピーを取らず、ポインタだけを保持する (STL 不使用方針)。
    //  ・file_path が nullptr の場合は「未初期化」状態のまま戻る。
    void Init(const wchar_t* file_path) noexcept {
        m_FilePath = file_path;
    }

    // Save: data を `.acssave` 形式で保存する (FSaveArchive::WriteToFile 経由)。
    //  ・version は呼び出し側が schema 進化を判定するためのタグ (default = 1)。
    //    schema を変えたら version を増やすと、旧データ読み込み時に
    //    ESaveArchiveSubCode::kSubMigrationNeeded が返って migrate しやすい。
    //  ・atomic 性は提供しない (CREATE_ALWAYS で truncate write)。電源断耐性が
    //    必要な場合は tmp file + Rename を呼出側で組むこと (Phase 2 候補)。
    TResult<void> Save(const T& data, u32 version = 1u) noexcept;

    // Load: ファイルから読み出して T を返す (FSaveArchive::ReadFromFile 経由)。
    //  ・expected_version != header.version の場合は
    //    Err(Asset, ESaveArchiveSubCode::kSubMigrationNeeded) を返す。
    //    呼び出し側は FSaveArchive::PeekVersion で旧 version を取り直して
    //    migrate するパスに分岐できる。
    TResult<T> Load(u32 expected_version = 1u) noexcept;

    // Exists: ファイルがあるかだけを判定する。
    //  未初期化 (Init 未呼出) なら常に false。
    bool Exists() const noexcept;

    // Delete: ファイルを削除する。
    //  未初期化 / ファイルが既に無い場合は成功扱い (べき等)。
    TResult<void> Delete() noexcept;

    // 内部状態用 getter (テスト・診断用)。
    const wchar_t* FilePath() const noexcept { return m_FilePath; }

private:
    // 未初期化の場合 nullptr。Init で設定される。
    const wchar_t* m_FilePath = nullptr;
};

// =============================================================================
// 実装 (template なので header 末尾に置く)
// -----------------------------------------------------------------------------
// 詳細ロジックは FSaveSlot.cpp 側の非テンプレートヘルパに移譲し、
// テンプレート部はそれを呼ぶだけにして includer 側の compile cost を抑える。
// =============================================================================

namespace detail {

// FSaveSlot.cpp 側に置く非テンプレートヘルパの宣言。
// テンプレート化された FSaveSlot<T> がここを呼ぶ形にして、
// T ごとにオブジェクトコードが膨らまないようにする。
// 中身は FSaveArchive::WriteToFile / ReadFromFile への薄いラッパ。
TResult<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                u32            version,
                                const void*    payload,
                                usize          payload_size) noexcept;

TResult<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                u32            expected_version,
                                void*          payload_out,
                                usize          payload_size) noexcept;

bool         SaveSlot_Exists(const wchar_t* file_path) noexcept;
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept;

} // namespace detail

template<typename T>
TResult<void> FSaveSlot<T>::Save(const T& data, u32 version) noexcept {
    return detail::SaveSlot_SaveBytes(m_FilePath,
                                      version,
                                      static_cast<const void*>(&data),
                                      sizeof(T));
}

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

template<typename T>
bool FSaveSlot<T>::Exists() const noexcept {
    return detail::SaveSlot_Exists(m_FilePath);
}

template<typename T>
TResult<void> FSaveSlot<T>::Delete() noexcept {
    return detail::SaveSlot_Delete(m_FilePath);
}

} // namespace acs::game
