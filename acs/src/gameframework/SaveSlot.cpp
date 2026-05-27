// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar J — FSaveSlot<T> 非テンプレート helper 実装
// -----------------------------------------------------------------------------
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
// =============================================================================
#include "gameframework/SaveSlot.h"

#include "gameframework/SaveArchive.h"
#include "platform/FileSystem.h"

namespace acs::game::detail {

namespace {

// 未初期化を示す独自 subcode (ESaveArchiveSubCode と衝突しない値域)。
// 値は FSaveArchive の 1..7 とは別空間にして、上位層が両方の subcode を
// 区別して扱えるようにしておく。
constexpr u16 kSubSlotNotInitialized = 100u;

// 未初期化 (file_path == nullptr) を判定する。
// 呼出側は Init() を忘れているプログラミングミスなので Warn ログを出す。
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

// -----------------------------------------------------------------------------
// SaveSlot_SaveBytes — FSaveArchive 経由で payload を保存
// -----------------------------------------------------------------------------
// 現状は FSaveArchive::WriteToFile を直接呼ぶ薄いラッパ。
// 将来 atomic rename を入れる場合はここで:
//   1. tmp_path = file_path + L".tmp"
//   2. FSaveArchive::WriteToFile(tmp_path, ...)
//   3. FileSystem::Rename(tmp_path, file_path)
// に書き換える (Phase 2 候補)。
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

// -----------------------------------------------------------------------------
// SaveSlot_LoadBytes — FSaveArchive 経由で payload を読み込み
// -----------------------------------------------------------------------------
// payload_size と header.payload_size の一致は FSaveArchive 側が
// kSubBufferTooSmall で検出する。version 不一致は kSubMigrationNeeded で
// そのまま伝搬する (呼び出し側が migrate ハンドラに分岐するための情報)。
TResult<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                u32            expected_version,
                                void*          payload_out,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Load"));
    u64 actual_payload_size = 0;
    auto r = FSaveArchive::ReadFromFile(file_path,
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

// -----------------------------------------------------------------------------
// SaveSlot_Exists — FileSystem::Exists 委譲
// -----------------------------------------------------------------------------
// 未初期化なら false (warning 出さない、判定 API として正常な呼び方を許す)。
bool SaveSlot_Exists(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return false;
    return FileSystem::Exists(file_path);
}

// -----------------------------------------------------------------------------
// SaveSlot_Delete — FileSystem::Delete 委譲 (べき等)
// -----------------------------------------------------------------------------
// ファイルが無い場合は成功扱い (削除済み = 望ましい状態に既にある)。
// 未初期化は IO error として返す (呼出側のプログラミングミス)。
TResult<void> SaveSlot_Delete(const wchar_t* file_path) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Delete"));
    if (!FileSystem::Exists(file_path)) {
        return Ok();
    }
    return FileSystem::Delete(file_path);
}

} // namespace acs::game::detail
