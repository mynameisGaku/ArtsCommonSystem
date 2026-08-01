// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — CAcpakWriter 実装 (Win32 I/O + CRC32 計算)
// -----------------------------------------------------------------------------
// 書き出しレイアウト:
//   1. ヘッダ (36 バイト固定、AcpakFormat.h の kAcpakHeaderDiskSize 参照)
//      ・file_table_offset と file_count は Finalize 後にしか確定できないため、
//        Open 時点ではプレースホルダ 0 で書き、Finalize で再度 seek + 上書き
//        する 2 パス書き込みを行う。
//   2. file 0 の生バイト、file 1 の生バイト、...
//      ・各ファイルの offset / crc32 をオンザフライで計算しながら書く。
//   3. file table (各 entry: path_len(4) + path(path_len*2) + offset(8) +
//      size_uncompressed(8) + size_stored(8) + crc32(4))
//   4. ヘッダ書き戻し (file_count + file_table_offset)
//
// 制約:
//   ・AddFile は path と data を内部 allocator へコピーし、呼び出し側の寿命から分離する。
// =============================================================================
#include "assetpack/AcpakWriter.h"

#include "foundation/EndianSerialization.h"
#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"

#include "assetpack/AcpakCrypto.h" // AES-256-GCM 暗号化
#include "assetpack/AcpakLz4.h"    // LZ4 圧縮

#include <cstddef>
#include <cwchar>

namespace acs::assetpack {

namespace {

/**
 * CRC32 lookup table (poly 0xEDB88320) を遅延構築して返す。
 *
 * @details
 * CAcpakReader.cpp と同じ実装。link 単位を分けているので重複する。256-entry
 * table を最初の呼び出し時に組み立てる (Meyers singleton)。
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

    // C++11 の関数ローカル static 初期化で並行初回呼び出しを直列化する。
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
 * 現在の file pointer 位置 (= これから書く位置 = "tell") を返す。
 *
 * @param h 対象のファイルハンドル。
 * @return 現在の file pointer のオフセット (取得失敗時は 0)。
 */
bool TryTell(HANDLE Handle, u64& Offset, DWORD& Error) noexcept
{
    const LARGE_INTEGER Zero{};
    LARGE_INTEGER Current{};
    if (!::SetFilePointerEx(Handle, Zero, &Current, FILE_CURRENT) ||
        Current.QuadPart < 0) {
        Error = ::GetLastError();
        if (Error == 0) Error = ERROR_SEEK;
        return false;
    }
    Offset = static_cast<u64>(Current.QuadPart);
    Error = 0;
    return true;
}

/**
 * file pointer を指定オフセットに移動する (SetFilePointerEx, FILE_BEGIN)。
 *
 * @param h 対象のファイルハンドル。
 * @param offset 移動先のファイルオフセット。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
 * @return 成功なら true、失敗なら false。
 */
bool SeekTo(HANDLE h, u64 offset, DWORD& err) noexcept
{
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
 * src の size バイトをすべて書き込む (>4GiB を考慮した WriteFile ラッパ)。
 *
 * @details
 * WriteFile は DWORD (32bit) 単位でしか書けないため、4GiB 超はチャンク分割して
 * ループする。size == 0 は何もせず成功を返す。
 * @param h 書き込み対象のファイルハンドル。
 * @param src 書き込むバイト列。
 * @param size src のバイト数。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
 * @return 成功なら true、失敗なら false。
 */
bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept
{
    err = 0;
    if (size == 0) return true;
    const u8* p = static_cast<const u8*>(src);
    u64 remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu) ? 0x7FFFFFFFu : static_cast<DWORD>(remaining);
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
 * u32 を host endian に依存せず 4 バイト LE で書き出す。
 *
 * @param dst 書き込み先 (4 バイト)。
 * @param v 書き込む u32。
 */
void WriteU32LE(u8* dst, u32 v) noexcept
{
    WriteLittleEndian(dst, v);
}

/**
 * u64 を host endian に依存せず 8 バイト LE で書き出す。
 *
 * @param dst 書き込み先 (8 バイト)。
 * @param v 書き込む u64。
 */
void WriteU64LE(u8* dst, u64 v) noexcept
{
    WriteLittleEndian(dst, v);
}

/**
 * 自前 wcslen で NUL 終端 wchar_t 列の長さを返す (依存ヘッダを増やさない)。
 *
 * @param s 長さを測る文字列 (nullptr なら 0)。
 * @return NUL を含まない wchar_t 数。
 */
usize LenWBounded(const wchar_t* Text) noexcept
{
    usize Length = 0;
    if (Text == nullptr) {
        return 0;
    }
    while (Length <= kAcpakMaxPathLength && Text[Length] != L'\0') {
        ++Length;
    }
    return Length;
}

bool EqualPath(const TArray<wchar_t>& Stored, const wchar_t* Path, usize Length) noexcept
{
    if (Stored.Size() != Length + 1u) return false;
    for (usize Index = 0; Index < Length; ++Index) {
        if (Stored[Index] != Path[Index]) return false;
    }
    return true;
}

bool CreateUniqueTemporaryFile(const wchar_t* OutputPath, wchar_t* TemporaryPath,
                               usize TemporaryCapacity, HANDLE& Handle,
                               DWORD& Error) noexcept
{
    static volatile LONG Counter = 0;
    for (u32 Attempt = 0; Attempt < 64u; ++Attempt) {
        const unsigned long Sequence =
            static_cast<unsigned long>(::InterlockedIncrement(&Counter));
        const int Written = ::swprintf_s(
            TemporaryPath, TemporaryCapacity, L"%ls.acstmp.%lu.%lu.%lu",
            OutputPath, static_cast<unsigned long>(::GetCurrentProcessId()),
            static_cast<unsigned long>(::GetCurrentThreadId()), Sequence);
        if (Written <= 0 || static_cast<usize>(Written) >= TemporaryCapacity) {
            Error = ERROR_BUFFER_OVERFLOW;
            return false;
        }
        Handle = ::CreateFileW(TemporaryPath, GENERIC_WRITE | DELETE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (Handle != INVALID_HANDLE_VALUE) {
            Error = 0;
            return true;
        }
        Error = ::GetLastError();
        if (Error != ERROR_FILE_EXISTS && Error != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    Error = ERROR_FILE_EXISTS;
    return false;
}

bool RenameOpenFileAtomically(HANDLE Handle, const wchar_t* AbsoluteOutputPath,
                              DWORD& Error) noexcept
{
    usize OutputLength = 0;
    while (OutputLength <= kAcpakMaxOutputPathLength &&
           AbsoluteOutputPath[OutputLength] != L'\0') {
        ++OutputLength;
    }
    if (OutputLength == 0 || OutputLength > kAcpakMaxOutputPathLength) {
        Error = ERROR_BUFFER_OVERFLOW;
        return false;
    }

    // FILE_SHARE_DELETE で既存 reader が開いている出力先を Windows 上で置換するには、
    // POSIX semantics 付き FileRenameInfoEx (22) が必要になる。reader が削除共有を
    // 明示的に許可していても、MoveFileExW はこの状況で ERROR_ACCESS_DENIED を返す。
    constexpr DWORD kReplaceIfExists = 0x00000001u;
    constexpr DWORD kPosixSemantics = 0x00000002u;
    constexpr usize kNameOffset = offsetof(FILE_RENAME_INFO, FileName);
    alignas(FILE_RENAME_INFO)
        u8 RenameBuffer[kNameOffset +
                        (kAcpakMaxOutputPathLength * sizeof(wchar_t))] = {};
    const DWORD RenameFlags = kReplaceIfExists | kPosixSemantics;
    MemCopy(RenameBuffer, &RenameFlags, sizeof(RenameFlags));

    auto* RenameInfo = reinterpret_cast<FILE_RENAME_INFO*>(RenameBuffer);
    RenameInfo->RootDirectory = nullptr;
    RenameInfo->FileNameLength =
        static_cast<DWORD>(OutputLength * sizeof(wchar_t));
    MemCopy(RenameBuffer + kNameOffset, AbsoluteOutputPath,
            static_cast<usize>(RenameInfo->FileNameLength));

    if (!::SetFileInformationByHandle(
            Handle, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22), RenameInfo,
            static_cast<DWORD>(kNameOffset + RenameInfo->FileNameLength))) {
        Error = ::GetLastError();
        return false;
    }
    Error = 0;
    return true;
}

} // namespace

/** DefaultAllocator で空の Writer を構築する。 */
CAcpakWriter::CAcpakWriter() noexcept : CAcpakWriter(DefaultAllocator())
{
}

/** 指定 allocator で pending list を構築する。 */
CAcpakWriter::CAcpakWriter(IAllocator& Allocator) noexcept : m_Allocator(&Allocator), m_Pending(Allocator)
{
}

/** 破棄時に Close を呼んで後始末する。 */
CAcpakWriter::~CAcpakWriter() noexcept
{
    Close();
}

/** ハンドルを閉じ、ResetState で内部状態をクリアする (多重 Close は no-op)。 */
void CAcpakWriter::Close() noexcept
{
    FScopedLock Lock(m_LifecycleLock);
    if (m_FileHandle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_FileHandle));
        m_FileHandle = nullptr;
    }
    if (m_TemporaryPath[0] != L'\0') {
        (void)::DeleteFileW(m_TemporaryPath);
    }
    ResetState();
}

/** flags / Finalize フラグ / pending list / 鍵情報をクリアする。 */
void CAcpakWriter::ResetState() noexcept
{
    m_Flags = 0;
    m_Finalized = false;
    m_Pending.ReleaseStorage();

    // 鍵 defensive zero
    MemSet(m_Key.bytes, 0, sizeof(m_Key.bytes));
    m_HasKey = false;
    m_OutputPath[0] = L'\0';
    m_TemporaryPath[0] = L'\0';
}

/** 暗号化鍵を内部にコピーし、鍵設定済みフラグを立てる。 */
void CAcpakWriter::SetKey(const FAcpakKey& Key) noexcept
{
    FScopedLock Lock(m_LifecycleLock);
    MemCopy(m_Key.bytes, Key.bytes, sizeof(m_Key.bytes));
    m_HasKey = true;
}

/** 出力ファイルを開き、ヘッダのプレースホルダ (36B) を書き出す。 */
TResult<void> CAcpakWriter::Open(const wchar_t* OutputPath, EAcpakFlags Flags) noexcept
{
    FScopedLock Lock(m_LifecycleLock);
    if (m_FileHandle != nullptr) {
        return ACS_ERR(IO, kAcpakSubAlreadyOpen, "CAcpakWriter::Open: writer already open");
    }
    if (OutputPath == nullptr || OutputPath[0] == L'\0') {
        return ACS_ERR(IO, kAcpakSubBadPath, "CAcpakWriter::Open: output_path is null or empty");
    }

    // encrypted / compressed は対応済。未知 flag bit のみ拒否。
    const u32 KnownFlags = static_cast<u32>(AcpakFlagEncrypted) | static_cast<u32>(AcpakFlagCompressed);
    if ((static_cast<u32>(Flags) & ~KnownFlags) != 0) {
        return ACS_ERR(Asset, kAcpakSubBadFlags, "CAcpakWriter::Open: unknown flag bits");
    }

    usize OutputLength = 0;
    while (OutputLength <= kAcpakMaxOutputPathLength && OutputPath[OutputLength] != L'\0') {
        ++OutputLength;
    }
    if (OutputLength == 0 || OutputLength > kAcpakMaxOutputPathLength) {
        return ACS_ERR(IO, kAcpakSubBadPath, "CAcpakWriter::Open: output_path exceeds limit");
    }
    const DWORD AbsoluteLength = ::GetFullPathNameW(
        OutputPath, static_cast<DWORD>(kAcpakMaxOutputPathLength + 1u),
        m_OutputPath, nullptr);
    if (AbsoluteLength == 0) {
        return ACS_ERR_OS(IO, kAcpakSubBadPath,
                          "CAcpakWriter::Open: output_path resolution failed",
                          ::GetLastError());
    }
    if (AbsoluteLength > kAcpakMaxOutputPathLength) {
        m_OutputPath[0] = L'\0';
        return ACS_ERR(IO, kAcpakSubBadPath,
                       "CAcpakWriter::Open: absolute output_path exceeds limit");
    }

    HANDLE Handle = INVALID_HANDLE_VALUE;
    DWORD CreateError = 0;
    if (!CreateUniqueTemporaryFile(m_OutputPath, m_TemporaryPath,
                                   sizeof(m_TemporaryPath) / sizeof(m_TemporaryPath[0]),
                                   Handle, CreateError)) {
        m_OutputPath[0] = L'\0';
        m_TemporaryPath[0] = L'\0';
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "CAcpakWriter::Open: temporary file creation failed", CreateError);
    }

    m_FileHandle = Handle;
    m_Flags = static_cast<u32>(Flags);
    m_Finalized = false;
    m_Pending.Clear();

    // ---- ヘッダプレースホルダを書く (Finalize で上書きする) -----------------
    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8, kAcpakVersion);
    WriteU32LE(header + 12, m_Flags);
    WriteU32LE(header + 16, 0); // file_count placeholder
    WriteU32LE(header + 20, 0); // padding = 0
    WriteU64LE(header + 24, 0); // file_table_offset placeholder
    WriteU32LE(header + 32, 0); // reserved = 0

    DWORD Error = 0;
    if (!WriteAll(Handle, header, kAcpakHeaderDiskSize, Error)) {
        ::CloseHandle(Handle);
        m_FileHandle = nullptr;
        (void)::DeleteFileW(m_TemporaryPath);
        ResetState();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Open: WriteFile (header) failed", Error);
    }

    return Ok();
}

/** 1 ファイルを pending list に積む (実書き込みは Finalize)。 */
TResult<void> CAcpakWriter::AddFile(const wchar_t* VirtualPath, const void* Data, u64 Size) noexcept
{
    FScopedLock Lock(m_LifecycleLock);
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "CAcpakWriter::AddFile: writer not open");
    }
    if (m_Finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "CAcpakWriter::AddFile: writer already finalized");
    }
    if (VirtualPath == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure, "CAcpakWriter::AddFile: virtual_path is null");
    }
    if (Data == nullptr && Size > 0) {
        return ACS_ERR(IO, kAcpakSubIOFailure, "CAcpakWriter::AddFile: data is null but size > 0");
    }

    if (m_Pending.Size() >= kAcpakMaxFileCount) {
        return ACS_ERR(IO, kAcpakSubBadSize, "CAcpakWriter::AddFile: file count exceeds limit");
    }

    /** NUL を除く仮想パス長。 */
    const usize PathLength = LenWBounded(VirtualPath);
    if (PathLength == 0 || PathLength > kAcpakMaxPathLength || !IsCanonicalAcpakVirtualPath(VirtualPath, PathLength)) {
        return ACS_ERR(IO, kAcpakSubBadPath,
                       "CAcpakWriter::AddFile: invalid virtual path");
    }
    /** 正規形 path に対して AddFile 中に一度だけ計算する hash。 */
    const u64 PathHash = HashCanonicalAcpakVirtualPath(VirtualPath, PathLength);
    /** hash 一致候補だけを完全比較する登録済み entry index。 */
    for (usize Index = 0; Index < m_Pending.Size(); ++Index) {
        if (m_Pending[Index].PathHash == PathHash && EqualPath(m_Pending[Index].Path, VirtualPath, PathLength)) {
            return ACS_ERR(Asset, kAcpakSubDuplicatePath,
                           "CAcpakWriter::AddFile: duplicate virtual path");
        }
    }
    /** 現在の address space で扱う payload byte 数。 */
    const usize PayloadSize = static_cast<usize>(Size);
    if (static_cast<u64>(PayloadSize) != Size) {
        return ACS_ERR(IO, kAcpakSubBadSize, "CAcpakWriter::AddFile: payload exceeds address space");
    }

    /** path と payload を保持する新規 pending entry。 */
    FPendingEntry* Entry = m_Pending.TryEmplaceBack(*m_Allocator);
    if (Entry == nullptr) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::AddFile: pending entry allocation failed");
    }

    if (!Entry->Path.TryResize(PathLength + 1u)) {
        m_Pending.PopBack();
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::AddFile: path copy allocation failed");
    }
    Entry->PathHash = PathHash;
    MemCopy(Entry->Path.Data(), VirtualPath, (PathLength + 1u) * sizeof(wchar_t));

    if (!Entry->Data.TryResize(PayloadSize)) {
        m_Pending.PopBack();
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::AddFile: payload copy allocation failed");
    }
    if (Size > 0) {
        MemCopy(Entry->Data.Data(), Data, static_cast<usize>(Size));
    }
    return Ok();
}

/** pending entry 群をディスクに書き出し、最後にヘッダを更新して pak を確定する。 */
TResult<void> CAcpakWriter::Finalize() noexcept
{
    FScopedLock Lock(m_LifecycleLock);
    // 各 entry に対し compress-then-encrypt パイプラインを通す:
    //   1. (元データ ptr, size) → CRC32 計算 (元バイト基準)
    //   2. compressed 立ち → LZ4 圧縮
    //   3. encrypted 立ち → CSPRNG nonce 生成 → AES-256-GCM 暗号化 (tag 出力)
    //   4. ディスクに stored bytes を書く
    //   5. file table に (offset, size_unc, size_st, crc32, nonce?, tag?) を書く
    //
    // Reader はファイル単位の per-entry "compressed?" 判定を持たず header flags
    // のみ参照するため、compressed flag 立ち時は entry が短くても必ず LZ4 を通し、
    // たとえ size_stored > size_uncompressed でも保存する。圧縮 + 暗号化が両方
    // 立つ場合も、LZ4 で生格納に倒れた entry を含め暗号化は通常通り行う。
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "CAcpakWriter::Finalize: writer not open");
    }
    if (m_Finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen, "CAcpakWriter::Finalize: already finalized");
    }

    const HANDLE h = static_cast<HANDLE>(m_FileHandle);
    DWORD err = 0;

    // ---- 既にヘッダ 36B は Open で書いてあり、file pointer は 36 にある -----
    // 念のため明示的に seek (古いヘッダ書き込みが flush されていない場合に
    // file pointer が想定外位置にいるリスクを下げる)。
    if (!SeekTo(h, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Finalize: SetFilePointerEx failed", err);
    }

    const bool is_encrypted = (m_Flags & static_cast<u32>(AcpakFlagEncrypted)) != 0u;
    const bool is_compressed = (m_Flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;

    // 暗号化が立っているのに鍵未設定なら早期に弾く (失敗のソースを明確化)。
    if (is_encrypted && !m_HasKey) {
        return ACS_ERR(Asset, kAcpakSubCryptoKey, "CAcpakWriter::Finalize: encrypted flag set but no key");
    }

    // 各 entry の (offset, size_unc, size_st, crc32, nonce[12], tag[16]) を控える。
    struct FWrittenEntry {
        u64 Offset;
        u64 UncompressedSize;
        u64 StoredSize;
        u32 Crc32;
        u8 CipherNonce[12];
        u8 CipherTag[16];
    };
    TArray<FWrittenEntry> written(*m_Allocator);
    if (!written.TryResize(m_Pending.Size())) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::Finalize: entry result allocation failed");
    }

    // 中間バッファは entry 間で再利用する (TArray<u8> を 2 つ用意)。
    TArray<u8> stage_compress(*m_Allocator); // LZ4 圧縮出力先
    TArray<u8> stage_encrypt(*m_Allocator);  // AES-GCM 暗号化出力先

    // ---- ファイルデータ書き出し -------------------------------------------
    for (usize i = 0; i < m_Pending.Size(); ++i) {
        const FPendingEntry& p = m_Pending[i];

        FWrittenEntry w{};
        if (!TryTell(h, w.Offset, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "CAcpakWriter::Finalize: tell data offset failed", err);
        }
        w.UncompressedSize = static_cast<u64>(p.Data.Size());
        w.Crc32 = ComputeCrc32(p.Data.Data(), p.Data.Size());

        // ステージごとの (data, size) を pipeline で更新していく。
        const u8* stage_ptr = p.Data.Data();
        u64 stage_size = static_cast<u64>(p.Data.Size());

        // ---- 圧縮 (compress-then-encrypt の 1 段目) --------------------
        if (is_compressed) {
            if (p.Data.Size() > 0xFFFFFFFFu) {
                return ACS_ERR(Asset, kAcpakSubBadSize, "CAcpakWriter::Finalize: input > 4GiB (LZ4 limit)");
            }
            const u32 src_size = static_cast<u32>(p.Data.Size());
            const u32 dst_cap = CAcpakLz4::MaxCompressedSize(src_size);
            if (!stage_compress.TryResize(dst_cap)) {
                return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::Finalize: compression allocation failed");
            }
            const auto cr = CAcpakLz4::Compress(p.Data.Data(), src_size, stage_compress.Data(), dst_cap);
            if (cr.IsErr()) return cr.Error();
            stage_ptr = stage_compress.Data();
            stage_size = cr.Value();
        }

        // ---- 暗号化 (compress-then-encrypt の 2 段目) -------------------
        if (is_encrypted) {
            // CSPRNG nonce 生成。RNG 失敗時はゼロ nonce での AES-GCM 暗号化
            // (= nonce 再利用 → 認証鍵漏洩) を絶対に避けるため、エラーを伝播し
            // 暗号化を中止する。
            const auto nr = CAcpakCrypto::GenerateRandomNonce(w.CipherNonce);
            if (nr.IsErr()) return nr.Error();

            // 出力バッファ (= 同 size の別領域、in-place も可だが分離で安全)
            if (!stage_encrypt.TryResize(static_cast<usize>(stage_size))) {
                return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::Finalize: encryption allocation failed");
            }
            const auto er = CAcpakCrypto::Encrypt(m_Key, w.CipherNonce, stage_ptr, stage_size, stage_encrypt.Data(),
                                                  w.CipherTag);
            if (er.IsErr()) return er.Error();
            stage_ptr = stage_encrypt.Data();
            // size 不変 (AES-GCM は size == size)
        }

        w.StoredSize = stage_size;

        // ---- ディスク書き出し ----------------------------------------
        if (stage_size > 0) {
            if (!WriteAll(h, stage_ptr, stage_size, err)) {
                return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Finalize: WriteFile (data) failed", err);
            }
        }

        written[i] = w;
    }

    // ---- file table 書き出し ---------------------------------------------
    u64 file_table_offset = 0;
    if (!TryTell(h, file_table_offset, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "CAcpakWriter::Finalize: tell manifest offset failed", err);
    }

    for (usize i = 0; i < m_Pending.Size(); ++i) {
        const FPendingEntry& p = m_Pending[i];
        const FWrittenEntry& w = written[i];
        const u32 len = static_cast<u32>(p.Path.Size() - 1u);

        // path_len (4) + path (len*2) + offset (8) + size_unc (8) + size_st (8)
        // + crc32 (4) を 1 つの一時バッファに詰めて書く。
        // 暗号化のとき末尾に cipher_nonce(12) + cipher_tag(16) を続ける。
        const u64 path_bytes = static_cast<u64>(len) * sizeof(wchar_t);
        const u64 tail_bytes = is_encrypted ? (28u + kAcpakCipherFieldsDiskSize) : 28u;
        const u64 entry_bytes = 4u + path_bytes + tail_bytes;

        u8 stack_buf[1024];
        u8* buf = stack_buf;
        TArray<u8> heap_buf(*m_Allocator);
        if (entry_bytes > sizeof(stack_buf)) {
            if (!heap_buf.TryResize(static_cast<usize>(entry_bytes))) {
                return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakWriter::Finalize: table entry allocation failed");
            }
            buf = heap_buf.Data();
        }

        WriteU32LE(buf + 0, len);
        if (path_bytes > 0) {
            MemCopy(buf + 4, p.Path.Data(), static_cast<usize>(path_bytes));
        }
        u8* const tail = buf + 4 + path_bytes;
        WriteU64LE(tail + 0, w.Offset);
        WriteU64LE(tail + 8, w.UncompressedSize);
        WriteU64LE(tail + 16, w.StoredSize);
        WriteU32LE(tail + 24, w.Crc32);
        if (is_encrypted) {
            MemCopy(tail + 28, w.CipherNonce, 12);
            MemCopy(tail + 28 + 12, w.CipherTag, 16);
        }

        if (!WriteAll(h, buf, entry_bytes, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Finalize: WriteFile (entry) failed", err);
        }
    }

    // ---- ヘッダ書き戻し (file_count + file_table_offset を確定) ------------
    // Finalize の再試行や短い table への変更でも古い末尾を残さない。
    if (!::SetEndOfFile(h)) {
        err = ::GetLastError();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "CAcpakWriter::Finalize: SetEndOfFile failed", err);
    }

    if (!SeekTo(h, 0, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Finalize: SetFilePointerEx (rewind) failed", err);
    }

    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8, kAcpakVersion);
    WriteU32LE(header + 12, m_Flags);
    WriteU32LE(header + 16, static_cast<u32>(m_Pending.Size()));
    WriteU32LE(header + 20, 0); // padding = 0
    WriteU64LE(header + 24, file_table_offset);
    WriteU32LE(header + 32, 0); // reserved = 0

    if (!WriteAll(h, header, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure, "CAcpakWriter::Finalize: WriteFile (header rewrite) failed", err);
    }

    // ディスクに反映 (クラッシュ耐性は完璧ではないが、最低限 flush 要求)
    if (!::FlushFileBuffers(h)) {
        err = ::GetLastError();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "CAcpakWriter::Finalize: FlushFileBuffers failed", err);
    }
    if (!RenameOpenFileAtomically(h, m_OutputPath, err)) {
        return ACS_ERR_OS(IO, kAcpakSubAtomicReplace,
                          "CAcpakWriter::Finalize: atomic replace failed", err);
    }

    if (!::CloseHandle(h)) {
        err = ::GetLastError();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "CAcpakWriter::Finalize: CloseHandle failed", err);
    }
    m_FileHandle = nullptr;

    m_TemporaryPath[0] = L'\0';
    m_Finalized = true;
    return Ok();
}

} // namespace acs::assetpack
