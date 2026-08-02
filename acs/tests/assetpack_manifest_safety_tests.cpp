// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "foundation/Move.h"
#include "memory/Memory.h"
#include "platform/FileSystem.h"

using namespace acs;

namespace {

void WriteU32(u8* Destination, u32 Value) noexcept
{
    MemCopy(Destination, &Value, sizeof(Value));
}

void WriteU64(u8* Destination, u64 Value) noexcept
{
    MemCopy(Destination, &Value, sizeof(Value));
}

u64 ReadU64(const u8* Source) noexcept
{
    u64 Value = 0;
    MemCopy(&Value, Source, sizeof(Value));
    return Value;
}

bool EqualBytes(const u8* Left, const u8* Right, usize Size) noexcept
{
    for (usize Index = 0; Index < Size; ++Index) {
        if (Left[Index] != Right[Index]) return false;
    }
    return true;
}

bool WritePack(const wchar_t* Path, const wchar_t* VirtualPath,
               const u8* Payload, usize PayloadSize) noexcept
{
    assetpack::CAcpakWriter Writer;
    if (Writer.Open(Path, assetpack::AcpakFlagNone).IsErr()) return false;
    if (Writer.AddFile(VirtualPath, Payload, PayloadSize).IsErr()) return false;
    return Writer.Finalize().IsOk();
}

class CNamedLoader final : public IAssetLoader {
public:
    explicit CNamedLoader(const char* Extension) noexcept : m_Extension(Extension) {}

    AssetType TypeId() const noexcept override { return ABinaryAsset::StaticType(); }
    const char* Extension() const noexcept override { return m_Extension; }
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        return ACS_ERR(Asset, 999u, "not used");
    }

private:
    const char* m_Extension = nullptr;
};

class CFailAllocator final : public IAllocator {
public:
    void* Alloc(usize, usize, FSourceLoc) noexcept override { return nullptr; }
    void Free(void*) noexcept override {}
};

} // namespace

ACS_TEST(AcpakManifestSafety, FailedOpenPreservesPreviouslyMountedManifest)
{
    constexpr const wchar_t* kValidPath = L"acs_manifest_transaction_valid.acpak";
    constexpr const wchar_t* kInvalidPath = L"acs_manifest_transaction_invalid.acpak";
    constexpr u8 kPayload[] = {3u, 1u, 4u};
    (void)CFileSystem::Delete(kValidPath);
    (void)CFileSystem::Delete(kInvalidPath);

    EXPECT_TRUE(WritePack(kValidPath, L"stable/data.bin", kPayload, sizeof(kPayload)));
    u8 Invalid[assetpack::kAcpakHeaderDiskSize] = {};
    MemCopy(Invalid, assetpack::kAcpakMagic, sizeof(assetpack::kAcpakMagic));
    WriteU32(Invalid + 8u, assetpack::kAcpakVersion);
    WriteU64(Invalid + 24u, assetpack::kAcpakHeaderDiskSize);
    WriteU32(Invalid + 32u, 1u); // v1 の予約領域は 0 でなければならない。
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kInvalidPath, Invalid, sizeof(Invalid)).IsOk());

    assetpack::CAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kValidPath).IsOk());
    const auto BadOpen = Reader.Open(kInvalidPath);
    EXPECT_TRUE(BadOpen.IsErr());
    if (BadOpen.IsErr()) {
        EXPECT_EQ(BadOpen.Error().subcode, assetpack::kAcpakSubBadSchema);
    }
    EXPECT_TRUE(Reader.IsOpen());
    EXPECT_EQ(Reader.FileCount(), 1u);
    u8 ReadBack[sizeof(kPayload)] = {};
    EXPECT_TRUE(Reader.ReadFile(L"stable/data.bin", ReadBack, sizeof(ReadBack)).IsOk());
    EXPECT_TRUE(EqualBytes(ReadBack, kPayload, sizeof(kPayload)));

    Reader.Close();
    (void)CFileSystem::Delete(kValidPath);
    (void)CFileSystem::Delete(kInvalidPath);
}

ACS_TEST(AcpakManifestSafety, RejectsEmbeddedNullAndDuplicateVirtualPaths)
{
    constexpr const wchar_t* kNullPath = L"acs_manifest_embedded_null.acpak";
    constexpr const wchar_t* kDuplicatePath = L"acs_manifest_duplicate.acpak";
    (void)CFileSystem::Delete(kNullPath);
    (void)CFileSystem::Delete(kDuplicatePath);

    // header + 1 data byte + path_len + UTF-16 path + 固定長 entry tail。
    u8 EmbeddedNull[75] = {};
    MemCopy(EmbeddedNull, assetpack::kAcpakMagic, sizeof(assetpack::kAcpakMagic));
    WriteU32(EmbeddedNull + 8u, assetpack::kAcpakVersion);
    WriteU32(EmbeddedNull + 16u, 1u);
    WriteU64(EmbeddedNull + 24u, 37u);
    EmbeddedNull[36] = 0x2Au;
    WriteU32(EmbeddedNull + 37u, 3u);
    const wchar_t BadName[3] = {L'a', L'\0', L'b'};
    MemCopy(EmbeddedNull + 41u, BadName, sizeof(BadName));
    WriteU64(EmbeddedNull + 47u, 36u);
    WriteU64(EmbeddedNull + 55u, 1u);
    WriteU64(EmbeddedNull + 63u, 1u);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kNullPath, EmbeddedNull, sizeof(EmbeddedNull)).IsOk());

    assetpack::CAcpakReader Reader;
    const auto NullResult = Reader.Open(kNullPath);
    EXPECT_TRUE(NullResult.IsErr());
    if (NullResult.IsErr()) {
        EXPECT_EQ(NullResult.Error().subcode, assetpack::kAcpakSubBadPath);
    }

    constexpr u8 kA[] = {1u};
    constexpr u8 kB[] = {2u};
    assetpack::CAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kDuplicatePath, assetpack::AcpakFlagNone).IsOk());
    EXPECT_TRUE(Writer.AddFile(L"a", kA, sizeof(kA)).IsOk());
    EXPECT_TRUE(Writer.AddFile(L"b", kB, sizeof(kB)).IsOk());
    EXPECT_TRUE(Writer.Finalize().IsOk());

    auto BytesResult = CFileSystem::ReadAllBytes(kDuplicatePath);
    EXPECT_TRUE(BytesResult.IsOk());
    if (BytesResult.IsOk()) {
        TArray<byte>& Bytes = BytesResult.Value();
        const u64 TableOffset = ReadU64(Bytes.Data() + 24u);
    // 先頭の len=1 entry は 34 byte。2 個目の 1 文字 path を書き換える。
        Bytes[static_cast<usize>(TableOffset) + 34u + 4u] = static_cast<byte>('a');
        Bytes[static_cast<usize>(TableOffset) + 34u + 5u] = 0;
        EXPECT_TRUE(CFileSystem::WriteAllBytes(
            kDuplicatePath, Bytes.Data(), Bytes.Size()).IsOk());
    }

    const auto DuplicateResult = Reader.Open(kDuplicatePath);
    EXPECT_TRUE(DuplicateResult.IsErr());
    if (DuplicateResult.IsErr()) {
        EXPECT_EQ(DuplicateResult.Error().subcode, assetpack::kAcpakSubDuplicatePath);
    }

    (void)CFileSystem::Delete(kNullPath);
    (void)CFileSystem::Delete(kDuplicatePath);
}

ACS_TEST(AcpakManifestSafety, WriterRejectsUnsafeNamesWithoutMutatingPendingState)
{
    constexpr const wchar_t* kPath = L"acs_manifest_writer_validation.acpak";
    constexpr u8 kPayload[] = {8u, 6u, 7u, 5u};
    (void)CFileSystem::Delete(kPath);

    assetpack::CAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagNone).IsOk());
    EXPECT_TRUE(Writer.AddFile(L"safe/data.bin", kPayload, sizeof(kPayload)).IsOk());

    const auto Duplicate = Writer.AddFile(L"safe/data.bin", kPayload, sizeof(kPayload));
    EXPECT_TRUE(Duplicate.IsErr());
    if (Duplicate.IsErr()) {
        EXPECT_EQ(Duplicate.Error().subcode, assetpack::kAcpakSubDuplicatePath);
    }
    const auto Traversal = Writer.AddFile(L"safe/../escape.bin", kPayload, sizeof(kPayload));
    EXPECT_TRUE(Traversal.IsErr());
    if (Traversal.IsErr()) {
        EXPECT_EQ(Traversal.Error().subcode, assetpack::kAcpakSubBadPath);
    }
    EXPECT_TRUE(Writer.Finalize().IsOk());

    assetpack::CAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kPath).IsOk());
    EXPECT_EQ(Reader.FileCount(), 1u);
    Reader.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AcpakManifestSafety, AtomicFinalizePreservesOpenReaderSnapshot)
{
    constexpr const wchar_t* kPath = L"acs_manifest_atomic_replace.acpak";
    constexpr u8 kOldPayload[] = {1u, 2u, 3u};
    constexpr u8 kNewPayload[] = {9u, 8u, 7u};
    (void)CFileSystem::Delete(kPath);
    EXPECT_TRUE(WritePack(kPath, L"data.bin", kOldPayload, sizeof(kOldPayload)));

    assetpack::CAcpakReader OldReader;
    EXPECT_TRUE(OldReader.Open(kPath).IsOk());
    EXPECT_TRUE(WritePack(kPath, L"data.bin", kNewPayload, sizeof(kNewPayload)));

    u8 OldRead[sizeof(kOldPayload)] = {};
    EXPECT_TRUE(OldReader.ReadFile(L"data.bin", OldRead, sizeof(OldRead)).IsOk());
    EXPECT_TRUE(EqualBytes(OldRead, kOldPayload, sizeof(kOldPayload)));

    assetpack::CAcpakReader NewReader;
    EXPECT_TRUE(NewReader.Open(kPath).IsOk());
    u8 NewRead[sizeof(kNewPayload)] = {};
    EXPECT_TRUE(NewReader.ReadFile(L"data.bin", NewRead, sizeof(NewRead)).IsOk());
    EXPECT_TRUE(EqualBytes(NewRead, kNewPayload, sizeof(kNewPayload)));

    OldReader.Close();
    NewReader.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AcpakManifestSafety, AbortedWriterLeavesExistingArchiveUntouched)
{
    constexpr const wchar_t* kPath = L"acs_manifest_abort_preserves_original.acpak";
    constexpr u8 kPayload[] = {4u, 2u};
    (void)CFileSystem::Delete(kPath);
    EXPECT_TRUE(WritePack(kPath, L"original.bin", kPayload, sizeof(kPayload)));

    {
        assetpack::CAcpakWriter Writer;
        EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagNone).IsOk());
        EXPECT_TRUE(Writer.AddFile(L"replacement.bin", kPayload, sizeof(kPayload)).IsOk());
    // Finalize せず、デストラクタが一意な一時ファイルだけを削除することを確認する。
    }

    assetpack::CAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kPath).IsOk());
    EXPECT_TRUE(Reader.FindEntry(L"original.bin") != nullptr);
    EXPECT_TRUE(Reader.FindEntry(L"replacement.bin") == nullptr);
    Reader.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AssetRegistrySafety, CheckedLoaderRegistrationClassifiesFailures)
{
    CNamedLoader Valid("safe_ext");
    CNamedLoader Duplicate("safe_ext");
    CNamedLoader Invalid("UPPER");
    CAssetRegistry Registry;

    EXPECT_TRUE(Registry.TryRegisterLoader(&Valid).IsOk());
    const auto DuplicateResult = Registry.TryRegisterLoader(&Duplicate);
    EXPECT_TRUE(DuplicateResult.IsErr());
    if (DuplicateResult.IsErr()) {
        EXPECT_EQ(DuplicateResult.Error().subcode, kAssetRegistrySubDuplicateLoader);
    }
    const auto InvalidResult = Registry.TryRegisterLoader(&Invalid);
    EXPECT_TRUE(InvalidResult.IsErr());
    if (InvalidResult.IsErr()) {
        EXPECT_EQ(InvalidResult.Error().subcode, kAssetRegistrySubInvalidExtension);
    }

    CFailAllocator Allocator;
    CAssetRegistry OomRegistry(Allocator);
    const auto OomResult = OomRegistry.TryRegisterLoader(&Valid);
    EXPECT_TRUE(OomResult.IsErr());
    if (OomResult.IsErr()) {
        EXPECT_EQ(OomResult.Error().subcode, kAssetRegistrySubOutOfMemory);
    }
}

ACS_TEST(AssetRegistrySafety, RejectsOverlongPathWithoutAsyncTruncation)
{
    wchar_t Path[kAssetRegistryMaxPathLength + 2u] = {};
    for (usize Index = 0; Index < kAssetRegistryMaxPathLength + 1u; ++Index) {
        Path[Index] = L'a';
    }

    CAssetRegistry Registry;
    const auto SyncResult = Registry.Load(Path);
    EXPECT_TRUE(SyncResult.IsErr());
    if (SyncResult.IsErr()) {
        EXPECT_EQ(SyncResult.Error().subcode, kAssetRegistrySubPathTooLong);
    }

    FAssetFuture Future = Registry.LoadAsync(Path);
    EXPECT_TRUE(Future.Valid());
    EXPECT_TRUE(Future.IsReady());
    const auto AsyncResult = Future.Get();
    EXPECT_TRUE(AsyncResult.IsErr());
    if (AsyncResult.IsErr()) {
        EXPECT_EQ(AsyncResult.Error().subcode, kAssetRegistrySubPathTooLong);
    }
}
