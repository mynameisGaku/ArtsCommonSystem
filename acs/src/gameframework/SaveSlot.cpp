// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — SaveSlot<T> 非テンプレート helper 実装 (Phase 1 stub)
//
// このファイルは SaveSlot.h で宣言した `detail::SaveSlot_*` 群の本体を実装する。
// テンプレート毎にコードが複製されないよう、bit-precise な I/O ロジックは
// すべてここに集約する。
//
// Phase 1 (現在):
//   ・Exists / Delete は acs::FileSystem を呼んで本物の挙動を提供する。
//   ・Save / Load は ACS_ERR(IO, kSub_NotImplemented) を返す stub のまま。
//
// Phase 2 (次):
//   ・Save: header (magic/version/payload_size/reserved) + payload + crc32 を
//     temp file (".tmp" suffix) に WriteAllBytes → FileSystem::Rename
//     (atomic move) で本ファイルに置き換える。
//   ・Load: ReadAllBytes → magic / version / size / crc32 検証 → memcpy。
//   ・CRC-32 ルーチンは acs::Hash 系に追加 (poly 0xEDB88320)。
#include "gameframework/SaveSlot.h"

#include "platform/FileSystem.h"

namespace acs::game::detail {

namespace {

// 未初期化 (file_path == nullptr) 判定を一箇所に集約する。
// 呼出側は Init() を忘れているプログラミングミスなので、Warn ログを出す。
inline Result<void> CheckInitialized(const wchar_t* file_path,
                                     const char*    operation) noexcept {
    if (file_path == nullptr) {
        ACS_LOG_WARN("SaveSlot::%s called before Init()", operation);
        return ACS_ERR(IO, SaveArchive::kSub_NotInitialized,
                       "SaveSlot is not initialized");
    }
    return Ok();
}

} // namespace

// -----------------------------------------------------------------------------
// SaveSlot_SaveBytes — Phase 1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase 2 の擬似コード:
//   1. tmp_path = file_path + L".tmp"
//   2. buf = [magic(4)][version(4)][payload_size(4)][reserved(4)]
//             [payload(payload_size)][crc32(4)]
//   3. FileSystem::WriteAllBytes(tmp_path, buf, total)
//   4. FileSystem::Rename(tmp_path, file_path)   ※atomic
//   5. 途中失敗時は tmp_path を best-effort で削除して abort
Result<void> SaveSlot_SaveBytes(const wchar_t* file_path,
                                const void*    payload,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Save"));
    (void)payload;
    (void)payload_size;
    // Phase 1: 実 I/O は未接続。"動くが必ず失敗する" 状態にしておき、
    // 上位層 (PhotoMode の sample 等) が Result を握りつぶさない設計を強制する。
    return ACS_ERR(IO, SaveArchive::kSub_NotImplemented,
                   "SaveSlot::Save is not yet implemented (Phase 1 stub)");
}

// -----------------------------------------------------------------------------
// SaveSlot_LoadBytes — Phase 1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase 2 の擬似コード:
//   1. ReadAllBytes(file_path) → buf
//   2. buf.size() < kHeaderSize + kFooterSize なら kSub_BadSize
//   3. magic 検証 → 不一致なら kSub_BadMagic
//   4. version 検証 → kVersion と不一致なら kSub_BadVersion
//   5. payload_size 検証 → 引数 payload_size と不一致なら kSub_BadSize
//   6. crc32 計算 → footer と不一致なら kSub_BadCrc
//   7. memcpy(payload_out, &buf[kHeaderSize], payload_size)
Result<void> SaveSlot_LoadBytes(const wchar_t* file_path,
                                void*          payload_out,
                                usize          payload_size) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Load"));
    (void)payload_out;
    (void)payload_size;
    return ACS_ERR(IO, SaveArchive::kSub_NotImplemented,
                   "SaveSlot::Load is not yet implemented (Phase 1 stub)");
}

// -----------------------------------------------------------------------------
// SaveSlot_Exists — Phase 1: 本物の動作 (FileSystem::Exists 委譲)
// -----------------------------------------------------------------------------
// 未初期化なら false を返すだけで warning も出さない (Exists は判定 API なので
// 通常フローで file が無い状態を聞かれるのは正常)。
bool SaveSlot_Exists(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return false;
    return FileSystem::Exists(file_path);
}

// -----------------------------------------------------------------------------
// SaveSlot_Delete — Phase 1: 本物の動作 (べき等)
// -----------------------------------------------------------------------------
// ファイルが無い場合は成功扱いにする (削除済み = 望ましい状態に既にある)。
// 未初期化は IO error として返す (これは呼出側のミス)。
Result<void> SaveSlot_Delete(const wchar_t* file_path) noexcept {
    ACS_TRY(CheckInitialized(file_path, "Delete"));
    if (!FileSystem::Exists(file_path)) {
        return Ok();
    }
    return FileSystem::Delete(file_path);
}

} // namespace acs::game::detail
