// SPDX-License-Identifier: Apache-2.0
// SaveArchive / TSaveSlot 永続化境界の堅牢化テスト。

#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Platform.h"
#include "gameframework/SaveArchive.h"
#include "gameframework/SaveSlot.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr u16 SaveSub(ESaveArchiveSubCode code) noexcept {
    return static_cast<u16>(static_cast<u32>(code));
}

class FAlwaysFailAllocator final : public FAllocator {
public:
    void* Alloc(usize, usize, FSourceLoc) noexcept override { return nullptr; }
    void Free(void*) noexcept override {}
};

struct FTempSavePath {
    explicit FTempSavePath(const wchar_t* tag) noexcept {
        usize pos = ::GetTempPathW(MAX_PATH, Path);
        Append(pos, L"acs_savegame_v2_");
        AppendU32(pos, static_cast<u32>(::GetCurrentProcessId()));
        Append(pos, L"_");
        Append(pos, tag);
        Append(pos, L".acssave");
        Path[pos] = L'\0';
        ::DeleteFileW(Path);
    }

    ~FTempSavePath() noexcept { ::DeleteFileW(Path); }

    void Append(usize& pos, const wchar_t* text) noexcept {
        while (*text != L'\0' && pos + 1u < Capacity()) {
            Path[pos++] = *text++;
        }
    }

    void AppendU32(usize& pos, u32 value) noexcept {
        wchar_t reversed[10] = {};
        usize count = 0;
        do {
            reversed[count++] =
                static_cast<wchar_t>(L'0' + (value % 10u));
            value /= 10u;
        } while (value != 0u);
        while (count > 0u && pos + 1u < Capacity()) {
            Path[pos++] = reversed[--count];
        }
    }

    static constexpr usize Capacity() noexcept {
        return MAX_PATH + 128u;
    }

    wchar_t Path[MAX_PATH + 128] = {};
};

bool PatchBytes(
    const wchar_t* path, u64 offset, const void* bytes, u32 size) noexcept {
    HANDLE file = ::CreateFileW(
        path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    bool ok =
        ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;
    DWORD written = 0;
    if (ok) {
        ok = ::WriteFile(file, bytes, size, &written, nullptr) != 0 &&
             written == size;
    }
    ::CloseHandle(file);
    return ok;
}

bool PatchByte(const wchar_t* path, u64 offset, u8 value) noexcept {
    return PatchBytes(path, offset, &value, 1u);
}

void WriteU64LE(u8* bytes, u64 value) noexcept {
    for (u32 i = 0; i < 8u; ++i) {
        bytes[i] = static_cast<u8>((value >> (8u * i)) & 0xFFu);
    }
}

bool AppendByte(const wchar_t* path, u8 value) noexcept {
    HANDLE file = ::CreateFileW(
        path, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok =
        ::WriteFile(file, &value, 1u, &written, nullptr) != 0 &&
        written == 1u;
    ::CloseHandle(file);
    return ok;
}

bool MakeLegacyTempPath(
    const wchar_t* path, wchar_t* out, usize capacity) noexcept {
    usize pos = 0;
    while (path[pos] != L'\0' && pos + 1u < capacity) {
        out[pos] = path[pos];
        ++pos;
    }
    if (path[pos] != L'\0') return false;

    constexpr wchar_t suffix[] = L".tmp.";
    for (usize i = 0; suffix[i] != L'\0'; ++i) {
        if (pos + 1u >= capacity) return false;
        out[pos++] = suffix[i];
    }

    const auto append_u32 =
        [out, capacity](usize& write_pos, u32 value) noexcept {
            wchar_t digits[10] = {};
            usize count = 0;
            do {
                digits[count++] =
                    static_cast<wchar_t>(L'0' + (value % 10u));
                value /= 10u;
            } while (value != 0u);
            if (count >= capacity - write_pos) return false;
            while (count > 0u) out[write_pos++] = digits[--count];
            return true;
        };

    if (!append_u32(
            pos, static_cast<u32>(::GetCurrentProcessId()))) {
        return false;
    }
    if (pos + 1u >= capacity) return false;
    out[pos++] = L'.';
    if (!append_u32(
            pos, static_cast<u32>(::GetCurrentThreadId()))) {
        return false;
    }
    out[pos] = L'\0';
    return true;
}

struct FSlotPayload {
    u32 Score = 0;
    u8 Name[8] = {};
};

} // namespace

ACS_TEST(SaveGameArchiveSafetyV2, ValidateFileReturnsVerifiedMetadata) {
    FTempSavePath path(L"validate_metadata");
    const u8 payload[] = {0x10u, 0x00u, 0x20u, 0x00u, 0x30u};
    EXPECT_TRUE(
        FSaveArchive::WriteToFile(
            path.Path, 42u, payload, sizeof(payload)).IsOk());

    const auto result = FSaveArchive::ValidateFile(path.Path);
    EXPECT_TRUE(result.IsOk());
    if (result.IsOk()) {
        EXPECT_EQ(result.Value().Version, 42u);
        EXPECT_EQ(
            result.Value().PayloadSize,
            static_cast<u64>(sizeof(payload)));
        EXPECT_TRUE(result.Value().PayloadCrc32 != 0u);
    }
}

ACS_TEST(SaveGameArchiveSafetyV2, ValidateFileRejectsCrcAndTrailingData) {
    FTempSavePath crc_path(L"validate_crc");
    const u8 payload[] = {1u, 2u, 3u, 4u};
    EXPECT_TRUE(
        FSaveArchive::WriteToFile(
            crc_path.Path, 1u, payload, sizeof(payload)).IsOk());
    EXPECT_TRUE(PatchByte(
        crc_path.Path, FSaveArchive::kHeaderSize, 0xFEu));

    const auto crc_result = FSaveArchive::ValidateFile(crc_path.Path);
    EXPECT_TRUE(crc_result.IsErr());
    if (crc_result.IsErr()) {
        EXPECT_EQ(
            crc_result.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubChecksumFail));
    }

    FTempSavePath trailing_path(L"validate_trailing");
    EXPECT_TRUE(
        FSaveArchive::WriteToFile(
            trailing_path.Path, 1u, payload, sizeof(payload)).IsOk());
    EXPECT_TRUE(AppendByte(trailing_path.Path, 0xCCu));
    const auto trailing_result =
        FSaveArchive::ValidateFile(trailing_path.Path);
    EXPECT_TRUE(trailing_result.IsErr());
    if (trailing_result.IsErr()) {
        EXPECT_EQ(
            trailing_result.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubSizeMismatch));
    }
}

ACS_TEST(SaveGameArchiveSafetyV2, ValidateFileRejectsOversizedDeclaration) {
    FTempSavePath path(L"validate_oversized");
    const u8 payload = 0x5Au;
    EXPECT_TRUE(
        FSaveArchive::WriteToFile(
            path.Path, 1u, &payload, sizeof(payload)).IsOk());

    u8 encoded_size[8] = {};
    WriteU64LE(
        encoded_size, FSaveArchive::kMaxPayloadSize + 1u);
    EXPECT_TRUE(PatchBytes(path.Path, 12u, encoded_size, 8u));

    const auto result = FSaveArchive::ValidateFile(path.Path);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(
            result.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubPayloadTooLarge));
    }
}

ACS_TEST(SaveGameArchiveSafetyV2, StaleTempCollisionRetriesSafely) {
    FTempSavePath path(L"stale_temp");
    wchar_t stale_path[MAX_PATH + 192] = {};
    EXPECT_TRUE(MakeLegacyTempPath(
        path.Path, stale_path,
        sizeof(stale_path) / sizeof(stale_path[0])));
    ::DeleteFileW(stale_path);

    HANDLE stale = ::CreateFileW(
        stale_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(stale != INVALID_HANDLE_VALUE);
    if (stale != INVALID_HANDLE_VALUE) {
        const u8 marker = 0xA5u;
        DWORD written = 0;
        EXPECT_TRUE(
            ::WriteFile(stale, &marker, 1u, &written, nullptr) != 0);
        EXPECT_EQ(written, 1u);
        EXPECT_TRUE(::CloseHandle(stale) != 0);
    }

    const FSlotPayload payload{73u, {'s', 'a', 'f', 'e', 0u}};
    EXPECT_TRUE(
        FSaveArchive::WriteToFile(
            path.Path, 3u, &payload, sizeof(payload)).IsOk());
    EXPECT_TRUE(
        ::GetFileAttributesW(stale_path) != INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(FSaveArchive::ValidateFile(path.Path).IsOk());
    EXPECT_TRUE(::DeleteFileW(stale_path) != 0);
}

ACS_TEST(SaveGameArchiveSafetyV2, TryInitOwnsTemporaryPath) {
    FTempSavePath path(L"owned_slot");
    wchar_t temporary[MAX_PATH + 128] = {};
    usize i = 0;
    for (; path.Path[i] != L'\0'; ++i) temporary[i] = path.Path[i];
    temporary[i] = L'\0';

    TSaveSlot<FSlotPayload> slot;
    EXPECT_TRUE(slot.TryInit(temporary).IsOk());
    EXPECT_TRUE(slot.IsPathOwned());
    temporary[0] = L'X';
    EXPECT_TRUE(::lstrcmpW(slot.FilePath(), path.Path) == 0);

    FSlotPayload payload{};
    payload.Score = 991u;
    EXPECT_TRUE(slot.Save(payload, 7u).IsOk());
    const auto loaded = slot.Load(7u);
    EXPECT_TRUE(loaded.IsOk());
    if (loaded.IsOk()) EXPECT_EQ(loaded.Value().Score, 991u);
}

ACS_TEST(SaveGameArchiveSafetyV2, TryInitOomPreservesBorrowedPath) {
    FAlwaysFailAllocator allocator;
    TSaveSlot<FSlotPayload> slot(allocator);
    constexpr wchar_t original[] = L"original.acssave";
    slot.Init(original);
    const wchar_t* const before = slot.FilePath();

    const auto result = slot.TryInit(L"replacement.acssave");
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(
            result.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubAllocationFailed));
    }
    EXPECT_TRUE(slot.FilePath() == before);
    EXPECT_FALSE(slot.IsPathOwned());
}

ACS_TEST(SaveGameArchiveSafetyV2, InvalidPathsFailBeforePayloadAccess) {
    const u8 one_byte = 0x55u;
    const auto empty =
        FSaveArchive::WriteToFile(L"", 1u, &one_byte, 1u);
    EXPECT_TRUE(empty.IsErr());
    if (empty.IsErr()) {
        EXPECT_EQ(
            empty.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubInvalidArgument));
    }

    wchar_t too_long[FSaveArchive::kMaxPathChars + 2u] = {};
    for (usize i = 0; i <= FSaveArchive::kMaxPathChars; ++i) {
        too_long[i] = L'a';
    }
    too_long[FSaveArchive::kMaxPathChars + 1u] = L'\0';

    const auto path_result =
        FSaveArchive::ValidateFile(too_long);
    EXPECT_TRUE(path_result.IsErr());
    if (path_result.IsErr()) {
        EXPECT_EQ(
            path_result.Error().subcode,
            SaveSub(ESaveArchiveSubCode::kSubPathTooLong));
    }
}
