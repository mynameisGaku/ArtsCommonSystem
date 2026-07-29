// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — FAcpakReader 実装 (Win32 I/O + CRC32 検証)
// -----------------------------------------------------------------------------
// 実装範囲:
//   ・magic / version / flags 検証
//   ・file table をエントリ毎に逐次読み出して `TArray<FAcpakFileEntry>` を構築
//   ・各 path は `m_StringPool` に NUL 終端付きで連結保存し、entry.path はその
//     先頭へのポインタを保持する
//   ・CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF) 検証
//   ・flags = 0 のみ正常パス、Encrypted / Compressed bit は NotImplemented
//
// 内部設計のポイント:
//   ・file table を「2 パス」で読む。1 パス目で各 entry の path 長 + 数値メタ
//     データだけ拾い、m_StringPool を 1 度だけ最終サイズで Reserve する。
//     2 パス目で path 文字列を pool にコピーし、entry.path を pool の最終
//     アドレスにバインドする。
//   ・これにより「pool が PushBack 中に grow して既存 entry.path が dangling
//     になる」問題を構造的に防ぐ (= grow が起きない条件で操作する)。
// =============================================================================
#include "assetpack/AcpakReader.h"

#include "foundation/Platform.h" // <windows.h> を 1 箇所に隠す
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "memory/Memory.h" // MemCopy / MemSet
#include "threading/ScopedLock.h"

#include "assetpack/AcpakCrypto.h" // AES-256-GCM 復号
#include "assetpack/AcpakLz4.h"    // LZ4 解凍

namespace acs::assetpack {

namespace {

/**
 * CRC32 lookup table (poly 0xEDB88320) を遅延構築して返す。
 *
 * @details 256-entry table を最初の呼び出し時に組み立てる (Meyers singleton)。
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

    // C++11 の関数ローカル static 初期化はスレッド間で一度だけ実行される。
    static const FCrc32Table Table;
    return Table.Values;
}

/**
 * バイト列の CRC32 を計算する。
 *
 * @details init = 0xFFFFFFFF, xorout = 0xFFFFFFFF (Zlib / PNG と同じ規約)。
 * @param data CRC を計算する対象バイト列。
 * @param size data のバイト数。
 * @return 計算した CRC32。
 */
u32 ComputeCrc32(const void* Data, u64 Size) noexcept
{
    const u32* Table = GetCrc32Table();
    const u8* Bytes = static_cast<const u8*>(Data);
    u32 Crc = 0xFFFFFFFFu;
    for (u64 Index = 0; Index < Size; ++Index) {
        Crc = Table[(Crc ^ Bytes[Index]) & 0xFFu] ^ (Crc >> 8);
    }
    return Crc ^ 0xFFFFFFFFu;
}

/**
 * 指定オフセットから size バイトを dst に読み出す (SetFilePointerEx + ReadFile)。
 *
 * @details
 * ReadFile は DWORD (32bit) 単位でしか読めないため、4GiB 超はチャンク分割して
 * ループする。size == 0 は何もせず成功を返す。
 * @param h 読み出し対象のファイルハンドル。
 * @param offset 読み出し開始のファイルオフセット。
 * @param dst 読み出し先バッファ。
 * @param size 読み出すバイト数。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
 * @return 成功なら true、失敗なら false。
 */
bool ReadAt(HANDLE Handle, u64 Offset, void* Destination, u64 Size, DWORD& Error, FMutex& IoLock) noexcept
{
    FScopedLock Lock(IoLock);
    Error = 0;
    if (Size == 0) {
        return true;
    }
    LARGE_INTEGER Position{};
    Position.QuadPart = static_cast<LONGLONG>(Offset);
    if (!::SetFilePointerEx(Handle, Position, nullptr, FILE_BEGIN)) {
        Error = ::GetLastError();
        return false;
    }
    // ReadFile は DWORD (32bit) 単位でしか読めないため、>4GiB をループする。
    u8* DestinationBytes = static_cast<u8*>(Destination);
    u64 Remaining = Size;
    while (Remaining > 0) {
        const DWORD Chunk = (Remaining > 0x7FFFFFFFu) ? 0x7FFFFFFFu : static_cast<DWORD>(Remaining);
        DWORD ReadBytes = 0;
        if (!::ReadFile(Handle, DestinationBytes, Chunk, &ReadBytes, nullptr) || ReadBytes == 0) {
            Error = ::GetLastError();
            if (Error == 0) {
                Error = ERROR_HANDLE_EOF;
            }
            return false;
        }
        DestinationBytes += ReadBytes;
        Remaining -= ReadBytes;
    }
    return true;
}

/**
 * 4 バイトを u32 (LE) として strict-aliasing 安全に読む。
 *
 * @details
 * reinterpret_cast<u32*> は strict-aliasing 違反になり得るため memcpy で読む。
 * ACS 対応プラットフォームは全て LE なので生バイト並びがそのまま LE 整数になる。
 * @param src 読み出し元 (4 バイト)。
 * @return 読み出した u32。
 */
u32 ReadU32LE(const u8* src) noexcept
{
    u32 v = 0;
    MemCopy(&v, src, sizeof(u32));
    return v;
}

/**
 * 8 バイトを u64 (LE) として strict-aliasing 安全に読む。
 *
 * @param src 読み出し元 (8 バイト)。
 * @return 読み出した u64。
 */
u64 ReadU64LE(const u8* src) noexcept
{
    u64 v = 0;
    MemCopy(&v, src, sizeof(u64));
    return v;
}

/**
 * 自前 wcscmp で 2 つの NUL 終端 wchar_t 列を比較する (依存ヘッダを増やさない)。
 *
 * @param a 比較対象の文字列 (NUL 終端)。
 * @param b 比較対象の文字列 (NUL 終端)。
 * @return 一致なら 0、a<b で負、a>b で正。
 */
int CompareW(const wchar_t* a, const wchar_t* b) noexcept
{
    while (*a && (*a == *b)) {
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

bool EqualPathUnits(const wchar_t* Left, usize LeftLength,
                    const wchar_t* Right, usize RightLength) noexcept
{
    if (LeftLength != RightLength) return false;
    for (usize Index = 0; Index < LeftLength; ++Index) {
        if (Left[Index] != Right[Index]) return false;
    }
    return true;
}

bool IsValidVirtualPath(const wchar_t* Path, usize Length) noexcept
{
    if (Path == nullptr || Length == 0 || Path[0] == L'/' || Path[Length - 1u] == L'/') {
        return false;
    }

    usize SegmentStart = 0;
    for (usize Index = 0; Index < Length; ++Index) {
        const wchar_t Character = Path[Index];
        if (Character == L'\0' || Character < 0x20 || Character == L'\\' || Character == L':') {
            return false;
        }
        if (Character >= 0xD800 && Character <= 0xDBFF) {
            if (Index + 1u >= Length || Path[Index + 1u] < 0xDC00 || Path[Index + 1u] > 0xDFFF) {
                return false;
            }
            ++Index;
            continue;
        }
        if (Character >= 0xDC00 && Character <= 0xDFFF) {
            return false;
        }
        if (Character != L'/') continue;

        const usize SegmentLength = Index - SegmentStart;
        if (SegmentLength == 0 ||
            (SegmentLength == 1 && Path[SegmentStart] == L'.') ||
            (SegmentLength == 2 && Path[SegmentStart] == L'.' && Path[SegmentStart + 1u] == L'.')) {
            return false;
        }
        SegmentStart = Index + 1u;
    }

    const usize LastLength = Length - SegmentStart;
    return !((LastLength == 1u && Path[SegmentStart] == L'.') ||
             (LastLength == 2u && Path[SegmentStart] == L'.' &&
              Path[SegmentStart + 1u] == L'.'));
}

bool SameFileSnapshot(const BY_HANDLE_FILE_INFORMATION& Left,
                      const BY_HANDLE_FILE_INFORMATION& Right) noexcept
{
    return Left.dwVolumeSerialNumber == Right.dwVolumeSerialNumber &&
           Left.nFileIndexHigh == Right.nFileIndexHigh &&
           Left.nFileIndexLow == Right.nFileIndexLow &&
           Left.nFileSizeHigh == Right.nFileSizeHigh &&
           Left.nFileSizeLow == Right.nFileSizeLow &&
           Left.ftLastWriteTime.dwHighDateTime == Right.ftLastWriteTime.dwHighDateTime &&
           Left.ftLastWriteTime.dwLowDateTime == Right.ftLastWriteTime.dwLowDateTime;
}

} // namespace

/** DefaultAllocator で空の Reader を構築する。 */
FAcpakReader::FAcpakReader() noexcept : FAcpakReader(DefaultAllocator())
{
}

/** 指定 allocator で file table と文字列 pool を構築する。 */
FAcpakReader::FAcpakReader(FAllocator& Allocator) noexcept
    : m_Entries(Allocator), m_StringPool(Allocator),
      m_StoredScratch(Allocator), m_FinalScratch(Allocator)
{
}

/** 破棄時に Close を呼んで後始末する。 */
FAcpakReader::~FAcpakReader() noexcept
{
    Close();
}

/** ハンドルを閉じ、文字列 pool + entry 配列 + 鍵情報を解放/0 クリアする。 */
void FAcpakReader::Close() noexcept
{
    FScopedExclusiveLock Lock(m_LifecycleLock);
    CloseUnlocked();
}

/** ライフサイクルロック取得済みで内部状態を空に戻す。 */
void FAcpakReader::CloseUnlocked() noexcept
{
    if (m_MappedView != nullptr) {
        ::UnmapViewOfFile(m_MappedView);
        m_MappedView = nullptr;
    }
    if (m_FileMappingHandle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_FileMappingHandle));
        m_FileMappingHandle = nullptr;
    }
    if (m_FileHandle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_FileHandle));
        m_FileHandle = nullptr;
    }
    m_FileSize = 0;
    m_Flags = 0;
    m_TableOffset = 0;
    m_Entries.ReleaseStorage();
    m_StringPool.ReleaseStorage();
    m_StoredScratch.ReleaseStorage();
    m_FinalScratch.ReleaseStorage();

    // 鍵情報の defensive zero (再 Open のときに古い鍵が漏れないよう)。
    MemSet(m_Key.bytes, 0, sizeof(m_Key.bytes));
    m_HasKey = false;
}

/** 暗号化 pak の復号鍵を内部にコピーし、鍵設定済みフラグを立てる。 */
void FAcpakReader::SetKey(const FAcpakKey& Key) noexcept
{
    FScopedExclusiveLock Lock(m_LifecycleLock);
    MemCopy(m_Key.bytes, Key.bytes, sizeof(m_Key.bytes));
    m_HasKey = true;
}

/** `.acpak` を開き、header と file table を読み出す (失敗時は Close 相当に戻す)。 */
TResult<void> FAcpakReader::Open(const wchar_t* FilePath) noexcept
{
    FScopedExclusiveLock Lock(m_LifecycleLock);

    // 新しいファイルは独立した Reader に完全に構築する。失敗時は現在の
    // handle / manifest / key を一切変更しない。
    FAcpakReader Staged(*m_Entries.GetAllocator());
    if (m_HasKey) {
        MemCopy(Staged.m_Key.bytes, m_Key.bytes, sizeof(m_Key.bytes));
        Staged.m_HasKey = true;
    }
    const auto Result = Staged.OpenIntoEmptyUnlocked(FilePath);
    if (Result.IsErr()) {
        return Result.Error();
    }

    CloseUnlocked();
    m_FileHandle = Staged.m_FileHandle;
    Staged.m_FileHandle = nullptr;
    m_FileMappingHandle = Staged.m_FileMappingHandle;
    Staged.m_FileMappingHandle = nullptr;
    m_MappedView = Staged.m_MappedView;
    Staged.m_MappedView = nullptr;
    m_FileSize = Staged.m_FileSize;
    Staged.m_FileSize = 0;
    m_Flags = Staged.m_Flags;
    Staged.m_Flags = 0;
    m_TableOffset = Staged.m_TableOffset;
    Staged.m_TableOffset = 0;
    m_Entries = Move(Staged.m_Entries);
    m_StringPool = Move(Staged.m_StringPool);
    MemCopy(m_Key.bytes, Staged.m_Key.bytes, sizeof(m_Key.bytes));
    m_HasKey = Staged.m_HasKey;
    MemSet(Staged.m_Key.bytes, 0, sizeof(Staged.m_Key.bytes));
    Staged.m_HasKey = false;
    return Ok();
}

TResult<void> FAcpakReader::OpenIntoEmptyUnlocked(const wchar_t* FilePath) noexcept
{
    if (FilePath == nullptr || FilePath[0] == L'\0') {
        return ACS_ERR(IO, kAcpakSubBadPath, "FAcpakReader::Open: file_path is null or empty");
    }
    usize FilePathLength = 0;
    while (FilePathLength <= kAcpakMaxOutputPathLength &&
           FilePath[FilePathLength] != L'\0') {
        ++FilePathLength;
    }
    if (FilePathLength > kAcpakMaxOutputPathLength) {
        return ACS_ERR(IO, kAcpakSubBadPath,
                       "FAcpakReader::Open: file_path exceeds limit");
    }

    const HANDLE Handle = ::CreateFileW(FilePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (Handle == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: CreateFileW failed", ::GetLastError());
    }

    BY_HANDLE_FILE_INFORMATION Before{};
    if (!::GetFileInformationByHandle(Handle, &Before)) {
        const DWORD Error = ::GetLastError();
        ::CloseHandle(Handle);
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakReader::Open: initial file snapshot failed", Error);
    }

    LARGE_INTEGER FileSize{};
    if (!::GetFileSizeEx(Handle, &FileSize) || FileSize.QuadPart < 0) {
        const DWORD Error = ::GetLastError();
        ::CloseHandle(Handle);
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: GetFileSizeEx failed", Error);
    }

    m_FileHandle = Handle;
    m_FileSize = static_cast<u64>(FileSize.QuadPart);

    const auto Result = LoadHeaderAndTable();
    if (Result.IsErr()) {
        CloseUnlocked();
        return Result.Error();
    }

    BY_HANDLE_FILE_INFORMATION After{};
    if (!::GetFileInformationByHandle(Handle, &After)) {
        const DWORD Error = ::GetLastError();
        CloseUnlocked();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakReader::Open: final file snapshot failed", Error);
    }
    if (!SameFileSnapshot(Before, After)) {
        CloseUnlocked();
        return ACS_ERR(IO, kAcpakSubFileChanged,
                       "FAcpakReader::Open: file changed while manifest was read");
    }
    TryCreateReadMappingUnlocked();
    return Ok();
}

void FAcpakReader::TryCreateReadMappingUnlocked() noexcept
{
    if (m_FileHandle == nullptr || m_FileSize < kAcpakMappedReadMinimumBytes || m_FileSize > static_cast<u64>(static_cast<usize>(-1))) {
        return;
    }

    const HANDLE Mapping = ::CreateFileMappingW(static_cast<HANDLE>(m_FileHandle), nullptr, PAGE_READONLY, 0u, 0u, nullptr);
    if (Mapping == nullptr) return;

    const void* const View = ::MapViewOfFile(Mapping, FILE_MAP_READ, 0u, 0u, 0u);
    if (View == nullptr) {
        ::CloseHandle(Mapping);
        return;
    }
    m_FileMappingHandle = Mapping;
    m_MappedView = static_cast<const u8*>(View);
}

/** header (36B) と file table の全 entry を 2 パスで読み出す。 */
TResult<void> FAcpakReader::LoadHeaderAndTable() noexcept
{
    // 2 パス構成:
    //   Pass 1: 各 entry の (path_len, offset, size_uncompressed, size_stored,
    //           crc32, path_pool_offset) を読み出す。path 文字列は paths_temp に
    //           連結保存し、m_StringPool の最終サイズを確定する。
    //   Pass 2: m_StringPool に paths_temp をコピーし、entries_raw を m_Entries
    //           に変換 (entry.path = pool_base + path_pool_offset)。
    // このやり方なら m_StringPool は Pass 2 で 1 度だけ Resize するので、
    // PushBack 中の re-grow による dangling pointer が起きない。
    const HANDLE Handle = static_cast<HANDLE>(m_FileHandle);

    if (m_FileSize < kAcpakHeaderDiskSize) {
        return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: file smaller than header");
    }

    // ---- ヘッダを 36 バイト読む ------------------------------------------
    u8 HeaderBytes[kAcpakHeaderDiskSize] = {};
    DWORD Error = 0;
    if (!ReadAt(Handle, 0, HeaderBytes, kAcpakHeaderDiskSize, Error, m_IoLock)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: ReadFile (header) failed", Error);
    }

    // magic 比較 (バイト列、strict-aliasing 安全)
    for (u32 Index = 0; Index < 8; ++Index) {
        if (HeaderBytes[Index] != kAcpakMagic[Index]) {
            return ACS_ERR(Asset, kAcpakSubBadMagic, "FAcpakReader::Open: magic mismatch (not an .acpak)");
        }
    }

    const u32 Version = ReadU32LE(HeaderBytes + 8);
    const u32 HeaderFlags = ReadU32LE(HeaderBytes + 12);
    const u32 FileCount = ReadU32LE(HeaderBytes + 16);
    const u32 HeaderPadding = ReadU32LE(HeaderBytes + 20);
    const u64 FileTableOffset = ReadU64LE(HeaderBytes + 24);
    const u32 HeaderReserved = ReadU32LE(HeaderBytes + 32);

    if (Version != kAcpakVersion) {
        ACS_LOG_WARN("FAcpakReader::Open: version mismatch (file=%u, expected=%u)", Version, kAcpakVersion);
        return ACS_ERR(Asset, kAcpakSubBadVersion, "FAcpakReader::Open: unsupported .acpak version");
    }

    // Encrypted / Compressed bit は実装済。未知 bit のみ拒否する。
    const u32 KnownFlags = static_cast<u32>(AcpakFlagEncrypted) | static_cast<u32>(AcpakFlagCompressed);
    if ((HeaderFlags & ~KnownFlags) != 0) {
        return ACS_ERR(Asset, kAcpakSubBadFlags, "FAcpakReader::Open: unknown flag bits in header");
    }
    if (HeaderPadding != 0 || HeaderReserved != 0) {
        return ACS_ERR(Asset, kAcpakSubBadSchema,
                       "FAcpakReader::Open: non-zero reserved header field");
    }

    if (FileTableOffset < kAcpakHeaderDiskSize || FileTableOffset > m_FileSize) {
        return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: file_table_offset out of range");
    }

    // allocation 前に、実ファイルの残量だけで entry 数を上限評価する。
    const bool bEncrypted = (HeaderFlags & static_cast<u32>(AcpakFlagEncrypted)) != 0u;
    const bool bCompressed = (HeaderFlags & static_cast<u32>(AcpakFlagCompressed)) != 0u;
    const u64 TailBytes = bEncrypted ? (28u + kAcpakCipherFieldsDiskSize) : 28u;
    const u64 MinEntryBytes = 4u + TailBytes;
    const u64 RemainingTableBytes = m_FileSize - FileTableOffset;
    if (FileCount > kAcpakMaxFileCount || static_cast<u64>(FileCount) > RemainingTableBytes / MinEntryBytes) {
        return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: file_count exceeds table bounds");
    }

    m_Flags = HeaderFlags;
    m_TableOffset = FileTableOffset;

    // file_count == 0 ならテーブル読み出しは不要 (空 pak)。
    if (FileCount == 0) {
        if (FileTableOffset != m_FileSize) {
            return ACS_ERR(Asset, kAcpakSubBadSchema,
                           "FAcpakReader::Open: empty manifest has trailing data");
        }
        return Ok();
    }

    // ---- Pass 1: 各 entry のメタデータと path 長を読み、path 文字列を
    //              `paths_temp` (TArray<wchar_t>) に連結する -----------------
    // 内部表現: entry のうち path だけ後で resolve するので、index 配列を別途
    // 保持する。encrypted pak のときは追加で cipher_nonce/tag も読む。
    struct FRawEntry {
        usize PathPoolOffset;
        usize PathLength;
        u64 Offset;
        u64 UncompressedSize;
        u64 StoredSize;
        u32 Crc32;
        u8 CipherNonce[12];
        u8 CipherTag[16];
    };

    TArray<FRawEntry> Raws(*m_Entries.GetAllocator());
    TArray<wchar_t> PathsTemporary(*m_StringPool.GetAllocator());
    if (!Raws.TryReserve(FileCount)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: entry metadata allocation failed");
    }

    u64 Cursor = FileTableOffset;

    for (u32 Index = 0; Index < FileCount; ++Index) {
        // path_len (u32)
        if (Cursor > m_FileSize || m_FileSize - Cursor < 4u) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: file table truncated (path_len)");
        }
        u8 PathLengthBytes[4];
        if (!ReadAt(Handle, Cursor, PathLengthBytes, 4, Error, m_IoLock)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: ReadFile (path_len) failed", Error);
        }
        const u32 PathLength = ReadU32LE(PathLengthBytes);
        Cursor += 4;

        // path 長を許容範囲に制限する (悪意のあるアーカイブで OOM を防ぐ)。
        // Win32 long path 対応のため 4096 wchar_t = 8KB まで許容する。
        if (PathLength == 0 || PathLength > kAcpakMaxPathLength) {
            return ACS_ERR(IO, kAcpakSubBadPath,
                           "FAcpakReader::Open: path_len is zero or exceeds limit");
        }

        const u64 PathBytes = static_cast<u64>(PathLength) * sizeof(wchar_t);
        // entry の残りサイズ: path + offset(8) + size_uncompressed(8) +
        // size_stored(8) + crc32(4) = path_bytes + 28
        // encrypted のときはさらに cipher_nonce(12) + cipher_tag(16) = 28
        if (Cursor > m_FileSize || PathBytes > m_FileSize - Cursor ||
            TailBytes > m_FileSize - Cursor - PathBytes) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: file table truncated (entry)");
        }

        // path を paths_temp に書き込む (NUL 込みで連結)。
        // path_len の現在末尾オフセットを記録する。
        const usize PoolOffset = PathsTemporary.Size();

        // PushBack ループ — TArray は exponential grow なので O(amortized 1)/wchar_t
        if (PathLength > 0) {
            // 効率のため Resize して直接 ReadFile する
            const usize PreviousSize = PathsTemporary.Size();
            const usize PathUnits = static_cast<usize>(PathLength);
            if (PreviousSize > static_cast<usize>(-1) - PathUnits - 1u ||
                (PreviousSize + PathUnits + 1u) > kAcpakMaxPathPoolBytes / sizeof(wchar_t)) {
                return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::Open: path pool exceeds limit");
            }
            if (!PathsTemporary.TryResize(PreviousSize + PathUnits)) {
                return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: path pool allocation failed");
            }
            if (!ReadAt(Handle, Cursor, PathsTemporary.Data() + PreviousSize, PathBytes, Error, m_IoLock)) {
                return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: ReadFile (path) failed", Error);
            }
        }

        const wchar_t* const CurrentPath = PathsTemporary.Data() + PoolOffset;
        if (!IsValidVirtualPath(CurrentPath, PathLength)) {
            return ACS_ERR(Asset, kAcpakSubBadPath,
                           "FAcpakReader::Open: invalid virtual path");
        }
        for (usize PriorIndex = 0; PriorIndex < Raws.Size(); ++PriorIndex) {
            const FRawEntry& Prior = Raws[PriorIndex];
            if (EqualPathUnits(CurrentPath, PathLength,
                               PathsTemporary.Data() + Prior.PathPoolOffset, Prior.PathLength)) {
                return ACS_ERR(Asset, kAcpakSubDuplicatePath,
                               "FAcpakReader::Open: duplicate virtual path");
            }
        }
        if (!PathsTemporary.TryPushBack(L'\0')) {
            return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: path terminator allocation failed");
        }
        Cursor += PathBytes;

        // offset / size_uncompressed / size_stored / crc32 (28 バイト)
        // + 暗号化時は nonce(12) + tag(16) を続けて読む。
        u8 Tail[28 + 12 + 16];
        const u32 TailRead = static_cast<u32>(TailBytes);
        if (!ReadAt(Handle, Cursor, Tail, TailRead, Error, m_IoLock)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::Open: ReadFile (entry tail) failed", Error);
        }
        Cursor += TailRead;

        FRawEntry Raw{};
        Raw.PathPoolOffset = PoolOffset;
        Raw.PathLength = PathLength;
        Raw.Offset = ReadU64LE(Tail + 0);
        Raw.UncompressedSize = ReadU64LE(Tail + 8);
        Raw.StoredSize = ReadU64LE(Tail + 16);
        Raw.Crc32 = ReadU32LE(Tail + 24);
        if (bEncrypted) {
            MemCopy(Raw.CipherNonce, Tail + 28, 12);
            MemCopy(Raw.CipherTag, Tail + 40, 16);
        }
        // else: ゼロ初期化のまま (RawEntry r{} で 0 クリア済)

        // sanity: entry が指す data 領域が file_size に収まっているか。
        // offset / size_stored は untrusted な u64 のため、`offset + size_stored`
        // を直接比較すると加算が u64 で wrap し、巨大値の組み合わせ
        // (例: offset=0xFFFF...FF00, size_stored=0x200) が境界チェックをすり抜け、
        // ReadFile で任意オフセットへ seek して OOB read する。減算側で評価して
        // overflow を回避する: size_stored > file_size || offset > file_size - size_stored。
        if (Raw.StoredSize > FileTableOffset ||
            Raw.Offset < kAcpakHeaderDiskSize ||
            Raw.Offset > FileTableOffset - Raw.StoredSize) {
            return ACS_ERR(IO, kAcpakSubBadSize,
                           "FAcpakReader::Open: entry data range crosses header or manifest");
        }
        // stored != uncompressed は AcpakFlagCompressed のときだけ許容。
        // Compressed 立ってないのに stored != uncompressed なら破損アーカイブ。
        if (!bCompressed && Raw.StoredSize != Raw.UncompressedSize) {
            return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::Open: stored != uncompressed but flag clear");
        }

        if (Raw.StoredSize > 0) {
            for (usize PriorIndex = 0; PriorIndex < Raws.Size(); ++PriorIndex) {
                const FRawEntry& Prior = Raws[PriorIndex];
                if (Prior.StoredSize > 0 &&
                    Raw.Offset < Prior.Offset + Prior.StoredSize &&
                    Prior.Offset < Raw.Offset + Raw.StoredSize) {
                    return ACS_ERR(Asset, kAcpakSubBadSize,
                                   "FAcpakReader::Open: entry data ranges overlap");
                }
            }
        }

        if (!Raws.TryPushBack(Raw)) {
            return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: entry metadata allocation failed");
        }
    }

    if (Cursor != m_FileSize) {
        return ACS_ERR(Asset, kAcpakSubBadSchema,
                       "FAcpakReader::Open: manifest has trailing unknown data");
    }

    // ---- Pass 2: m_StringPool を最終サイズで Resize し、paths_temp を
    //              ムーブ相当でコピー。entry を組み立てて m_Entries に格納 ----
    if (!m_StringPool.TryResize(PathsTemporary.Size())) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: final path pool allocation failed");
    }
    if (!PathsTemporary.IsEmpty()) {
        MemCopy(m_StringPool.Data(), PathsTemporary.Data(), PathsTemporary.Size() * sizeof(wchar_t));
    }
    // ここから m_StringPool は再 grow させない (entry.path は pool ベースに
    // 依存するため)。

    if (!m_Entries.TryReserve(Raws.Size())) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: final entry allocation failed");
    }
    const wchar_t* PoolBase = m_StringPool.Data();
    for (usize Index = 0; Index < Raws.Size(); ++Index) {
        const FRawEntry& Raw = Raws[Index];
        FAcpakFileEntry Entry{};
        Entry.path = PoolBase + Raw.PathPoolOffset;
        Entry.offset = Raw.Offset;
        Entry.size_uncompressed = Raw.UncompressedSize;
        Entry.size_stored = Raw.StoredSize;
        Entry.crc32 = Raw.Crc32;
        // 暗号化フィールドは encrypted pak のみ意味あり、それ以外は 0。
        MemCopy(Entry.cipher_nonce, Raw.CipherNonce, 12);
        MemCopy(Entry.cipher_tag, Raw.CipherTag, 16);
        if (!m_Entries.TryPushBack(Entry)) {
            return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::Open: final entry allocation failed");
        }
    }

    return Ok();
}

/** index 番目の entry を返す (範囲外 / 未 Open なら nullptr)。 */
bool FAcpakReader::IsOpen() const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    return m_FileHandle != nullptr;
}

/** 現在開いている pak の entry 数を返す。 */
u32 FAcpakReader::FileCount() const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    return static_cast<u32>(m_Entries.Size());
}

/** header.flags を返す。 */
u32 FAcpakReader::Flags() const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    return m_Flags;
}

/** index 番目の entry を返す (範囲外 / 未 Open なら nullptr)。 */
const FAcpakFileEntry* FAcpakReader::GetEntry(u32 Index) const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (m_FileHandle == nullptr || static_cast<usize>(Index) >= m_Entries.Size()) {
        return nullptr;
    }
    return &m_Entries[static_cast<usize>(Index)];
}

/** 仮想パスから entry を線形探索で探す (無い / 未 Open なら nullptr)。 */
const FAcpakFileEntry* FAcpakReader::FindEntry(const wchar_t* Path) const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    return FindEntryUnlocked(Path);
}

/** ライフサイクル共有ロック取得済みで仮想パスを検索する。 */
const FAcpakFileEntry* FAcpakReader::FindEntryUnlocked(const wchar_t* Path) const noexcept
{
    if (m_FileHandle == nullptr || Path == nullptr) {
        return nullptr;
    }
    for (usize Index = 0; Index < m_Entries.Size(); ++Index) {
        if (CompareW(m_Entries[Index].path, Path) == 0) {
            return &m_Entries[Index];
        }
    }
    return nullptr;
}

/** 仮想パスの復号 + 解凍後のバイト数を返す (未存在は kAcpakSubNotFound)。 */
TResult<u64> FAcpakReader::GetUncompressedSize(const wchar_t* Path) const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "FAcpakReader::GetUncompressedSize: pak not open");
    }
    const FAcpakFileEntry* Entry = FindEntryUnlocked(Path);
    if (Entry == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotFound, "FAcpakReader::GetUncompressedSize: path not found");
    }
    return TResult<u64>(OkInit, Entry->size_uncompressed);
}

/** 仮想パスのファイルを out_buffer に読み出す (復号 → 解凍 → CRC32 検証)。 */
TResult<u64> FAcpakReader::ReadFile(const wchar_t* Path, void* OutBuffer, u64 BufferSize) noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "FAcpakReader::ReadFile: pak not open");
    }
    if (OutBuffer == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure, "FAcpakReader::ReadFile: out_buffer is null");
    }
    const FAcpakFileEntry* Entry = FindEntryUnlocked(Path);
    if (Entry == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotFound, "FAcpakReader::ReadFile: path not found");
    }
    if (BufferSize < Entry->size_uncompressed) {
        return ACS_ERR(IO, kAcpakSubBufferTooSmall, "FAcpakReader::ReadFile: buffer too small");
    }
    return ReadEntryUnlocked(*Entry, OutBuffer, BufferSize);
}

TResult<u64> FAcpakReader::ReadEntryUnlocked(const FAcpakFileEntry& EntryReference, void* OutBuffer, u64 BufferSize) noexcept
{
    const FAcpakFileEntry* const Entry = &EntryReference;
    if (OutBuffer == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure, "FAcpakReader::ReadFile: out_buffer is null");
    }
    if (BufferSize < Entry->size_uncompressed) {
        return ACS_ERR(IO, kAcpakSubBufferTooSmall, "FAcpakReader::ReadFile: buffer too small");
    }

    const bool bEncrypted = (m_Flags & static_cast<u32>(AcpakFlagEncrypted)) != 0u;
    const bool bCompressed = (m_Flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;
    const HANDLE Handle = static_cast<HANDLE>(m_FileHandle);
    if (bEncrypted && !m_HasKey) {
        return ACS_ERR(Asset, kAcpakSubCryptoKey, "FAcpakReader::ReadFile: encrypted pak but no key set");
    }

    const usize FinalSize = static_cast<usize>(Entry->size_uncompressed);
    if (static_cast<u64>(FinalSize) != Entry->size_uncompressed) {
        return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::ReadFile: output exceeds address space");
    }
    const usize StoredSize = static_cast<usize>(Entry->size_stored);
    if (static_cast<u64>(StoredSize) != Entry->size_stored) {
        return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::ReadFile: stored data exceeds address space");
    }

    FReadDiagnosticCounters& Diagnostic = m_ReadDiagnosticCounters;
    const u8* const MappedSource = m_MappedView != nullptr
        ? m_MappedView + static_cast<usize>(Entry->offset) : nullptr;

    // 無圧縮・無暗号のマッピング経路は CRC 成功後だけ出力へコピーする。
    if (!bEncrypted && !bCompressed && MappedSource != nullptr) {
        Diagnostic.MappedReadCount.FetchAdd(1u);
        Diagnostic.MappedReadBytes.FetchAdd(Entry->size_stored);
        const u32 ActualCrc = ComputeCrc32(MappedSource, Entry->size_uncompressed);
        if (ActualCrc != Entry->crc32) {
            ACS_LOG_WARN("FAcpakReader::ReadFile: CRC mismatch (expected=0x%08x, actual=0x%08x)", Entry->crc32, ActualCrc);
            return ACS_ERR(Asset, kAcpakSubBadCrc, "FAcpakReader::ReadFile: CRC32 mismatch");
        }
        if (FinalSize > 0u) MemCopy(OutBuffer, MappedSource, FinalSize);
        return TResult<u64>(OkInit, Entry->size_uncompressed);
    }

    const bool NeedStoredScratch = bCompressed && (bEncrypted || MappedSource == nullptr);
    const bool NeedFinalScratch = true;
    const bool RetainEligible = StoredSize <= kAcpakRetainedScratchMaxBytes && FinalSize <= kAcpakRetainedScratchMaxBytes;

    TArray<u8> LocalStored(*m_StringPool.GetAllocator());
    TArray<u8> LocalFinal(*m_StringPool.GetAllocator());
    TArray<u8>* Stored = &LocalStored;
    TArray<u8>* Final = &LocalFinal;
    FMutex* RetainedLock = nullptr;
    if (RetainEligible && m_ScratchLock.TryLock()) {
        RetainedLock = &m_ScratchLock;
        Stored = &m_StoredScratch;
        Final = &m_FinalScratch;
        const bool Reused = (!NeedStoredScratch || Stored->Capacity() >= StoredSize) && (!NeedFinalScratch || Final->Capacity() >= FinalSize);
        if (Reused) Diagnostic.ScratchReuseCount.FetchAdd(1u);
    } else {
        Diagnostic.ScratchFallbackCount.FetchAdd(1u);
    }

    /** 保持一時領域を全 return 経路で返す。 */
    struct FRetainedScratchGuard {
        FMutex* Lock = nullptr;
        ~FRetainedScratchGuard() noexcept
        {
            if (Lock != nullptr) Lock->Unlock();
        }
    } ScratchGuard{RetainedLock};

    if (NeedStoredScratch && !Stored->TryResize(StoredSize)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::ReadFile: intermediate allocation failed");
    }
    if (!Final->TryResize(FinalSize)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakReader::ReadFile: transactional output allocation failed");
    }

    u8* const FinalDestination = Final->IsEmpty() ? nullptr : Final->Data();
    u8* MutableStored = nullptr;
    const u8* StoredSource = nullptr;
    if (bCompressed) {
        if (Entry->size_stored > 0xFFFFFFFFu) {
            return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::ReadFile: stored size > 4GiB (LZ4 limit)");
        }
        if (NeedStoredScratch) {
            MutableStored = Stored->IsEmpty() ? nullptr : Stored->Data();
            StoredSource = MutableStored;
        } else {
            StoredSource = MappedSource;
        }
    } else {
        MutableStored = FinalDestination;
        StoredSource = FinalDestination;
    }

    if (Entry->size_stored > 0u) {
        if (MappedSource != nullptr) {
            Diagnostic.MappedReadCount.FetchAdd(1u);
            Diagnostic.MappedReadBytes.FetchAdd(Entry->size_stored);
            if (MutableStored != nullptr) {
                MemCopy(MutableStored, MappedSource, StoredSize);
            }
        } else {
            DWORD Error = 0u;
            if (!ReadAt(Handle, Entry->offset, MutableStored, Entry->size_stored, Error, m_IoLock)) {
                return ACS_ERR_OS(IO, kAcpakSubIOFailure, "FAcpakReader::ReadFile: ReadFile (data) failed", Error);
            }
            Diagnostic.BufferedReadCount.FetchAdd(1u);
            Diagnostic.BufferedReadBytes.FetchAdd(Entry->size_stored);
        }
    }

    // 空の暗号化 entry でも tag 検証を行う。
    if (bEncrypted) {
        const auto DecryptResult = FAcpakCrypto::Decrypt(m_Key, Entry->cipher_nonce, Entry->cipher_tag, MutableStored, Entry->size_stored, MutableStored);
        if (DecryptResult.IsErr()) {
            return DecryptResult.Error();
        }
        StoredSource = MutableStored;
    }

    if (bCompressed && Entry->size_uncompressed > 0) {
        if (Entry->size_uncompressed > 0xFFFFFFFFu) {
            return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::ReadFile: uncompressed size > 4GiB (LZ4 limit)");
        }
        const auto DecompressResult = FAcpakLz4::Decompress(StoredSource, static_cast<u32>(Entry->size_stored), FinalDestination, static_cast<u32>(Entry->size_uncompressed));
        if (DecompressResult.IsErr()) {
            return DecompressResult.Error();
        }
        if (static_cast<u64>(DecompressResult.Value()) != Entry->size_uncompressed) {
            return ACS_ERR(Asset, kAcpakSubBadSize, "FAcpakReader::ReadFile: LZ4 decompressed size mismatch");
        }
    }

    const u32 ActualCrc = ComputeCrc32(FinalDestination, Entry->size_uncompressed);
    if (ActualCrc != Entry->crc32) {
        ACS_LOG_WARN("FAcpakReader::ReadFile: CRC mismatch (expected=0x%08x, actual=0x%08x)", Entry->crc32, ActualCrc);
        return ACS_ERR(Asset, kAcpakSubBadCrc, "FAcpakReader::ReadFile: CRC32 mismatch");
    }

    if (FinalSize > 0u) MemCopy(OutBuffer, FinalDestination, FinalSize);
    return TResult<u64>(OkInit, Entry->size_uncompressed);
}

TResult<u64> FAcpakReader::ReadFiles(const wchar_t* const* Paths, void* const* OutBuffers, const u64* BufferSizes, u32 Count, u32* CompletedCount) noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (CompletedCount != nullptr) *CompletedCount = 0u;
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "FAcpakReader::ReadFiles: pak not open");
    }
    if (Count > kAcpakReadBatchMaxEntries || (Count > 0u && (Paths == nullptr || OutBuffers == nullptr || BufferSizes == nullptr))) {
        return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::ReadFiles: invalid batch");
    }

    FReadDiagnosticCounters& Diagnostic = m_ReadDiagnosticCounters;
    Diagnostic.BatchCount.FetchAdd(1u);
    Diagnostic.BatchEntryCount.FetchAdd(Count);

    u64 TotalBytes = 0u;
    for (u32 Index = 0u; Index < Count; ++Index) {
        const FAcpakFileEntry* const Entry = FindEntryUnlocked(Paths[Index]);
        if (Entry == nullptr) {
            return ACS_ERR(IO, kAcpakSubNotFound, "FAcpakReader::ReadFiles: path not found");
        }
        if (TotalBytes > static_cast<u64>(-1) - Entry->size_uncompressed) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakReader::ReadFiles: byte count overflow");
        }
        const auto Result = ReadEntryUnlocked(*Entry, OutBuffers[Index], BufferSizes[Index]);
        if (Result.IsErr()) return Result.Error();
        TotalBytes += Result.Value();
        if (CompletedCount != nullptr) *CompletedCount = Index + 1u;
    }
    return TResult<u64>(OkInit, TotalBytes);
}

FAcpakReadDiagnostics FAcpakReader::ReadDiagnostics() const noexcept
{
    // 読み取りを短時間止め、全カウンタを同じ完了境界で集約する。
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    FAcpakReadDiagnostics Result{};
    Result.mapped_read_count =
        m_ReadDiagnosticCounters.MappedReadCount.Load(EMemoryOrder::Relaxed);
    Result.mapped_read_bytes =
        m_ReadDiagnosticCounters.MappedReadBytes.Load(EMemoryOrder::Relaxed);
    Result.buffered_read_count =
        m_ReadDiagnosticCounters.BufferedReadCount.Load(EMemoryOrder::Relaxed);
    Result.buffered_read_bytes =
        m_ReadDiagnosticCounters.BufferedReadBytes.Load(EMemoryOrder::Relaxed);
    Result.scratch_reuse_count =
        m_ReadDiagnosticCounters.ScratchReuseCount.Load(EMemoryOrder::Relaxed);
    Result.scratch_fallback_count =
        m_ReadDiagnosticCounters.ScratchFallbackCount.Load(EMemoryOrder::Relaxed);
    Result.batch_count =
        m_ReadDiagnosticCounters.BatchCount.Load(EMemoryOrder::Relaxed);
    Result.batch_entry_count =
        m_ReadDiagnosticCounters.BatchEntryCount.Load(EMemoryOrder::Relaxed);
    {
        FScopedLock ScratchLock(m_ScratchLock);
        Result.retained_scratch_bytes =
            static_cast<u64>(m_StoredScratch.Capacity()) +
            static_cast<u64>(m_FinalScratch.Capacity());
    }
    Result.mapped_view_active = m_MappedView != nullptr;
    return Result;
}

void FAcpakReader::ResetReadDiagnostics() noexcept
{
    // reset 前に開始した読み取りを完了させ、reset 後の増分と混在させない。
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    m_ReadDiagnosticCounters.MappedReadCount.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.MappedReadBytes.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.BufferedReadCount.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.BufferedReadBytes.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.ScratchReuseCount.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.ScratchFallbackCount.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.BatchCount.Store(0u, EMemoryOrder::Relaxed);
    m_ReadDiagnosticCounters.BatchEntryCount.Store(0u, EMemoryOrder::Relaxed);
}

} // namespace acs::assetpack
