// SPDX-License-Identifier: Apache-2.0
// ファイル I/O 実装（Win32 Create/Read/Write FileW）
#include "platform/FileSystem.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"

namespace acs {

namespace {

/** ReadFile 呼び出し回数。 */
TAtomic<u64> g_ReadSyscalls{0};

/** WriteFile 呼び出し回数。 */
TAtomic<u64> g_WriteSyscalls{0};

/** ReadAllText が中間配列から再コピーした byte 数。単一確保経路では常に 0。 */
TAtomic<u64> g_TextIntermediateCopyBytes{0};

/** 原子的書き込み用の一時ファイル名を一意化する採番値。 */
TAtomic<u32> g_AtomicWriteSequence{1};

/**
 * ASCIIのドライブ文字ならtrueを返す。
 *
 * @param value 判定する文字。
 * @return A-Zまたはa-zならtrue。
 */
constexpr bool IsDriveLetter(wchar_t value) noexcept
{
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

/**
 * 指定位置以降がpath separatorだけならtrueを返す。
 *
 * @param path 判定するパス。
 * @param begin 判定開始位置。
 * @param length パスの文字数。
 * @return separator以外を含まなければtrue。
 */
constexpr bool HasOnlyPathSeparators(const wchar_t* path, usize begin, usize length) noexcept
{
    /** 判定する文字位置。 */
    for (usize i = begin; i < length; ++i) {
        if (!CFileSystem::IsPathSeparator(path[i])) return false;
    }
    return true;
}

/**
 * 指定prefix直後がドライブrootならtrueを返す。
 *
 * @param path 判定するパス。
 * @param length パスの文字数。
 * @param prefix_length ドライブ文字より前の文字数。
 * @return drive-letter、colon、separatorだけで終わる場合はtrue。
 */
constexpr bool IsDriveRootPath(const wchar_t* path, usize length, usize prefix_length) noexcept
{
    if (length < prefix_length + 3u) return false;
    if (!IsDriveLetter(path[prefix_length]) || path[prefix_length + 1u] != L':' || !CFileSystem::IsPathSeparator(path[prefix_length + 2u])) return false;
    return HasOnlyPathSeparators(path, prefix_length + 3u, length);
}

/**
 * 通常UNCのserver/share rootならtrueを返す。
 *
 * @param path 判定するパス。
 * @param length パスの文字数。
 * @return serverとshareが各1要素あり、その後がseparatorだけならtrue。
 */
constexpr bool IsUncShareRootPath(const wchar_t* path, usize length) noexcept
{
    if (length < 5u || !CFileSystem::IsPathSeparator(path[0]) || !CFileSystem::IsPathSeparator(path[1])) return false;

    /** server要素の終端位置。 */
    usize server_end = 2u;
    while (server_end < length && !CFileSystem::IsPathSeparator(path[server_end])) ++server_end;
    if (server_end == 2u || server_end == length) return false;
    if (server_end == 3u && (path[2] == L'?' || path[2] == L'.')) return false;

    /** share要素の開始位置。 */
    const usize share_begin = server_end + 1u;
    if (share_begin >= length || CFileSystem::IsPathSeparator(path[share_begin])) return false;
    /** share要素の終端位置。 */
    usize share_end = share_begin;
    while (share_end < length && !CFileSystem::IsPathSeparator(path[share_end])) ++share_end;
    return HasOnlyPathSeparators(path, share_end, length);
}

/**
 * CreateDirectoryWを呼ばず既存確認だけで受理できるroot構文ならtrueを返す。
 *
 * @param path 判定するパス。
 * @param length パスの文字数。
 * @return drive root、UNC share root、拡張drive rootのいずれかならtrue。
 */
constexpr bool IsDirectoryRootPath(const wchar_t* path, usize length) noexcept
{
    if (IsDriveRootPath(path, length, 0u) || IsUncShareRootPath(path, length)) return true;
    return length >= 4u && CFileSystem::IsPathSeparator(path[0]) && CFileSystem::IsPathSeparator(path[1]) && path[2] == L'?' && CFileSystem::IsPathSeparator(path[3]) && IsDriveRootPath(path, length, 4u);
}

/**
 * 文字列literalをroot構文としてcompile時に判定する。
 *
 * @tparam PathSize 終端文字を含む配列要素数。
 * @param path 判定する文字列literal。
 * @return root構文ならtrue。
 */
template<usize PathSize>
constexpr bool IsDirectoryRootLiteral(const wchar_t (&path)[PathSize]) noexcept
{
    static_assert(PathSize > 0u);
    return IsDirectoryRootPath(path, PathSize - 1u);
}

// root shortcutで受理する構文と、紛らわしい不正構文をcompile時に固定する。
static_assert(IsDirectoryRootLiteral(L"C:\\"));
static_assert(IsDirectoryRootLiteral(L"c:/"));
static_assert(IsDirectoryRootLiteral(L"\\\\server\\share"));
static_assert(IsDirectoryRootLiteral(L"\\\\server\\share\\"));
static_assert(IsDirectoryRootLiteral(L"\\\\?\\C:\\"));
static_assert(!IsDirectoryRootLiteral(L"C:\\file"));
static_assert(!IsDirectoryRootLiteral(L"\\\\server"));
static_assert(!IsDirectoryRootLiteral(L"\\\\server\\"));
static_assert(!IsDirectoryRootLiteral(L"\\\\server\\\\share"));
static_assert(!IsDirectoryRootLiteral(L"\\\\.\\C:\\"));
static_assert(!IsDirectoryRootLiteral(L"\\\\?\\C:\\file"));

/**
 * 読み取り用にファイルを開いてハンドルを返す。
 *
 * @details GENERIC_READ・FILE_SHARE_READ・OPEN_EXISTING で開くため未存在は失敗する。
 * @param path 開くファイルのパス。
 * @return ファイルハンドル (失敗時は INVALID_HANDLE_VALUE)。
 */
HANDLE OpenForRead(const wchar_t* path) noexcept
{
    return ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

/**
 * 書き込み用にファイルを開いてハンドルを返す (既存は上書き)。
 *
 * @details GENERIC_WRITE・共有なし・CREATE_ALWAYS で開くため既存内容は破棄される。
 * @param path 開くファイルのパス。
 * @return ファイルハンドル (失敗時は INVALID_HANDLE_VALUE)。
 */
HANDLE OpenForWrite(const wchar_t* path) noexcept
{
    return ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

/**
 * ファイルを 1 回の確保と最大 1 回の ReadFile で読み込む。
 *
 * @tparam Element byte または char。
 * @tparam NullTerminate true なら読み取り byte の後ろへ NUL を 1 個付ける。
 */
template<typename Element, bool NullTerminate>
TResult<TArray<Element>> ReadWholeFile(const wchar_t* path) noexcept
{
    static_assert(sizeof(Element) == 1, "全体読み込みの出力要素は 1 byte である必要があります");
    if (!path || path[0] == L'\0') {
        return ACS_ERR(IO, 99, "ReadWholeFile: path is null or empty");
    }

    /** 読み取り対象のファイルハンドル。 */
    const HANDLE h = OpenForRead(path);
    if (h == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 100, "CreateFileW (read) failed", ::GetLastError());

    /** 読み取り対象のファイルサイズ。 */
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        /** サイズ取得時の OS エラー。 */
        const DWORD error = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, 101, "GetFileSizeEx failed", error);
    }
    if (size.QuadPart < 0 || size.QuadPart > MAXDWORD) {
        ::CloseHandle(h);
        return ACS_ERR(IO, 102, "File too large (>4GB)");
    }

    /** ファイル本体の byte 数。 */
    const usize content_size = static_cast<usize>(size.QuadPart);
    /** 文字列終端を含む確保要素数。 */
    const usize allocation_size = content_size + (NullTerminate ? 1u : 0u);
    if (allocation_size < content_size) {
        ::CloseHandle(h);
        return ACS_ERR(IO, 104, "ReadWholeFile: allocation size overflow");
    }

    /** ファイル内容を直接受け取る出力配列。 */
    TArray<Element> buffer;
    if (!buffer.TryResize(allocation_size)) {
        ::CloseHandle(h);
        return ACS_ERR(IO, 105, "ReadWholeFile: allocation failed");
    }

    /** 実際に読み取った byte 数。 */
    DWORD bytes_read = 0;
    /** ReadFile の成否。 */
    BOOL read_ok = TRUE;
    /** ReadFile 失敗時の OS エラー。 */
    DWORD read_error = ERROR_SUCCESS;
    if (content_size != 0) {
        g_ReadSyscalls.FetchAdd(1);
        read_ok = ::ReadFile(h, buffer.Data(), static_cast<DWORD>(content_size), &bytes_read, nullptr);
        if (!read_ok) read_error = ::GetLastError();
    }
    ::CloseHandle(h);
    if (!read_ok || bytes_read != content_size)
        return ACS_ERR_OS(IO, 103, "ReadFile failed", read_error);

    if constexpr (NullTerminate) buffer[content_size] = Element{};
    return TResult<TArray<Element>>(OkInit, Move(buffer));
}

} // namespace

// ファイル全体をバイト列として読み込む
TResult<TArray<byte>> CFileSystem::ReadAllBytes(const wchar_t* path) noexcept {
    return ReadWholeFile<byte, false>(path);
}

// ファイル全体を直接 char 配列へ読み込み、末尾に NUL を付与する。
TResult<TArray<char>> CFileSystem::ReadAllText(const wchar_t* path) noexcept {
    return ReadWholeFile<char, true>(path);
}

// バイト列を書き出す（上書き）
TResult<void> CFileSystem::WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 109, "WriteAllBytes: path is null or empty");
    if (size > MAXDWORD)
        return ACS_ERR(IO, 112, "WriteAllBytes: payload is larger than 4GB");
    if (!data && size != 0)
        return ACS_ERR(IO, 113, "WriteAllBytes: data is null");

    /** 書き込み対象のファイルハンドル。 */
    const HANDLE h = OpenForWrite(path);
    if (h == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 110, "CreateFileW (write) failed", ::GetLastError());
    /** 実際に書き込んだ byte 数。 */
    DWORD wrote = 0;
    /** WriteFile の成否。 */
    BOOL ok = TRUE;
    if (size != 0) {
        g_WriteSyscalls.FetchAdd(1);
        ok = ::WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);
    }
    /** WriteFile 失敗時の OS エラー。 */
    const DWORD err = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok || wrote != size)
        return ACS_ERR_OS(IO, 111, "WriteFile failed", err);
    return Ok();
}

// 同一ディレクトリの一時ファイルを完全に書いてから原子的に公開する。
TResult<void> CFileSystem::WriteAllBytesAtomic(const wchar_t* path, const byte* data, usize size) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 114, "WriteAllBytesAtomic: path is null or empty");
    if (size > MAXDWORD)
        return ACS_ERR(IO, 115, "WriteAllBytesAtomic: payload is larger than 4GB");
    if (!data && size != 0)
        return ACS_ERR(IO, 116, "WriteAllBytesAtomic: data is null");

    /** 公開先の現在属性。 */
    const DWORD destination_attributes = ::GetFileAttributesW(path);
    if (destination_attributes != INVALID_FILE_ATTRIBUTES && (destination_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        // reparse point 自体を rename で置換せず、従来どおりリンク先へ書く。
        return WriteAllBytes(path, data, size);
    }

    /** 同一ディレクトリへ作る一時ファイルパス。 */
    wchar_t temporary_path[1024]{};
    /** 一時ファイルパスへ書き込んだ文字数。 */
    usize path_length = 0;
    while (path[path_length] != L'\0' && path_length < 960) {
        temporary_path[path_length] = path[path_length];
        ++path_length;
    }
    if (path[path_length] != L'\0')
        return ACS_ERR(IO, 117, "WriteAllBytesAtomic: path is too long");

    /** 一時ファイル名へ付ける識別接尾辞。 */
    constexpr wchar_t kSuffix[] = L".acs-tmp-";
    /** 接尾辞から終端を除いてコピーする位置。 */
    for (usize i = 0; i + 1 < sizeof(kSuffix) / sizeof(kSuffix[0]); ++i)
        temporary_path[path_length++] = kSuffix[i];

    /** 32 bit 値を固定八桁の十六進数として末尾へ追加する。 */
    const auto append_hex = [&](u32 value, usize& cursor) noexcept {
        /** 十六進数の変換表。 */
        constexpr wchar_t kHex[] = L"0123456789abcdef";
        /** 現在出力する四 bit の位置。 */
        for (i32 shift = 28; shift >= 0; shift -= 4)
            temporary_path[cursor++] = kHex[(value >> shift) & 0x0f];
    };
    append_hex(::GetCurrentProcessId(), path_length);
    temporary_path[path_length++] = L'-';
    /** 一意な採番値を書き込む開始位置。 */
    const usize sequence_offset = path_length;

    /** 作成できた一時ファイルハンドル。 */
    HANDLE temporary = INVALID_HANDLE_VALUE;
    /** 一時ファイル作成時の直近 OS エラー。 */
    DWORD create_error = ERROR_FILE_EXISTS;
    /** 一時ファイル名の再試行回数。 */
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        /** 今回の一時ファイル名末尾位置。 */
        usize cursor = sequence_offset;
        append_hex(g_AtomicWriteSequence.FetchAdd(1), cursor);
        temporary_path[cursor] = L'\0';
        temporary = ::CreateFileW(temporary_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (temporary != INVALID_HANDLE_VALUE) break;
        create_error = ::GetLastError();
        if (create_error != ERROR_FILE_EXISTS && create_error != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (temporary == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 118, "WriteAllBytesAtomic: temporary CreateFileW failed", create_error);

    /** 一時ファイルへ書き込んだ byte 数。 */
    DWORD written = 0;
    /** 一時ファイル書き込みと flush の成否。 */
    BOOL write_ok = TRUE;
    if (size != 0) {
        g_WriteSyscalls.FetchAdd(1);
        write_ok = ::WriteFile(temporary, data, static_cast<DWORD>(size), &written, nullptr);
    }
    /** 書き込みまたは置換時の OS エラー。 */
    DWORD error = write_ok ? ERROR_SUCCESS : ::GetLastError();
    if (write_ok && written == size && !::FlushFileBuffers(temporary)) {
        write_ok = FALSE;
        error = ::GetLastError();
    }
    ::CloseHandle(temporary);
    if (!write_ok || written != size) {
        ::DeleteFileW(temporary_path);
        return ACS_ERR_OS(IO, 119, "WriteAllBytesAtomic: temporary write failed", error);
    }

    // 一時ファイル作成中に公開先が reparse point へ変わった競合も rename 前に再確認する。
    /** 置換直前の公開先属性。 */
    const DWORD current_destination_attributes = ::GetFileAttributesW(path);
    if (current_destination_attributes != INVALID_FILE_ATTRIBUTES && (current_destination_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        ::DeleteFileW(temporary_path);
        return WriteAllBytes(path, data, size);
    }

    /** 一時ファイルを公開先へ置換できたか。 */
    BOOL replaced = FALSE;
    if (current_destination_attributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ::ReplaceFileW(path, temporary_path, nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    }
    if (!replaced) {
        replaced = ::MoveFileExW(temporary_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!replaced) {
        error = ::GetLastError();
        ::DeleteFileW(temporary_path);
        return ACS_ERR_OS(IO, 121, "WriteAllBytesAtomic: replace failed", error);
    }
    return Ok();
}

// 文字列を書き出す（NUL 終端は書かない）
TResult<void> CFileSystem::WriteAllText(const wchar_t* path, const char* text) noexcept {
    /** 終端を除いた入力文字数。 */
    usize len = 0;
    while (text && text[len]) ++len;
    return WriteAllBytes(path, reinterpret_cast<const byte*>(text), len);
}

// ファイルサイズ取得
TResult<u64> CFileSystem::FileSize(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 119, "FileSize: path is null or empty");
    /** ファイルサイズを含む Win32 属性。 */
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &d))
        return ACS_ERR_OS(IO, 120, "GetFileAttributesExW failed", ::GetLastError());
    /** 64 bit に結合するファイルサイズ。 */
    LARGE_INTEGER sz{};
    sz.LowPart = d.nFileSizeLow;
    sz.HighPart = static_cast<LONG>(d.nFileSizeHigh);
    return TResult<u64>(OkInit, static_cast<u64>(sz.QuadPart));
}

// ファイル存在確認
bool CFileSystem::Exists(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0') return false;
    /** 対象パスの Win32 属性。 */
    const DWORD a = ::GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ディレクトリ存在確認
bool CFileSystem::DirectoryExists(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0') return false;
    /** 対象パスの Win32 属性。 */
    const DWORD a = ::GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** 親を含めてディレクトリを作成し、通常ファイルとの衝突はエラーにする。 */
TResult<void> CFileSystem::CreateDirectory(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0')
        return ACS_ERR(IO, 131, "CreateDirectory: path is null or empty");
    /** 途中の区切りを一時終端へ置換する作業パス。 */
    wchar_t buf[1024];
    /** 作業パスの文字数。 */
    usize n = 0;
    while (path[n] && n < 1023) { buf[n] = path[n]; ++n; }
    if (path[n] != L'\0')
        return ACS_ERR(IO, 131, "CreateDirectory: path is too long");
    buf[n] = 0;

    // drive root と UNC の server/share 部分は作成対象ではない。
    /** 中間ディレクトリを作り始める位置。 */
    usize creation_start = 0;
    if (n >= 3 && ((buf[0] >= L'A' && buf[0] <= L'Z') || (buf[0] >= L'a' && buf[0] <= L'z')) && buf[1] == L':' && IsPathSeparator(buf[2])) {
        creation_start = 3;
    } else if (n >= 2 && IsPathSeparator(buf[0]) && IsPathSeparator(buf[1])) {
        creation_start = n;
        /** UNC の server/share 区切り数。 */
        usize separator_count = 0;
        /** UNC の区切りを走査する位置。 */
        for (usize i = 2; i < n; ++i) {
            if (!IsPathSeparator(buf[i])) continue;
            ++separator_count;
            if (separator_count == 2) {
                creation_start = i + 1;
                break;
            }
        }
    } else if (n != 0 && IsPathSeparator(buf[0])) {
        creation_start = 1;
    }

    // CreateDirectoryWが既存volume rootへALREADY_EXISTS以外を返す環境でもroot契約を保つ。
    if (IsDirectoryRootPath(buf, n) && DirectoryExists(path)) return Ok();

    /** 中間ディレクトリ候補を走査する位置。 */
    for (usize i = 0; i < n; ++i) {
        /** 一時終端へ置換する元の文字。 */
        const wchar_t c = buf[i];
        if (IsPathSeparator(c) && i >= creation_start && i > 0) {
            buf[i] = 0;
            if (!DirectoryExists(buf) && !::CreateDirectoryW(buf, nullptr)) {
                /** 中間ディレクトリ作成時の OS エラー。 */
                const DWORD error = ::GetLastError();
                if (error != ERROR_ALREADY_EXISTS || !DirectoryExists(buf)) {
                    buf[i] = c;
                    return ACS_ERR_OS(IO, 132, "CreateDirectoryW parent failed", error);
                }
            }
            buf[i] = c;
        }
    }
    if (!::CreateDirectoryW(path, nullptr)) {
        /** 最終ディレクトリ作成時の OS エラー。 */
        const DWORD err = ::GetLastError();
        if (err != ERROR_ALREADY_EXISTS || !DirectoryExists(path))
            return ACS_ERR_OS(IO, 130, "CreateDirectoryW failed", err);
    }
    return Ok();
}

// ファイル削除
TResult<void> CFileSystem::Delete(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0')
        return ACS_ERR(IO, 139, "Delete: path is null or empty");
    if (!::DeleteFileW(path))
        return ACS_ERR_OS(IO, 140, "DeleteFileW failed", ::GetLastError());
    return Ok();
}

/** 現在の I/O 診断値を返す。 */
FFileSystemDiagnostics CFileSystem::Diagnostics() noexcept
{
    return FFileSystemDiagnostics{
        g_ReadSyscalls.Load(EMemoryOrder::Acquire),
        g_WriteSyscalls.Load(EMemoryOrder::Acquire),
        g_TextIntermediateCopyBytes.Load(EMemoryOrder::Acquire),
    };
}

/** I/O 診断値だけを 0 に戻す。 */
void CFileSystem::ResetDiagnostics() noexcept
{
    g_ReadSyscalls.Store(0, EMemoryOrder::Release);
    g_WriteSyscalls.Store(0, EMemoryOrder::Release);
    g_TextIntermediateCopyBytes.Store(0, EMemoryOrder::Release);
}

} // namespace acs
