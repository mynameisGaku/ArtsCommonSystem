// SPDX-License-Identifier: Apache-2.0
// ファイル I/O 実装（Win32 Create/Read/Write FileW）
#include "platform/FileSystem.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * 読み取り用にファイルを開いてハンドルを返す。
 *
 * @details GENERIC_READ・FILE_SHARE_READ・OPEN_EXISTING で開くため未存在は失敗する。
 * @param path 開くファイルのパス。
 * @return ファイルハンドル (失敗時は INVALID_HANDLE_VALUE)。
 */
HANDLE OpenForRead(const wchar_t* path) noexcept {
    return ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

/**
 * 書き込み用にファイルを開いてハンドルを返す (既存は上書き)。
 *
 * @details GENERIC_WRITE・共有なし・CREATE_ALWAYS で開くため既存内容は破棄される。
 * @param path 開くファイルのパス。
 * @return ファイルハンドル (失敗時は INVALID_HANDLE_VALUE)。
 */
HANDLE OpenForWrite(const wchar_t* path) noexcept {
    return ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

// ファイル全体をバイト列として読み込む
TResult<TArray<byte>> FFileSystem::ReadAllBytes(const wchar_t* path) noexcept {
    const HANDLE h = OpenForRead(path);
    if (h == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 100, "CreateFileW (read) failed", ::GetLastError());

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, 101, "GetFileSizeEx failed", err);
    }
    if (size.QuadPart > 0xFFFFFFFFLL) {
        ::CloseHandle(h);
        return ACS_ERR(IO, 102, "File too large (>4GB)");
    }

    TArray<byte> buf;
    buf.Resize(static_cast<usize>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, buf.Data(), static_cast<DWORD>(buf.Size()), &read, nullptr);
    const DWORD err = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok || read != buf.Size())
        return ACS_ERR_OS(IO, 103, "ReadFile failed", err);
    return TResult<TArray<byte>>(OkInit, Move(buf));
}

// ファイル全体を文字列として読み込む（末尾に NUL を付与する）
TResult<TArray<char>> FFileSystem::ReadAllText(const wchar_t* path) noexcept {
    auto br = ReadAllBytes(path);
    if (br.IsErr()) return Err<TArray<char>>(br.Error());
    TArray<byte>& b = br.Value();
    TArray<char> out;
    out.Resize(b.Size() + 1);
    for (usize i = 0; i < b.Size(); ++i) out[i] = static_cast<char>(b[i]);
    out[b.Size()] = '\0';
    return TResult<TArray<char>>(OkInit, Move(out));
}

// バイト列を書き出す（上書き）
TResult<void> FFileSystem::WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept {
    const HANDLE h = OpenForWrite(path);
    if (h == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 110, "CreateFileW (write) failed", ::GetLastError());
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);
    const DWORD err = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok || wrote != size)
        return ACS_ERR_OS(IO, 111, "WriteFile failed", err);
    return Ok();
}

// 文字列を書き出す（NUL 終端は書かない）
TResult<void> FFileSystem::WriteAllText(const wchar_t* path, const char* text) noexcept {
    usize len = 0;
    while (text && text[len]) ++len;
    return WriteAllBytes(path, reinterpret_cast<const byte*>(text), len);
}

// ファイルサイズ取得
TResult<u64> FFileSystem::FileSize(const wchar_t* path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &d))
        return ACS_ERR_OS(IO, 120, "GetFileAttributesExW failed", ::GetLastError());
    LARGE_INTEGER sz{};
    sz.LowPart = d.nFileSizeLow;
    sz.HighPart = static_cast<LONG>(d.nFileSizeHigh);
    return TResult<u64>(OkInit, static_cast<u64>(sz.QuadPart));
}

// ファイル存在確認
bool FFileSystem::Exists(const wchar_t* path) noexcept {
    const DWORD a = ::GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ディレクトリ存在確認
bool FFileSystem::DirectoryExists(const wchar_t* path) noexcept {
    const DWORD a = ::GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ディレクトリ作成（既に存在する場合は成功扱い、親も再帰作成）
TResult<void> FFileSystem::CreateDirectory(const wchar_t* path) noexcept {
    if (DirectoryExists(path)) return Ok();
    wchar_t buf[1024];
    usize n = 0;
    while (path[n] && n < 1023) { buf[n] = path[n]; ++n; }
    buf[n] = 0;
    for (usize i = 0; i < n; ++i) {
        const wchar_t c = buf[i];
        if ((c == L'\\' || c == L'/') && i > 0) {
            buf[i] = 0;
            if (!DirectoryExists(buf)) ::CreateDirectoryW(buf, nullptr);
            buf[i] = c;
        }
    }
    if (!::CreateDirectoryW(path, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return ACS_ERR_OS(IO, 130, "CreateDirectoryW failed", err);
    }
    return Ok();
}

// ファイル削除
TResult<void> FFileSystem::Delete(const wchar_t* path) noexcept {
    if (!::DeleteFileW(path))
        return ACS_ERR_OS(IO, 140, "DeleteFileW failed", ::GetLastError());
    return Ok();
}

} // namespace acs
