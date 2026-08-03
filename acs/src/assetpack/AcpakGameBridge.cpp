// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#include "assetpack/AcpakGameBridge.h"

#include "foundation/Error.h"
#include "threading/ScopedLock.h"

#include <Windows.h>

namespace acs::assetpack {

namespace {

/** 入力が妥当な UTF-8 でない。 */
inline constexpr u16 kSubAcpakBridgeBadUtf8 = 1391;

/** 変換後の文字列が許容長を超える。 */
inline constexpr u16 kSubAcpakBridgePathTooLong = 1392;

/** 引数が null / 空文字列など不正。 */
inline constexpr u16 kSubAcpakBridgeBadArgument = 1393;

/** UTF-8 名 pool の防御的な最大バイト数。 */
inline constexpr usize kAcpakMaxUtf8NamePoolBytes = kAcpakMaxPathPoolBytes * 2u;

/** NUL を含む UTF-16 変換先要素数を検証して返す。 */
TResult<u32> RequiredWideUnits(const char* Text) noexcept
{
    if (Text == nullptr || Text[0] == 0) {
        return TResult<u32>(ACS_ERR(IO, kSubAcpakBridgeBadArgument, "Acpak bridge path is empty"));
    }
    /** 末尾 NUL を含む UTF-16 変換先要素数。 */
    const int Required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, -1, nullptr, 0);
    if (Required <= 0) {
        return TResult<u32>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "Acpak bridge path is not valid UTF-8"));
    }
    return TResult<u32>(OkInit, static_cast<u32>(Required));
}

/** UTF-8 文字列を呼び出し側所有の UTF-16 バッファへ変換する。 */
TResult<const wchar_t*> ConvertUtf8ToWide(const char* Text, wchar_t* Out, u32 OutCapacity) noexcept
{
    if (Out == nullptr || OutCapacity == 0u) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadArgument, "Acpak bridge path is empty"));
    }

    /** UTF-16 変換先要素数の検証結果。 */
    const auto RequiredResult = RequiredWideUnits(Text);
    if (RequiredResult.IsErr()) {
        return TResult<const wchar_t*>(RequiredResult.Error());
    }
    /** 末尾 NUL を含む UTF-16 変換先要素数。 */
    const u32 Required = RequiredResult.Value();
    if (Required > OutCapacity) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgePathTooLong, "Acpak bridge path is too long"));
    }

    /** 実際に変換した UTF-16 要素数。 */
    const int Written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, -1, Out, static_cast<int>(Required));
    if (Written <= 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "Acpak bridge path conversion failed"));
    }
    return TResult<const wchar_t*>(OkInit, Out);
}

} // namespace

CAcpakGameReader::CAcpakGameReader() noexcept : CAcpakGameReader(DefaultAllocator())
{
}

CAcpakGameReader::CAcpakGameReader(IAllocator& Allocator) noexcept
    : m_Reader(Allocator), m_Utf8NamePool(Allocator), m_Utf8NameOffsets(Allocator)
{
}

CAcpakGameReader::~CAcpakGameReader() noexcept
{
    Unmount();
}

TResult<void> CAcpakGameReader::BuildFileNamePool() noexcept
{
    const u32 FileCount = m_Reader.FileCount();
    if (!m_Utf8NameOffsets.TrySetNum(FileCount)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakGameReader: file name offset allocation failed");
    }

    usize TotalBytes = 0;
    for (u32 Index = 0; Index < FileCount; ++Index) {
        const FAcpakFileEntry* Entry = m_Reader.GetEntry(Index);
        if (Entry == nullptr) {
            return ACS_ERR(IO, kAcpakSubBadSize, "CAcpakGameReader: entry disappeared while mounting");
        }

        const int Required = ::WideCharToMultiByte(CP_UTF8, 0, Entry->path, -1, nullptr, 0, nullptr, nullptr);
        if (Required <= 0) {
            return ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "CAcpakGameReader: file name conversion failed");
        }
        const usize RequiredBytes = static_cast<usize>(Required);
        if (RequiredBytes > kAcpakMaxUtf8NamePoolBytes - TotalBytes) {
            return ACS_ERR(IO, kAcpakSubBadSize, "CAcpakGameReader: UTF-8 file name pool exceeds limit");
        }
        m_Utf8NameOffsets[Index] = TotalBytes;
        TotalBytes += RequiredBytes;
    }

    if (!m_Utf8NamePool.TrySetNum(TotalBytes)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakGameReader: file name pool allocation failed");
    }

    for (u32 Index = 0; Index < FileCount; ++Index) {
        const FAcpakFileEntry* Entry = m_Reader.GetEntry(Index);
        char* Destination = m_Utf8NamePool.GetData() + m_Utf8NameOffsets[Index];
        const usize AvailableBytes = TotalBytes - m_Utf8NameOffsets[Index];
        if (Entry == nullptr) {
            return ACS_ERR(IO, kAcpakSubBadSize, "CAcpakGameReader: invalid file name pool layout");
        }

        const int Written = ::WideCharToMultiByte(CP_UTF8, 0, Entry->path, -1, Destination,
                                                  static_cast<int>(AvailableBytes), nullptr, nullptr);
        if (Written <= 0) {
            return ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "CAcpakGameReader: file name conversion failed");
        }
    }

    return Ok();
}

void CAcpakGameReader::ReleaseFileNamePool() noexcept
{
    m_Utf8NameOffsets.Empty();
    m_Utf8NamePool.Empty();
}

TResult<void> CAcpakGameReader::Mount(const char* PackPath) noexcept
{
    FScopedExclusiveLock Lock(m_LifecycleLock);
    m_Reader.Close();
    ReleaseFileNamePool();

    wchar_t WidePath[kPathCapacity] = {};
    const auto ConvertedPath = ConvertUtf8ToWide(PackPath, WidePath, kPathCapacity);
    if (ConvertedPath.IsErr()) {
        return ConvertedPath.Error();
    }

    const auto OpenResult = m_Reader.Open(ConvertedPath.Value());
    if (OpenResult.IsErr()) {
        return OpenResult.Error();
    }

    const auto PoolResult = BuildFileNamePool();
    if (PoolResult.IsErr()) {
        m_Reader.Close();
        ReleaseFileNamePool();
        return PoolResult.Error();
    }
    return Ok();
}

void CAcpakGameReader::Unmount() noexcept
{
    FScopedExclusiveLock Lock(m_LifecycleLock);
    m_Reader.Close();
    ReleaseFileNamePool();
}

bool CAcpakGameReader::IsMounted() const noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    return m_Reader.IsOpen();
}

TResult<u32> CAcpakGameReader::FileCount() noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (!m_Reader.IsOpen()) {
        return TResult<u32>(ACS_ERR(IO, game::kSubAssetPackNotMounted, "CAcpakGameReader::FileCount before Mount"));
    }
    return TResult<u32>(OkInit, m_Reader.FileCount());
}

TResult<const char*> CAcpakGameReader::FileName(u32 Index) noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    if (!m_Reader.IsOpen()) {
        return TResult<const char*>(
            ACS_ERR(IO, game::kSubAssetPackNotMounted, "CAcpakGameReader::FileName before Mount"));
    }
    if (static_cast<usize>(Index) >= m_Utf8NameOffsets.Num()) {
        return TResult<const char*>(ACS_ERR(IO, kAcpakSubNotFound, "CAcpakGameReader::FileName index not found"));
    }
    return TResult<const char*>(OkInit, m_Utf8NamePool.GetData() + m_Utf8NameOffsets[Index]);
}

TResult<u64> CAcpakGameReader::FileSize(const char* Name) noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(Name, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return TResult<u64>(ConvertedName.Error());
    }
    return m_Reader.GetUncompressedSize(ConvertedName.Value());
}

TResult<void> CAcpakGameReader::ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept
{
    FScopedSharedLock Lock(m_LifecycleLock);
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(Name, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return ConvertedName.Error();
    }
    const auto ReadResult = m_Reader.ReadFile(ConvertedName.Value(), OutBuffer, BufferSize);
    if (ReadResult.IsErr()) {
        return ReadResult.Error();
    }
    return Ok();
}

TResult<void> CAcpakGameReader::ReadFiles(const game::FAssetPackReadRequest* Requests, u32 Count, u32* CompletedCount) noexcept
{
    /** mount 状態と reader を一括処理中に保持する共有 lock。 */
    FScopedSharedLock Lock(m_LifecycleLock);
    if (CompletedCount != nullptr) *CompletedCount = 0u;
    if (Count > kAcpakReadBatchMaxEntries || (Count > 0u && Requests == nullptr)) {
        return ACS_ERR(IO, game::kSubAssetPackInvalidBatch, "CAcpakGameReader::ReadFiles: invalid batch");
    }
    if (Count == 0u) return Ok();

    /** batch の変換配列へ使う既存 allocator。 */
    IAllocator& Allocator = *m_Utf8NamePool.GetAllocator();
    /** 全 path の UTF-16 変換結果を連続保持する pool。 */
    TArray<wchar_t> WideNamePool(Allocator);
    /** reader へ渡す各 UTF-16 path の先頭。 */
    TArray<const wchar_t*> WideNames(Allocator);
    /** reader へ渡す各出力 buffer。 */
    TArray<void*> OutBuffers(Allocator);
    /** reader へ渡す各出力容量。 */
    TArray<u64> BufferSizes(Allocator);
    /** 各 path の末尾 NUL を含む UTF-16 要素数。 */
    TArray<u32> RequiredUnits(Allocator);
    if (!WideNames.TrySetNum(Count) || !OutBuffers.TrySetNum(Count) || !BufferSizes.TrySetNum(Count) || !RequiredUnits.TrySetNum(Count)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakGameReader::ReadFiles: allocation failed");
    }

    /** pool に必要な UTF-16 合計要素数。 */
    usize PathUnits = 0u;
    /** UTF-8 検証まで完了した要求数。 */
    u32 PreparedCount = 0u;
    /** 先行 read 後へ遅延して返す path 変換エラー。 */
    FErrorCode DeferredError{};
    /** 要求配列を検証する添字。 */
    for (u32 Index = 0u; Index < Count; ++Index) {
        /** 現在 path の UTF-16 要素数検証結果。 */
        const auto RequiredResult = RequiredWideUnits(Requests[Index].Name);
        if (RequiredResult.IsErr()) {
            DeferredError = RequiredResult.Error();
            break;
        }
        if (RequiredResult.Value() > kPathCapacity) {
            DeferredError = ACS_ERR(IO, kSubAcpakBridgePathTooLong, "CAcpakGameReader::ReadFiles: path is too long");
            break;
        }
        RequiredUnits[Index] = RequiredResult.Value();
        PathUnits += RequiredUnits[Index];
        OutBuffers[Index] = Requests[Index].OutBuffer;
        BufferSizes[Index] = Requests[Index].BufferSize;
        ++PreparedCount;
    }
    if (!WideNamePool.TrySetNum(PathUnits)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "CAcpakGameReader::ReadFiles: path pool allocation failed");
    }

    /** 次の変換結果を書き込む pool offset。 */
    usize PathOffset = 0u;
    /** UTF-16 変換まで完了した要求数。 */
    u32 ConvertedCount = 0u;
    /** 準備済み path を変換する添字。 */
    for (u32 Index = 0u; Index < PreparedCount; ++Index) {
        /** 現在 path の UTF-16 変換先。 */
        wchar_t* const WideName = WideNamePool.GetData() + PathOffset;
        /** 現在 path で実際に変換した要素数。 */
        const int Written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Requests[Index].Name, -1, WideName, static_cast<int>(RequiredUnits[Index]));
        if (Written != static_cast<int>(RequiredUnits[Index])) {
            DeferredError = ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "CAcpakGameReader::ReadFiles: path conversion failed");
            break;
        }
        WideNames[Index] = WideName;
        PathOffset += RequiredUnits[Index];
        ++ConvertedCount;
    }

    // 後続パス変換エラーより先行 read エラーを優先し、逐次 API と同じ部分完了順を保つ。
    if (ConvertedCount == 0u) return DeferredError;
    /** 呼び出し元が完了数を不要とした場合の受け皿。 */
    u32 IgnoredCompletedCount = 0u;
    /** reader が成功済み要素数を書き戻す先。 */
    u32* const ActualCompletedCount = CompletedCount != nullptr ? CompletedCount : &IgnoredCompletedCount;
    /** 変換済み要求の一括 read 結果。 */
    const auto Result = m_Reader.ReadFiles(WideNames.GetData(), OutBuffers.GetData(), BufferSizes.GetData(), ConvertedCount, ActualCompletedCount);
    if (Result.IsErr()) return Result.Error();
    if (!DeferredError.IsOk()) return DeferredError;
    return Ok();
}

TResult<void> CAcpakGameWriter::BeginPack(const char* OutputPath) noexcept
{
    wchar_t WidePath[kPathCapacity] = {};
    const auto ConvertedPath = ConvertUtf8ToWide(OutputPath, WidePath, kPathCapacity);
    if (ConvertedPath.IsErr()) {
        return ConvertedPath.Error();
    }
    return m_Writer.Open(ConvertedPath.Value(), AcpakFlagNone);
}

TResult<void> CAcpakGameWriter::AddFile(const char* VirtualName, const u8* Data, u64 Size) noexcept
{
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(VirtualName, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return ConvertedName.Error();
    }
    return m_Writer.AddFile(ConvertedName.Value(), Data, Size);
}

TResult<void> CAcpakGameWriter::FinishPack() noexcept
{
    const auto FinishResult = m_Writer.Finalize();
    m_Writer.Close();
    return FinishResult;
}

} // namespace acs::assetpack
