// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "foundation/Platform.h"
#include "platform/FileSystem.h"

using namespace acs;

namespace {

/** 衝突後も通常ファイルが維持されたことを調べる内容。 */
constexpr byte kOriginalContents[] = {0x41u, 0x43u, 0x53u};

/** 衝突後に照合する通常ファイルのバイト数。 */
constexpr usize kOriginalContentSize = sizeof(kOriginalContents);

/** Win32を呼ばない入力拒否で維持される最終エラー値。 */
constexpr DWORD kLastErrorSentinel = 0xA5A5u;

/** 一意な一時パスを保持し、テスト終了時に残った通常ファイルまたは空ディレクトリを削除する。 */
class FTemporaryFileSystemPath final {
public:
    /** 一時ディレクトリ内に空の通常ファイルを作り、一意なパスを確保する。 */
    FTemporaryFileSystemPath() noexcept
    {
        /** OSが返す一時ディレクトリ。 */
        wchar_t temporary_directory[MAX_PATH] = {};
        /** Win32へ渡すバッファ容量。 */
        constexpr DWORD kCapacity = static_cast<DWORD>(MAX_PATH);
        /** 一時ディレクトリの文字数。 */
        const DWORD directory_length = ::GetTempPathW(kCapacity, temporary_directory);
        if (directory_length == 0u || directory_length >= kCapacity) return;
        if (::GetTempFileNameW(temporary_directory, L"acs", 0u, m_Path) == 0u) m_Path[0] = L'\0';
    }

    /** 一時パスのcleanup責務を複製しない。 */
    FTemporaryFileSystemPath(const FTemporaryFileSystemPath&) = delete;
    FTemporaryFileSystemPath& operator=(const FTemporaryFileSystemPath&) = delete;
    FTemporaryFileSystemPath(FTemporaryFileSystemPath&&) = delete;
    FTemporaryFileSystemPath& operator=(FTemporaryFileSystemPath&&) = delete;

    /** 残った通常ファイルまたは空ディレクトリを削除し、失敗をテストへ記録する。 */
    ~FTemporaryFileSystemPath() noexcept
    {
        if (!TryCleanup()) test::RecordFailure(FSourceLoc::Current(), "temporary path cleanup", "Win32 cleanup failed");
    }

    /** 一時パスを利用できる場合にtrueを返す。 */
    bool IsValid() const noexcept
    {
        return m_Path[0] != L'\0';
    }

    /** 一時パスを返す。 */
    const wchar_t* Get() const noexcept
    {
        return m_Path;
    }

    /** constructorが作った空の通常ファイルを削除する。 */
    bool TryRemovePlaceholder() noexcept
    {
        return IsValid() && ::DeleteFileW(m_Path) != FALSE;
    }

private:
    /** 現在のパス種別を調べ、通常ファイルまたは空ディレクトリを対応するWin32 APIで削除する。 */
    bool TryCleanup() noexcept
    {
        if (!IsValid()) return true;
        /** cleanup対象の現在の属性。 */
        const DWORD attributes = ::GetFileAttributesW(m_Path);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            /** 属性取得失敗時のWin32エラー。 */
            const DWORD error = ::GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) return ::RemoveDirectoryW(m_Path) != FALSE;
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0u) {
            /** 読み取り専用属性を外したcleanup用属性。 */
            const DWORD writable_attributes = attributes & ~FILE_ATTRIBUTE_READONLY;
            if (::SetFileAttributesW(m_Path, writable_attributes) == FALSE) return false;
        }
        return ::DeleteFileW(m_Path) != FALSE;
    }

    /** 一意な一時ファイルまたはディレクトリのパス。 */
    wchar_t m_Path[MAX_PATH] = {};
};

/**
 * 元のパスとsuffixを出力バッファへ連結する。
 * @param path 元の終端文字付きパス。
 * @param suffix 追加する終端文字付きsuffix。
 * @param output 連結結果の出力先。
 * @param capacity outputの要素容量。
 * @return 全体と終端文字が収まればtrue、不正入力または容量不足ならfalse。
 */
bool TryCopyWithSuffix(const wchar_t* path, const wchar_t* suffix, wchar_t* output, usize capacity) noexcept
{
    if (path == nullptr || suffix == nullptr || output == nullptr || capacity == 0u) return false;
    output[0] = L'\0';
    /** 次に書き込む出力位置。 */
    usize position = 0u;
    while (path[position] != L'\0') {
        if (position + 1u >= capacity) return false;
        output[position] = path[position];
        ++position;
    }
    /** 次に読み込むsuffix位置。 */
    usize suffix_position = 0u;
    while (suffix[suffix_position] != L'\0') {
        if (position + 1u >= capacity) return false;
        output[position++] = suffix[suffix_position++];
    }
    output[position] = L'\0';
    return true;
}

/**
 * 通常ファイルの内容が衝突検出前から変わっていないことを確認する。
 * @param path 読み戻す通常ファイルのパス。
 */
void ExpectOriginalContents(const wchar_t* path)
{
    /** 読み戻した通常ファイルの結果。 */
    auto contents_result = FFileSystem::ReadAllBytes(path);
    EXPECT_TRUE(contents_result.IsOk());
    if (contents_result.IsErr()) return;
    /** 読み戻した通常ファイルのバイト列。 */
    const TArray<byte>& contents = contents_result.Value();
    EXPECT_EQ(contents.Size(), kOriginalContentSize);
    /** 元の内容と照合するバイト位置。 */
    usize index = 0u;
    for (; index < contents.Size(); ++index) {
        EXPECT_EQ(contents[index], kOriginalContents[index]);
    }
}

} // namespace

/** 同名の通常ファイルを既存ディレクトリとして成功扱いせず、内容も維持することを確認する。 */
ACS_TEST(FileSystemSafety, RejectsExistingFileAsDirectory)
{
    /** 同名衝突に使う一意な一時パス。 */
    FTemporaryFileSystemPath path;
    EXPECT_TRUE(path.IsValid());
    if (!path.IsValid()) return;

    /** 衝突させる通常ファイルの作成結果。 */
    const TResult<void> write_result = FFileSystem::WriteAllBytes(path.Get(), kOriginalContents, kOriginalContentSize);
    EXPECT_TRUE(write_result.IsOk());
    if (write_result.IsErr()) return;

    /** 通常ファイルと同名のディレクトリ作成結果。 */
    const TResult<void> directory_result = FFileSystem::CreateDirectory(path.Get());
    EXPECT_TRUE(directory_result.IsErr());
    EXPECT_TRUE(FFileSystem::Exists(path.Get()));
    EXPECT_FALSE(FFileSystem::DirectoryExists(path.Get()));
    ExpectOriginalContents(path.Get());

    /** UNCではない一時ファイルならtrue。 */
    const bool path_is_unc = FFileSystem::IsPathSeparator(path.Get()[0]) && FFileSystem::IsPathSeparator(path.Get()[1]);
    if (path_is_unc) return;

    /** 同じ通常ファイルを指す拡張絶対パス。 */
    wchar_t extended_file[MAX_PATH + 4u] = {};
    /** 拡張絶対prefixと一時ファイルの連結結果。 */
    const bool extended_file_built = TryCopyWithSuffix(L"\\\\?\\", path.Get(), extended_file, MAX_PATH + 4u);
    EXPECT_TRUE(extended_file_built);
    if (!extended_file_built) return;

    /** 拡張絶対パスでも通常ファイルをrootと誤受理しない作成結果。 */
    const TResult<void> extended_result = FFileSystem::CreateDirectory(extended_file);
    EXPECT_TRUE(extended_result.IsErr());
    EXPECT_TRUE(FFileSystem::Exists(extended_file));
    EXPECT_FALSE(FFileSystem::DirectoryExists(extended_file));
    ExpectOriginalContents(extended_file);
}

/** nullptrと空パスをWin32へ渡さずIO:131で拒否することを確認する。 */
ACS_TEST(FileSystemSafety, RejectsNullAndEmptyPathsBeforeCallingOs)
{
    ::SetLastError(kLastErrorSentinel);
    /** nullptrの作成結果。 */
    const TResult<void> null_result = FFileSystem::CreateDirectory(nullptr);
    /** nullptr拒否直後のWin32最終エラー。 */
    const DWORD null_last_error = ::GetLastError();
    EXPECT_TRUE(null_result.IsErr());
    if (null_result.IsErr()) {
        EXPECT_EQ(null_result.Error().category, EErrCategory::IO);
        EXPECT_EQ(null_result.Error().subcode, static_cast<u16>(131));
        EXPECT_EQ(null_result.Error().os_error, 0u);
    }
    EXPECT_EQ(null_last_error, kLastErrorSentinel);

    ::SetLastError(kLastErrorSentinel);
    /** 空パスの作成結果。 */
    const TResult<void> empty_result = FFileSystem::CreateDirectory(L"");
    /** 空パス拒否直後のWin32最終エラー。 */
    const DWORD empty_last_error = ::GetLastError();
    EXPECT_TRUE(empty_result.IsErr());
    if (empty_result.IsErr()) {
        EXPECT_EQ(empty_result.Error().category, EErrCategory::IO);
        EXPECT_EQ(empty_result.Error().subcode, static_cast<u16>(131));
        EXPECT_EQ(empty_result.Error().os_error, 0u);
    }
    EXPECT_EQ(empty_last_error, kLastErrorSentinel);
}

/** 長すぎるパスをWin32へ渡さずIO:131で拒否することを確認する。 */
ACS_TEST(FileSystemSafety, RejectsTooLongPathBeforeCallingOs)
{
    /** 作業バッファ上限を一文字超える入力パス。 */
    wchar_t too_long_path[1025u] = {};
    /** 入力パスを埋める文字位置。 */
    for (usize position = 0u; position < 1024u; ++position) too_long_path[position] = L'a';

    ::SetLastError(kLastErrorSentinel);
    /** 長すぎるパスの作成結果。 */
    const TResult<void> result = FFileSystem::CreateDirectory(too_long_path);
    /** 入力拒否直後のWin32最終エラー。 */
    const DWORD last_error = ::GetLastError();

    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().category, EErrCategory::IO);
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(131));
        EXPECT_EQ(result.Error().os_error, 0u);
    }
    EXPECT_EQ(last_error, kLastErrorSentinel);
}

/** 同名ディレクトリが既にある場合は成功扱いにすることを確認する。 */
ACS_TEST(FileSystemSafety, AcceptsExistingDirectory)
{
    /** 既存ディレクトリへ切り替える一意な一時パス。 */
    FTemporaryFileSystemPath path;
    EXPECT_TRUE(path.IsValid());
    if (!path.IsValid()) return;

    /** ディレクトリ作成前の空ファイル削除結果。 */
    const bool placeholder_removed = path.TryRemovePlaceholder();
    EXPECT_TRUE(placeholder_removed);
    if (!placeholder_removed) return;

    /** Win32による事前ディレクトリ作成結果。 */
    const bool directory_created = ::CreateDirectoryW(path.Get(), nullptr) != FALSE;
    EXPECT_TRUE(directory_created);
    if (!directory_created) return;

    /** 既存ディレクトリに対する作成結果。 */
    const TResult<void> result = FFileSystem::CreateDirectory(path.Get());
    EXPECT_TRUE(result.IsOk());
    EXPECT_TRUE(FFileSystem::DirectoryExists(path.Get()));
}

/** 既存のドライブrootと拡張絶対rootを作成済みディレクトリとして受理することを確認する。 */
ACS_TEST(FileSystemSafety, AcceptsExistingVolumeRoots)
{
    /** volume root解決に使う既存の一時パス。 */
    FTemporaryFileSystemPath path;
    EXPECT_TRUE(path.IsValid());
    if (!path.IsValid()) return;

    /** 一時パスを含むvolumeのroot。 */
    wchar_t volume_root[MAX_PATH] = {};
    /** volume rootを取得できた場合はtrue。 */
    const bool volume_root_resolved = ::GetVolumePathNameW(path.Get(), volume_root, MAX_PATH) != FALSE;
    EXPECT_TRUE(volume_root_resolved);
    if (!volume_root_resolved) return;

    /** 通常のドライブrootに対する作成結果。 */
    const TResult<void> volume_result = FFileSystem::CreateDirectory(volume_root);
    EXPECT_TRUE(volume_result.IsOk());
    EXPECT_TRUE(FFileSystem::DirectoryExists(volume_root));

    /** UNC上の一時領域では通常root検証がserver/share契約も兼ねる。 */
    const bool volume_is_unc = FFileSystem::IsPathSeparator(volume_root[0]) && FFileSystem::IsPathSeparator(volume_root[1]);
    if (volume_is_unc) return;

    /** UNCと同じprefix走査分岐を通る拡張絶対root。 */
    wchar_t extended_root[MAX_PATH + 4u] = {};
    /** 拡張絶対prefixとvolume rootの連結結果。 */
    const bool extended_root_built = TryCopyWithSuffix(L"\\\\?\\", volume_root, extended_root, MAX_PATH + 4u);
    EXPECT_TRUE(extended_root_built);
    if (!extended_root_built) return;

    /** 拡張絶対rootに対する作成結果。 */
    const TResult<void> extended_result = FFileSystem::CreateDirectory(extended_root);
    EXPECT_TRUE(extended_result.IsOk());
    EXPECT_TRUE(FFileSystem::DirectoryExists(extended_root));

    /** volume rootをWin32 device namespaceへ置いた不正な作成対象。 */
    wchar_t device_root[MAX_PATH + 4u] = {};
    /** device namespace prefixとvolume rootの連結結果。 */
    const bool device_root_built = TryCopyWithSuffix(L"\\\\.\\", volume_root, device_root, MAX_PATH + 4u);
    EXPECT_TRUE(device_root_built);
    if (!device_root_built) return;

    /** device namespaceをUNC share rootと誤受理しない作成結果。 */
    const TResult<void> device_result = FFileSystem::CreateDirectory(device_root);
    EXPECT_TRUE(device_result.IsErr());
    EXPECT_TRUE(FFileSystem::DirectoryExists(device_root));
}

/** 末尾separator付きパスでもディレクトリを作成できることを確認する。 */
ACS_TEST(FileSystemSafety, CreatesDirectoryWithTrailingSeparator)
{
    /** 作成先に使う一意な一時パス。 */
    FTemporaryFileSystemPath path;
    EXPECT_TRUE(path.IsValid());
    if (!path.IsValid()) return;

    /** ディレクトリ作成前の空ファイル削除結果。 */
    const bool placeholder_removed = path.TryRemovePlaceholder();
    EXPECT_TRUE(placeholder_removed);
    if (!placeholder_removed) return;

    /** 末尾separatorを加えた作成パス。 */
    wchar_t path_with_separator[MAX_PATH + 1u] = {};
    /** 末尾separatorの連結結果。 */
    const bool path_built = TryCopyWithSuffix(path.Get(), L"\\", path_with_separator, MAX_PATH + 1u);
    EXPECT_TRUE(path_built);
    if (!path_built) return;

    /** 末尾separator付きパスの作成結果。 */
    const TResult<void> result = FFileSystem::CreateDirectory(path_with_separator);
    EXPECT_TRUE(result.IsOk());
    EXPECT_TRUE(FFileSystem::DirectoryExists(path.Get()));
    EXPECT_TRUE(FFileSystem::DirectoryExists(path_with_separator));
}

/** 中間要素が通常ファイルの場合はエラーにし、その内容も維持することを確認する。 */
ACS_TEST(FileSystemSafety, RejectsFileUsedAsParentDirectory)
{
    /** 親ディレクトリ位置を占有する一意な通常ファイル。 */
    FTemporaryFileSystemPath path;
    EXPECT_TRUE(path.IsValid());
    if (!path.IsValid()) return;

    /** 衝突させる通常ファイルの作成結果。 */
    const TResult<void> write_result = FFileSystem::WriteAllBytes(path.Get(), kOriginalContents, kOriginalContentSize);
    EXPECT_TRUE(write_result.IsOk());
    if (write_result.IsErr()) return;

    /** 通常ファイルの下へディレクトリを追加した不正パス。 */
    wchar_t child_path[MAX_PATH + 16u] = {};
    /** 子パスの連結結果。 */
    const bool path_built = TryCopyWithSuffix(path.Get(), L"\\child", child_path, MAX_PATH + 16u);
    EXPECT_TRUE(path_built);
    if (!path_built) return;

    /** 通常ファイルを親にしたディレクトリ作成結果。 */
    const TResult<void> result = FFileSystem::CreateDirectory(child_path);
    EXPECT_TRUE(result.IsErr());
    EXPECT_TRUE(FFileSystem::Exists(path.Get()));
    EXPECT_FALSE(FFileSystem::DirectoryExists(child_path));
    if (result.IsErr()) EXPECT_NE(result.Error().os_error, 0u);
    ExpectOriginalContents(path.Get());
}
