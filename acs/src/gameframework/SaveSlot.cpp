// SPDX-License-Identifier: Apache-2.0
// FSaveSlot<T> 非テンプレート helper 実装
//
// このファイルは FSaveSlot.h で宣言した `detail::SaveSlot_*` 群の本体を実装する。
// テンプレート毎にコードが複製されないよう、bit-precise な I/O ロジックは
// すべてここに集約する (Save/Load は FSaveArchive::WriteToFile / ReadFromFile
// に委譲、Exists/Delete は acs::FileSystem に委譲)。
//
// 設計の流れ:
//   Save  → CheckInitialized → FSaveArchive::WriteToFile (header + payload + crc)
//   Load  → CheckInitialized → FSaveArchive::ReadFromFile (verify + copy)
//   Exists → FileSystem::Exists
//   Delete → CheckInitialized → FileSystem::Exists (べき等) → FileSystem::Delete
//
// 「未初期化 (Init() 未呼出)」を表す internal subcode は、FSaveArchive の
// ESaveArchiveSubCode に該当値が無いため独自に 100 番台で持つ。
#include "gameframework/SaveSlot.h"

#include "gameframework/SaveArchive.h"
#include "platform/FileSystem.h"

namespace acs::game::detail {

namespace {

/**
 * 未初期化を示す独自 subcode (ESaveArchiveSubCode と衝突しない値域)。
 *
 * @details 値は FSaveArchive の 1..7 とは別空間にして、上位層が両方の subcode を区別して扱えるようにしておく。
 */
constexpr u16 kSubSlotNotInitialized = 100u;

/**
 * 未初期化 (file_path == nullptr) を判定する。
 *
 * @details 呼出側は Init() を忘れているプログラミングミスなので Warn ログを出す。
 * @param file_path 検査するファイルパス (nullptr なら未初期化とみなす)。
 * @param operation エラーログに埋め込む呼び出し元 API 名 ("Save" / "Load" 等)。
 * @return file_path が非 nullptr なら成功、nullptr なら kSubSlotNotInitialized エラー。
 */
inline TResult<void> CheckInitialized(const wchar_t* file_path,
                                     const char*    operation) noexcept {
    if (file_path == nullptr) {
        ACS_LOG_WARN("FSaveSlot::%s called before Init()", operation);
        return ACS_ERR(IO, kSubSlotNotInitialized,
                       "FSaveSlot is not initialized");
    }
    return Ok();
}

} // namespace

/** SaveSlot_SaveBytes の実装 (CheckInitialized 後に FSaveArchive::WriteToFile へ委譲)。 */
TResult<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                u32            version,
                                const void*    payload,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Save"));
    return FSaveArchive::WriteToFile(file_path,
                                    version,
                                    payload,
                                    static_cast<u64>(payload_size));
}

/**
 * SaveSlot_LoadBytes の実装 (CheckInitialized 後に FSaveArchive::ReadFromFile へ委譲)。
 *
 * @details
 * version 不一致は kSubMigrationNeeded でそのまま伝搬する (呼び出し側が migrate ハンドラに
 * 分岐するための情報)。FSaveArchive は payload_size を ">=" でしか比較しないため、
 * header.payload_size が sizeof(T) と完全一致することをここで追加検証し、不一致なら
 * kSubBufferTooSmall を返す。
 */
TResult<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                u32            expected_version,
                                void*          payload_out,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Load"));
    u64 actual_payload_size = 0;
    const auto r = FSaveArchive::ReadFromFile(file_path,
                                       payload_out,
                                       static_cast<u64>(payload_size),
                                       expected_version,
                                       actual_payload_size);
    if (r.IsErr()) return r.Error();
    // header.payload_size が sizeof(T) と完全一致することを追加で確認する
    // (FSaveArchive 側は ">=" 比較しかしないため、シュリンク/拡張時に検出)。
    if (actual_payload_size != static_cast<u64>(payload_size)) {
        ACS_LOG_WARN("FSaveSlot::Load: payload size mismatch (file=%llu, T=%llu)",
                     static_cast<unsigned long long>(actual_payload_size),
                     static_cast<unsigned long long>(payload_size));
        return ACS_ERR(
            Asset,
            static_cast<u16>(ESaveArchiveSubCode::kSubBufferTooSmall),
            "FSaveSlot::Load: header.payload_size != sizeof(T)");
    }
    return Ok();
}

/** SaveSlot_Exists の実装 (未初期化なら警告なしで false、それ以外は FileSystem::Exists へ委譲)。 */
bool SaveSlot_Exists(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return false;
    return FileSystem::Exists(file_path);
}

/**
 * SaveSlot_Delete の実装 (べき等な削除、CheckInitialized 後に FileSystem::Delete へ委譲)。
 *
 * @details ファイルが無い場合は成功扱い (削除済み = 望ましい状態に既にある)。未初期化は IO error として返す (呼出側のプログラミングミス)。
 */
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Delete"));
    if (!FileSystem::Exists(file_path)) {
        return Ok();
    }
    return FileSystem::Delete(file_path);
}

} // namespace acs::game::detail
