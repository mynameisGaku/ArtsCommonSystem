// SPDX-License-Identifier: Apache-2.0
// FSaveArchive 実装 (Win32 I/O + CRC32)
//
// FSaveArchive.h で宣言した 4 メソッド (WriteToFile / ReadFromFile /
// PeekVersion / PeekPayloadSize) の本体。Win32 ファイル API を直接叩いて
// `.acssave` の bit-precise format を読み書きする。
//
// 実装上のポイント:
//   ・<windows.h> は foundation/Platform.h 経由で 1 箇所に閉じ込める
//     (Yield / CreateFile / GetMessage 等のマクロ汚染対策はここで吸収済)。
//   ・CRC32 ルーチンは assetpack/FAcpakReader.cpp / FAcpakWriter.cpp と同じ実装
//     (poly 0xEDB88320, init/xorout 0xFFFFFFFF, table 256 entry)。link 単位を
//     独立させたい (assetpack を依存に持たないテストビルドでも FSaveArchive を
//     使えるように) ため、ここでも単独に持つ。
//   ・little-endian 読み書きは MemCopy 経由で strict-aliasing 違反を避ける。
//     ACS 対応プラットフォームは Win/x64 と ARM64 (LE) のみ前提。
//   ・payload は安全上限 256 MiB。WriteFile/ReadFile は DWORD (32bit) 単位なので
//     chunk ループを使い、将来上限を拡張しても I/O 単位が溢れないようにする。
#include "gameframework/SaveArchive.h"

#include "foundation/Platform.h"   // <windows.h>
#include "foundation/Log.h"
#include "memory/Memory.h"         // MemCopy / MemCmp / MemSet

#include <cstddef>

namespace acs::game {

/** header 先頭に置く magic バイト列 "ACSSAVE\0" の実体。 */
const u8 FSaveArchive::kMagicBytes[FSaveArchive::kMagicSize] = {
    'A', 'C', 'S', 'S', 'A', 'V', 'E', '\0'
};

namespace {

/**
 * CRC32 (poly 0xEDB88320) の lookup table を返す。
 *
 * @details Meyer's singleton で thread-safe に 256 entry の table を遅延初期化する。
 * @return 256 要素の CRC32 lookup table。
 */
const u32* GetCrc32Table() noexcept
{
    struct FCrc32Table {
        FCrc32Table() noexcept
        {
            for (u32 Index = 0; Index < 256; ++Index) {
                u32 Value = Index;
                for (u32 Bit = 0; Bit < 8; ++Bit) {
                    Value = (Value & 1u) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
                }
                Values[Index] = Value;
            }
        }

        u32 Values[256] = {};
    };

    static const FCrc32Table Table;
    return Table.Values;
}

/**
 * バイト列の CRC32 を計算する (Zlib / PNG 規約)。
 *
 * @param data 入力バイト列の先頭。
 * @param size 入力バイト数。
 * @return 計算した CRC32 値 (init/xorout 0xFFFFFFFF)。
 */
u32 UpdateCrc32(u32 crc, const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32 crc = UpdateCrc32(0xFFFFFFFFu, data, size);
    return crc ^ 0xFFFFFFFFu;
}

/**
 * u32 を little-endian で書き込む (strict-aliasing 安全)。
 *
 * @details reinterpret_cast での書き込みは UB なので MemCopy 経由でバイトコピーする。ホストも LE 前提。
 * @param dst 書き込み先 (4 バイト以上)。
 * @param v 書き込む値。
 */
void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }

/**
 * u64 を little-endian で書き込む (strict-aliasing 安全)。
 *
 * @details reinterpret_cast での書き込みは UB なので MemCopy 経由でバイトコピーする。ホストも LE 前提。
 * @param dst 書き込み先 (8 バイト以上)。
 * @param v 書き込む値。
 */
void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }

/**
 * little-endian の u32 を読み込む (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (4 バイト以上)。
 * @return 読み込んだ u32 値。
 */
u32 ReadU32LE(const u8* src) noexcept {
    u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v;
}

/**
 * little-endian の u64 を読み込む (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (8 バイト以上)。
 * @return 読み込んだ u64 値。
 */
u64 ReadU64LE(const u8* src) noexcept {
    u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v;
}

/**
 * ファイルポインタを先頭からの offset へ移動する。
 *
 * @param h 対象ファイルハンドル。
 * @param offset 先頭からのバイトオフセット。
 * @param err 失敗時に GetLastError を入れる出力引数 (成功時は 0)。
 * @return 成功なら true、失敗なら false。
 */
bool SeekTo(HANDLE h, u64 offset, DWORD& err) noexcept {
    err = 0;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        err = ::GetLastError();
        return false;
    }
    return true;
}

/**
 * size バイトを完全に読み込む (DWORD 単位の chunk ループ)。
 *
 * @details >4GiB を扱うため DWORD 単位で分割読みする (assetpack と同流儀)。
 * @param h 対象ファイルハンドル。
 * @param dst 読み込み先バッファ。
 * @param size 読み込むバイト数。
 * @param err 失敗時に GetLastError (EOF 時は ERROR_HANDLE_EOF) を入れる出力引数。
 * @return 全バイト読めたら true、失敗なら false。
 */
bool ReadAll(HANDLE h, void* dst, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    u8* p = static_cast<u8*>(dst);
    u64 remaining = size;
    while (remaining > 0) {
        DWORD chunk = (remaining > 0x7FFFFFFFu)
                          ? 0x7FFFFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            return false;
        }
        p += got;
        remaining -= got;
    }
    return true;
}

/**
 * size バイトを完全に書き込む (DWORD 単位の chunk ループ)。
 *
 * @details >4GiB を扱うため DWORD 単位で分割書きする (assetpack と同流儀)。
 * @param h 対象ファイルハンドル。
 * @param src 書き込むバイト列の先頭。
 * @param size 書き込むバイト数。
 * @param err 失敗時に GetLastError (部分書き込み時は ERROR_WRITE_FAULT) を入れる出力引数。
 * @return 全バイト書けたら true、失敗なら false。
 */
bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    const u8* p = static_cast<const u8*>(src);
    u64 remaining = size;
    while (remaining > 0) {
        DWORD chunk = (remaining > 0x7FFFFFFFu)
                          ? 0x7FFFFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT;
            return false;
        }
        p += wrote;
        remaining -= wrote;
    }
    return true;
}

/**
 * ESaveArchiveSubCode を FErrorCode.subcode 用の u16 に縮約する。
 *
 * @details
 * ESaveArchiveSubCode は u32 だが FErrorCode.subcode は u16。定義済み値は u16 範囲内なので、
 * 情報落ちがないことを前提に明示的に縮約する。
 * @param sc 縮約するサブコード。
 * @return u16 に縮約したサブコード値。
 */
constexpr u16 SubU16(ESaveArchiveSubCode sc) noexcept {
    return static_cast<u16>(static_cast<u32>(sc));
}

TResult<void> ValidatePathArgument(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubInvalidArgument),
                       "FSaveArchive: path is null or empty");
    }

    usize chars = 0;
    while (chars <= FSaveArchive::kMaxPathChars && path[chars] != L'\0') {
        ++chars;
    }
    if (chars > FSaveArchive::kMaxPathChars) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubPathTooLong),
                       "FSaveArchive: path exceeds safety limit");
    }
    return Ok();
}

/** Win32 の拡張パスを含む最大文字数 (終端 NUL 込み)。 */
constexpr usize kMaxWin32PathChars = 32768;

/**
 * u32 の 10 進表現を path buffer へ追記する。
 *
 * @param value 追記する値。
 * @param out 出力 path buffer。
 * @param pos 現在位置。成功時に末尾位置へ進む。
 * @param cap out の wchar_t 要素数。
 * @return 全桁を追記できれば true。
 */
bool AppendDecimal(u32 value, wchar_t* out, usize& pos, usize cap) noexcept
{
    wchar_t reversed[10] = {};
    usize count = 0;
    do {
        reversed[count++] = static_cast<wchar_t>(L'0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    if (pos > cap || count > cap - pos) return false;
    while (count > 0) out[pos++] = reversed[--count];
    return true;
}

/**
 * `<path>.tmp.<pid>.<tid>` を同一ディレクトリの一時パスとして組み立てる。
 *
 * @details
 * 同一ディレクトリに置くことで最終 MoveFileExW が同一 volume 内の置換になる。
 * pid/tid suffix により、複数プロセス/スレッドが同じ保存先へ同時保存しても
 * 書き込み途中の一時ファイルを共有しない。
 */
bool MakeAtomicTempPath(const wchar_t* path,
                        wchar_t*       out,
                        usize          cap,
                        u32            retry_nonce = 0u) noexcept
{
    constexpr wchar_t kTmpSuffix[] = L".tmp.";
    constexpr usize kTmpSuffixChars = 5;
    constexpr usize kWorstCaseSuffixChars =
        kTmpSuffixChars + 10 + 1 + 10 + 1 + 10;

    usize path_chars = 0;
    while (path_chars < cap && path[path_chars] != L'\0') ++path_chars;
    if (cap <= kWorstCaseSuffixChars ||
        path_chars == cap ||
        path_chars > cap - 1 - kWorstCaseSuffixChars) {
        return false;
    }

    for (usize i = 0; i < path_chars; ++i) out[i] = path[i];
    usize pos = path_chars;
    for (usize i = 0; i < kTmpSuffixChars; ++i) out[pos++] = kTmpSuffix[i];
    if (!AppendDecimal(static_cast<u32>(::GetCurrentProcessId()), out, pos, cap)) return false;
    if (pos + 1 >= cap) return false;
    out[pos++] = L'.';
    if (!AppendDecimal(static_cast<u32>(::GetCurrentThreadId()), out, pos, cap)) return false;
    if (retry_nonce != 0u) {
        if (pos + 1 >= cap) return false;
        out[pos++] = L'.';
        if (!AppendDecimal(retry_nonce, out, pos, cap)) return false;
    }
    if (pos >= cap) return false;
    out[pos] = L'\0';
    return true;
}

/**
 * Process Heap 上の一時領域をスコープ終了時に解放する。
 *
 * @details
 * transactional read payload と atomic write path 用。DefaultAllocator の差し替えや
 * 例外に依存せず、Win32 I/O と同じプロセス基盤だけで寿命を閉じる。
 */
class FProcessHeapBuffer {
public:
    explicit FProcessHeapBuffer(u64 size) noexcept
    {
        if (size > 0) {
            m_Data = ::HeapAlloc(::GetProcessHeap(), 0, static_cast<SIZE_T>(size));
        }
    }

    ~FProcessHeapBuffer() noexcept
    {
        if (m_Data != nullptr) {
            ::HeapFree(::GetProcessHeap(), 0, m_Data);
        }
    }

    FProcessHeapBuffer(const FProcessHeapBuffer&) = delete;
    FProcessHeapBuffer& operator=(const FProcessHeapBuffer&) = delete;

    void* Data() noexcept { return m_Data; }
    const void* Data() const noexcept { return m_Data; }

private:
    void* m_Data = nullptr;
};

/**
 * 24 バイトの header buffer を検証してフィールドを取り出す。
 *
 * @details magic が一致した場合のみ out_* を埋める。不一致時の out_* の値は不定。
 * @param header_buf 24 バイトの header バッファ。
 * @param out_version magic 一致時に version を受け取る出力引数。
 * @param out_payload_size magic 一致時に payload サイズを受け取る出力引数。
 * @param out_crc32 magic 一致時に payload の CRC32 を受け取る出力引数。
 * @return magic が一致すれば true、不一致なら false。
 */
/**
 * FileRenameInfoEx に渡す可変長バッファの固定部分。
 *
 * SDK の FILE_RENAME_INFO は target Windows version により先頭が BOOLEAN
 * または DWORD になるため、FileRenameInfoEx の DWORD Flags layout を明示する。
 */
struct FFileRenameInfoEx {
    DWORD   Flags = 0;
    HANDLE  RootDirectory = nullptr;
    DWORD   FileNameLength = 0;
    wchar_t FileName[1] = {};
};

/**
 * 開いた旧ファイルの snapshot を維持したまま一時ファイルを置換する。
 *
 * FileRenameInfoEx の POSIX semantics は、対象が FILE_SHARE_DELETE で
 * 開かれていても replace を許可する。未対応環境では安全に失敗する。
 */
bool TryPosixAtomicReplace(const wchar_t* temp_path,
                           const wchar_t* file_path,
                           DWORD&         out_error) noexcept {
    constexpr DWORD kRenameReplaceIfExists = 0x00000001u;
    constexpr DWORD kRenamePosixSemantics  = 0x00000002u;
    constexpr auto kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

    out_error = 0;
    usize path_chars = 0;
    while (path_chars < kMaxWin32PathChars && file_path[path_chars] != L'\0') {
        ++path_chars;
    }
    if (path_chars == kMaxWin32PathChars) {
        out_error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }

    constexpr usize kPrefixBytes = offsetof(FFileRenameInfoEx, FileName);
    const usize path_bytes = path_chars * sizeof(wchar_t);
    const usize buffer_bytes = kPrefixBytes + path_bytes + sizeof(wchar_t);
    FProcessHeapBuffer storage(buffer_bytes);
    if (storage.Data() == nullptr) {
        out_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    auto* const info = static_cast<FFileRenameInfoEx*>(storage.Data());
    MemSet(info, 0, buffer_bytes);
    info->Flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->FileNameLength = static_cast<DWORD>(path_bytes);
    if (path_bytes > 0) {
        MemCopy(info->FileName, file_path, path_bytes);
    }

    HANDLE source = ::CreateFileW(
        temp_path,
        DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }

    const BOOL renamed = ::SetFileInformationByHandle(
        source,
        kFileRenameInfoEx,
        info,
        static_cast<DWORD>(buffer_bytes));
    if (!renamed) {
        out_error = ::GetLastError();
    }
    const BOOL closed = ::CloseHandle(source);
    if (renamed && !closed) {
        out_error = ::GetLastError();
        return false;
    }
    return renamed != 0;
}

bool ParseHeader(const u8* header_buf,
                 u32&      out_version,
                 u64&      out_payload_size,
                 u32&      out_crc32) noexcept {
    if (MemCmp(header_buf, FSaveArchive::kMagicBytes, FSaveArchive::kMagicSize) != 0) {
        return false;
    }
    out_version      = ReadU32LE(header_buf + 8);
    out_payload_size = ReadU64LE(header_buf + 12);
    out_crc32        = ReadU32LE(header_buf + 20);
    return true;
}

/**
 * ファイルを読み込み専用で開く (file not found は専用 subcode)。
 *
 * @details
 * CreateFileW の失敗を ERROR_FILE_NOT_FOUND とそれ以外で分け、上位のセーブ UI が
 * 「続きから表示」を出すか判断しやすくする。
 * @param file_path 開くファイルパス (nullptr はエラー)。
 * @return 成功なら開いたハンドルを持つ TResult、失敗ならエラー。
 */
TResult<HANDLE> OpenForRead(const wchar_t* file_path) noexcept {
    const auto path_result = ValidatePathArgument(file_path);
    if (path_result.IsErr()) return path_result.Error();
    // FILE_SHARE_DELETE により、読み込み中も atomic replace を妨げない。開いた handle は
    // 置換前の一貫した file object を参照し続けるため、読み込み途中で内容は切り替わらない。
    HANDLE h = ::CreateFileW(file_path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubFileNotFound),
                              "FSaveArchive: file not found", err);
        }
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive: CreateFileW (read) failed", err);
    }
    return TResult<HANDLE>(OkInit, h);
}

} // namespace

/**
 * header (24B) → payload の順で一時ファイルへ書き、保存先を atomic replace する。
 *
 * @details
 * `<path>.tmp.<pid>.<tid>` を FlushFileBuffers/CloseHandle してから、同一ディレクトリ内で
 * MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) する。最終置換まで既存ファイルは触らない。
 */
TResult<void> FSaveArchive::WriteToFile(const wchar_t* file_path,
                                       u32            version,
                                       const void*    payload,
                                       u64            payload_size) noexcept {
    const auto path_result = ValidatePathArgument(file_path);
    if (path_result.IsErr()) return path_result.Error();
    if (payload == nullptr && payload_size > 0) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubInvalidArgument),
                       "FSaveArchive::WriteToFile: payload is null but size > 0");
    }
    if (payload_size > kMaxPayloadSize) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubPayloadTooLarge),
                       "FSaveArchive::WriteToFile: payload exceeds safety limit");
    }

    FProcessHeapBuffer temp_path_storage(kMaxWin32PathChars * sizeof(wchar_t));
    if (temp_path_storage.Data() == nullptr) {
        return ACS_ERR(Memory, SubU16(ESaveArchiveSubCode::kSubAllocationFailed),
                       "FSaveArchive::WriteToFile: atomic temp path allocation failed");
    }
    auto* const temp_path = static_cast<wchar_t*>(temp_path_storage.Data());
    // 互換性のため最初の候補名は維持し、中断 process が残した古い file がある場合は
    // atomic nonce を加えて回復する。既存 object を追跡または truncate せず、通常の
    // 名前衝突だけを retry し、それ以外の create 失敗は fail-closed にする。
    static volatile LONG temp_serial = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD create_err = ERROR_FILE_EXISTS;
    constexpr u32 kMaxTempCreateAttempts = 8u;
    for (u32 attempt = 0; attempt < kMaxTempCreateAttempts; ++attempt) {
        const u32 nonce = (attempt == 0u)
            ? 0u
            : static_cast<u32>(::InterlockedIncrement(&temp_serial));
        if (!MakeAtomicTempPath(file_path, temp_path, kMaxWin32PathChars, nonce)) {
            return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubPathTooLong),
                           "FSaveArchive::WriteToFile: file path too long for atomic temp suffix");
        }

        h = ::CreateFileW(temp_path,
                          GENERIC_WRITE,
                          0,
                          nullptr,
                          CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr);
        if (h != INVALID_HANDLE_VALUE) break;

        create_err = ::GetLastError();
        if (create_err != ERROR_FILE_EXISTS &&
            create_err != ERROR_ALREADY_EXISTS) {
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::WriteToFile: CreateFileW (temp) failed",
                              create_err);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: unique temp path attempts exhausted",
                          create_err);
    }

    // header (24B) を build
    u8 header_buf[kHeaderSize] = {};
    MemCopy(header_buf, kMagicBytes, kMagicSize);
    WriteU32LE(header_buf + 8,  version);
    WriteU64LE(header_buf + 12, payload_size);
    const u32 crc = (payload_size > 0) ? ComputeCrc32(payload, payload_size)
                                       : 0xFFFFFFFFu ^ 0xFFFFFFFFu;  // 空 payload の CRC32 = 0
    WriteU32LE(header_buf + 20, crc);

    DWORD err = 0;
    if (!WriteAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        ::DeleteFileW(temp_path);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: WriteFile (header) failed", err);
    }

    // payload (size==0 でも noop で良い)
    if (payload_size > 0) {
        if (!WriteAll(h, payload, payload_size, err)) {
            ::CloseHandle(h);
            ::DeleteFileW(temp_path);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::WriteToFile: WriteFile (payload) failed",
                              err);
        }
    }

    if (!::FlushFileBuffers(h)) {
        const DWORD flush_err = ::GetLastError();
        ::CloseHandle(h);
        ::DeleteFileW(temp_path);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: FlushFileBuffers (temp) failed",
                          flush_err);
    }

    if (!::CloseHandle(h)) {
        DWORD close_err = ::GetLastError();
        ::DeleteFileW(temp_path);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: CloseHandle (temp) failed", close_err);
    }

    if (!::MoveFileExW(temp_path, file_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_err = ::GetLastError();
        DWORD posix_err = 0;
        if (!TryPosixAtomicReplace(temp_path, file_path, posix_err)) {
            ::DeleteFileW(temp_path);
            const DWORD reported_err =
                (posix_err == ERROR_INVALID_PARAMETER ||
                 posix_err == ERROR_NOT_SUPPORTED ||
                 posix_err == ERROR_CALL_NOT_IMPLEMENTED)
                    ? move_err
                    : posix_err;
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::WriteToFile: atomic replace failed",
                              reported_err);
        }
    }
    return Ok();
}

/**
 * .acssave ファイルを読み込んで payload を out_payload に展開する。
 *
 * @details
 * header (24B) を読んで magic + version + payload_size + crc を取り出し、file_size 検査 →
 * version 検査 → 容量検査 → 一時領域へ payload 読込 → CRC 検証 → 出力 commit の順で進む。
 * 最後の commit より前に失敗した場合、呼び出し側の out_payload は一切変更しない。
 */
TResult<u32> FSaveArchive::ReadFromFile(const wchar_t* file_path,
                                      void*          out_payload,
                                      u64            out_capacity,
                                      u32            expected_version,
                                      u64&           out_payload_size) noexcept {
    out_payload_size = 0;

    if (out_payload == nullptr && out_capacity > 0) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubInvalidArgument),
                       "FSaveArchive::ReadFromFile: out_payload is null but capacity > 0");
    }

    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    // file_size の sanity check
    LARGE_INTEGER fsize{};
    if (!::GetFileSizeEx(h, &fsize)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::ReadFromFile: GetFileSizeEx failed", err);
    }
    const u64 file_size = static_cast<u64>(fsize.QuadPart);
    if (file_size < kHeaderSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::ReadFromFile: file smaller than header");
    }

    // header (24B) を読む
    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::ReadFromFile: ReadFile (header) failed", err);
    }

    u32 version       = 0;
    u64 payload_size  = 0;
    u32 expected_crc  = 0;
    if (!ParseHeader(header_buf, version, payload_size, expected_crc)) {
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::ReadFromFile: magic mismatch (not an .acssave)");
    }

    if (payload_size > kMaxPayloadSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubPayloadTooLarge),
                       "FSaveArchive::ReadFromFile: payload exceeds safety limit");
    }

    // 短い payload だけでなく末尾の余分なデータも拒否し、1 header = 1 payload を保証する。
    if (payload_size != file_size - kHeaderSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubSizeMismatch),
                       "FSaveArchive::ReadFromFile: declared payload size does not match file");
    }

    out_payload_size = payload_size;

    // version 不一致は kSubMigrationNeeded で先に返す。
    // 「buffer に書き込まない」のが migration ハンドラから見ると最も扱いやすい。
    if (version != expected_version) {
        ACS_LOG_WARN("FSaveArchive::ReadFromFile: version mismatch (file=%u, expected=%u)",
                     version, expected_version);
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubMigrationNeeded),
                       "FSaveArchive::ReadFromFile: version mismatch (migration needed)");
    }

    // buffer 容量 check
    if (out_capacity < payload_size) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBufferTooSmall),
                       "FSaveArchive::ReadFromFile: out_capacity < payload_size");
    }

    // CRC 検証前に呼び出し側の object/buffer を壊さないよう、一時領域へ読み込む。
    FProcessHeapBuffer temporary_payload(payload_size);
    if (payload_size > 0 && temporary_payload.Data() == nullptr) {
        ::CloseHandle(h);
        return ACS_ERR(Memory, SubU16(ESaveArchiveSubCode::kSubAllocationFailed),
                       "FSaveArchive::ReadFromFile: temporary payload allocation failed");
    }

    if (payload_size > 0) {
        if (!ReadAll(h, temporary_payload.Data(), payload_size, err)) {
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::ReadFromFile: ReadFile (payload) failed", err);
        }
    }

    // CRC32 検証
    const u32 actual_crc = (payload_size > 0) ? ComputeCrc32(temporary_payload.Data(), payload_size)
                                              : (0xFFFFFFFFu ^ 0xFFFFFFFFu);
    if (actual_crc != expected_crc) {
        ACS_LOG_WARN("FSaveArchive::ReadFromFile: CRC mismatch (expected=0x%08x, actual=0x%08x)",
                     expected_crc, actual_crc);
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubChecksumFail),
                       "FSaveArchive::ReadFromFile: CRC32 mismatch (corrupt or tampered)");
    }

    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::ReadFromFile: CloseHandle failed", close_err);
    }

    // すべての検証と handle close が成功した後にだけ呼び出し側へ反映する。
    if (payload_size > 0) {
        MemCopy(out_payload, temporary_payload.Data(), static_cast<usize>(payload_size));
    }
    return TResult<u32>(OkInit, version);
}

/** header (24B) のみ読み、payload を読まずに version を返す。 */
TResult<u32> FSaveArchive::PeekVersion(const wchar_t* file_path) noexcept {
    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekVersion: ReadFile (header) failed", err);
    }
    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekVersion: CloseHandle failed", close_err);
    }

    u32 version = 0, dummy_crc = 0;
    u64 dummy_size = 0;
    if (!ParseHeader(header_buf, version, dummy_size, dummy_crc)) {
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::PeekVersion: magic mismatch (not an .acssave)");
    }
    return TResult<u32>(OkInit, version);
}

/**
 * header (24B) のみ読み、payload を読まずに payload_size を返す。
 *
 * @details
 * 戻り値は「この値で buffer を確保する」用途 (SaveArchive.h の宣言コメント参照) なので、
 * header の申告値をそのまま信じず、安全上限と実ファイルサイズの完全一致を検証して返す。
 * 改竄された巨大値や末尾へのデータ付加を、呼び出し側が確保する前に拒否する。
 */
TResult<u64> FSaveArchive::PeekPayloadSize(const wchar_t* file_path) noexcept {
    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    LARGE_INTEGER fsize{};
    if (!::GetFileSizeEx(h, &fsize)) {
        DWORD size_err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekPayloadSize: GetFileSizeEx failed", size_err);
    }
    const u64 file_size = static_cast<u64>(fsize.QuadPart);
    if (file_size < kHeaderSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::PeekPayloadSize: file smaller than header");
    }

    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekPayloadSize: ReadFile (header) failed", err);
    }
    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekPayloadSize: CloseHandle failed", close_err);
    }

    u32 dummy_version = 0, dummy_crc = 0;
    u64 payload_size  = 0;
    if (!ParseHeader(header_buf, dummy_version, payload_size, dummy_crc)) {
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::PeekPayloadSize: magic mismatch (not an .acssave)");
    }
    if (payload_size > kMaxPayloadSize) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubPayloadTooLarge),
                       "FSaveArchive::PeekPayloadSize: payload exceeds safety limit");
    }
    if (payload_size != file_size - kHeaderSize) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubSizeMismatch),
                       "FSaveArchive::PeekPayloadSize: declared payload size does not match file");
    }
    return TResult<u64>(OkInit, payload_size);
}

TResult<FSaveArchiveMetadata> FSaveArchive::ValidateFile(
    const wchar_t* file_path) noexcept {
    const auto open_result = OpenForRead(file_path);
    if (open_result.IsErr()) return open_result.Error();
    HANDLE file = open_result.Value();

    LARGE_INTEGER file_size_value{};
    if (!::GetFileSizeEx(file, &file_size_value)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(file);
        return ACS_ERR_OS(
            IO, SubU16(ESaveArchiveSubCode::kSubIoError),
            "FSaveArchive::ValidateFile: GetFileSizeEx failed", error);
    }
    if (file_size_value.QuadPart < static_cast<LONGLONG>(kHeaderSize)) {
        ::CloseHandle(file);
        return ACS_ERR(
            IO, SubU16(ESaveArchiveSubCode::kSubBadMagic),
            "FSaveArchive::ValidateFile: file smaller than header");
    }
    const u64 file_size = static_cast<u64>(file_size_value.QuadPart);

    u8 header[kHeaderSize] = {};
    DWORD error = 0;
    if (!ReadAll(file, header, kHeaderSize, error)) {
        ::CloseHandle(file);
        return ACS_ERR_OS(
            IO, SubU16(ESaveArchiveSubCode::kSubIoError),
            "FSaveArchive::ValidateFile: header read failed", error);
    }

    FSaveArchiveMetadata metadata{};
    if (!ParseHeader(
            header, metadata.Version, metadata.PayloadSize,
            metadata.PayloadCrc32)) {
        ::CloseHandle(file);
        return ACS_ERR(
            Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
            "FSaveArchive::ValidateFile: magic mismatch");
    }
    if (metadata.PayloadSize > kMaxPayloadSize) {
        ::CloseHandle(file);
        return ACS_ERR(
            IO, SubU16(ESaveArchiveSubCode::kSubPayloadTooLarge),
            "FSaveArchive::ValidateFile: payload exceeds safety limit");
    }
    if (metadata.PayloadSize != file_size - kHeaderSize) {
        ::CloseHandle(file);
        return ACS_ERR(
            IO, SubU16(ESaveArchiveSubCode::kSubSizeMismatch),
            "FSaveArchive::ValidateFile: declared payload size does not match file");
    }

    // 固定 stack buffer により、非信頼 payload size と heap 可用性から検証を分離する。
    constexpr usize kValidationChunkSize = 64u * 1024u;
    u8 chunk[kValidationChunkSize] = {};
    u64 remaining = metadata.PayloadSize;
    u32 crc = 0xFFFFFFFFu;
    while (remaining > 0u) {
        const u64 chunk_size =
            remaining > kValidationChunkSize ? kValidationChunkSize : remaining;
        if (!ReadAll(file, chunk, chunk_size, error)) {
            ::CloseHandle(file);
            return ACS_ERR_OS(
                IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                "FSaveArchive::ValidateFile: payload read failed", error);
        }
        crc = UpdateCrc32(crc, chunk, chunk_size);
        remaining -= chunk_size;
    }
    crc ^= 0xFFFFFFFFu;

    if (crc != metadata.PayloadCrc32) {
        ::CloseHandle(file);
        return ACS_ERR(
            Asset, SubU16(ESaveArchiveSubCode::kSubChecksumFail),
            "FSaveArchive::ValidateFile: CRC32 mismatch");
    }
    if (!::CloseHandle(file)) {
        const DWORD close_error = ::GetLastError();
        return ACS_ERR_OS(
            IO, SubU16(ESaveArchiveSubCode::kSubIoError),
            "FSaveArchive::ValidateFile: CloseHandle failed", close_error);
    }

    return TResult<FSaveArchiveMetadata>(OkInit, metadata);
}

} // namespace acs::game
