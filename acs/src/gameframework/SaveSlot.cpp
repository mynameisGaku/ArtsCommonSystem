// SPDX-License-Identifier: Apache-2.0
// TSaveSlot<T> 非テンプレート helper 実装
//
// このファイルは TSaveSlot.h で宣言した `detail::SaveSlot_*` 群の本体を実装する。
// テンプレート毎にコードが複製されないよう、bit-precise な I/O ロジックは
// すべてここに集約する (Save/Load は CSaveArchive::WriteToFile / ReadFromFile
// に委譲、Exists/Delete は acs::FileSystem に委譲)。
//
// 設計の流れ:
//   Save  → CheckInitialized → CSaveArchive::WriteToFile (header + payload + crc)
//   Load  → CheckInitialized → CSaveArchive::ReadFromFile (verify + copy)
//   Exists → FileSystem::Exists
//   Delete → CheckInitialized → FileSystem::Exists (べき等) → FileSystem::Delete
//
// 「未初期化 (Init() 未呼出)」を表す internal subcode は、CSaveArchive の
// ESaveArchiveSubCode に該当値が無いため独自に 100 番台で持つ。
#include "gameframework/SaveSlot.h"

#include "container/Array.h"
#include "foundation/Platform.h"
#include "gameframework/SaveArchive.h"
#include "memory/Memory.h"
#include "platform/FileSystem.h"

namespace acs::game::detail {

namespace {

/**
 * 未初期化を示す独自 subcode (ESaveArchiveSubCode と衝突しない値域)。
 *
 * @details 値は CSaveArchive の予約済み subcode と別空間にし、上位層が区別できるようにする。
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
        ACS_LOG_WARN("TSaveSlot::%s called before Init()", operation);
        return ACS_ERR(IO, kSubSlotNotInitialized,
                       "TSaveSlot is not initialized");
    }
    return Ok();
}

} // namespace

/** SaveSlot_SaveBytes の実装 (CheckInitialized 後に CSaveArchive::WriteToFile へ委譲)。 */
TResult<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                u32            version,
                                const void*    payload,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Save"));
    if (payload_size > static_cast<usize>(CSaveArchive::kMaxPayloadSize)) {
        return ACS_ERR(IO,
                       static_cast<u16>(ESaveArchiveSubCode::kSubPayloadTooLarge),
                       "TSaveSlot::Save: payload exceeds CSaveArchive safety limit");
    }
    return CSaveArchive::WriteToFile(file_path,
                                    version,
                                    payload,
                                    static_cast<u64>(payload_size));
}

/**
 * SaveSlot_LoadBytes の実装 (CheckInitialized 後に CSaveArchive::ReadFromFile へ委譲)。
 *
 * @details
 * version 不一致を含む CSaveArchive の診断はそのまま伝搬する。読み込み先には一時bufferを
 * 使い、CRC検証と型サイズ完全一致の後にだけ payload_out へ反映する。したがって
 * detail helper を直接使う場合も、失敗時に対象objectを部分更新しない。
 */
TResult<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                u32            expected_version,
                                void*          payload_out,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Load"));

    if (payload_out == nullptr && payload_size > 0) {
        return ACS_ERR(IO,
                       static_cast<u16>(ESaveArchiveSubCode::kSubInvalidArgument),
                       "TSaveSlot::Load: output is null but payload size is non-zero");
    }
    if (payload_size > static_cast<usize>(CSaveArchive::kMaxPayloadSize)) {
        return ACS_ERR(IO,
                       static_cast<u16>(ESaveArchiveSubCode::kSubPayloadTooLarge),
                       "TSaveSlot::Load: payload exceeds CSaveArchive safety limit");
    }

    TArray<u8> temporary;
    if (!temporary.TrySetNum(payload_size)) {
        return ACS_ERR(Memory,
                       static_cast<u16>(ESaveArchiveSubCode::kSubAllocationFailed),
                       "TSaveSlot::Load: temporary payload allocation failed");
    }

    u64 actual_payload_size = 0;
    const auto r = CSaveArchive::ReadFromFile(file_path,
                                              temporary.GetData(),
                                              static_cast<u64>(payload_size),
                                              expected_version,
                                              actual_payload_size);
    if (r.IsErr()) return r.Error();

    if (actual_payload_size != static_cast<u64>(payload_size)) {
        ACS_LOG_WARN("TSaveSlot::Load: payload size mismatch (file=%llu, T=%llu)",
                     static_cast<unsigned long long>(actual_payload_size),
                     static_cast<unsigned long long>(payload_size));
        return ACS_ERR(
            Asset,
            static_cast<u16>(ESaveArchiveSubCode::kSubBufferTooSmall),
            "TSaveSlot::Load: header.payload_size != sizeof(T)");
    }

    if (payload_size > 0) {
        MemCopy(payload_out, temporary.GetData(), payload_size);
    }
    return Ok();
}

/** SaveSlot_Exists の実装 (未初期化なら警告なしで false、それ以外は FileSystem::Exists へ委譲)。 */
bool SaveSlot_Exists(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return false;
    return CFileSystem::Exists(file_path);
}

/**
 * SaveSlot_Delete の実装 (べき等な削除、CheckInitialized 後に FileSystem::Delete へ委譲)。
 *
 * @details ファイルが無い場合は成功扱い (削除済み = 望ましい状態に既にある)。未初期化は IO error として返す (呼出側のプログラミングミス)。
 */
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Delete"));
    const auto result = CFileSystem::Delete(file_path);
    if (result.IsOk()) return Ok();

    const u32 os_error = result.Error().os_error;
    if (os_error == ERROR_FILE_NOT_FOUND || os_error == ERROR_PATH_NOT_FOUND) {
        return Ok(); // 競合削除を含め「既に無い」はべき等成功。
    }
    return result.Error();
}

} // namespace acs::game::detail
