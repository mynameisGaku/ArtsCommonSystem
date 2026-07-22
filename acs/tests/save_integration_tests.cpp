// SPDX-License-Identifier: Apache-2.0
// SaveArchive 上位統合: TSaveSlot / FProgression の transactional contract 回帰テスト
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Platform.h"
#include "gameframework/Progression.h"
#include "gameframework/SaveArchive.h"
#include "gameframework/SaveSlot.h"
#include "memory/Memory.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr u16 SaveSub(ESaveArchiveSubCode code) noexcept
{
    return static_cast<u16>(static_cast<u32>(code));
}

constexpr u16 ProgressionSub(EProgressionPersistenceSubCode code) noexcept
{
    return static_cast<u16>(static_cast<u32>(code));
}

struct FTempSavePath {
    explicit FTempSavePath(const wchar_t* tag) noexcept
    {
        usize pos = ::GetTempPathW(MAX_PATH, m_Path);
        Append(pos, L"acs_save_integration_");
        AppendU32(pos, static_cast<u32>(::GetCurrentProcessId()));
        Append(pos, L"_");
        Append(pos, tag);
        Append(pos, L".acssave");
        m_Path[pos] = L'\0';
        ::DeleteFileW(m_Path);
    }

    ~FTempSavePath() noexcept
    {
        ::DeleteFileW(m_Path);
    }

    const wchar_t* Get() const noexcept { return m_Path; }

private:
    void Append(usize& pos, const wchar_t* text) noexcept
    {
        while (*text != L'\0' && pos + 1 < sizeof(m_Path) / sizeof(m_Path[0])) {
            m_Path[pos++] = *text++;
        }
    }

    void AppendU32(usize& pos, u32 value) noexcept
    {
        wchar_t reversed[10] = {};
        usize count = 0;
        do {
            reversed[count++] = static_cast<wchar_t>(L'0' + (value % 10u));
            value /= 10u;
        } while (value != 0u);
        while (count > 0 && pos + 1 < sizeof(m_Path) / sizeof(m_Path[0])) {
            m_Path[pos++] = reversed[--count];
        }
    }

    wchar_t m_Path[MAX_PATH + 96] = {};
};

void WriteU32LE(u8* p, u32 value) noexcept
{
    p[0] = static_cast<u8>(value & 0xFFu);
    p[1] = static_cast<u8>((value >> 8) & 0xFFu);
    p[2] = static_cast<u8>((value >> 16) & 0xFFu);
    p[3] = static_cast<u8>((value >> 24) & 0xFFu);
}

void WriteU64LE(u8* p, u64 value) noexcept
{
    for (u32 i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>((value >> (i * 8)) & 0xFFull);
    }
}

u32 HashId(const char* id) noexcept
{
    u32 hash = 2166136261u;
    while (*id != '\0') {
        hash ^= static_cast<u32>(static_cast<unsigned char>(*id++));
        hash *= 16777619u;
    }
    return hash;
}

bool PatchFileBytes(const wchar_t* path, u64 offset, const void* bytes, u32 size) noexcept
{
    HANDLE file = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    bool ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;
    if (ok) {
        DWORD written = 0;
        ok = ::WriteFile(file, bytes, size, &written, nullptr) != 0 && written == size;
    }
    ::CloseHandle(file);
    return ok;
}

struct FSlotPayload {
    u32 Score = 0;
    u32 Count = 0;
};

struct FSmallSlotPayload {
    u32 Score = 0;
};

void RegisterTwoMilestones(FProgression& progression) noexcept
{
    progression.RegisterMilestone({"milestone.alpha", "Alpha", 10u, nullptr});
    progression.RegisterMilestone({"milestone.beta", "Beta", 20u, nullptr});
}

} // namespace

ACS_TEST(SaveIntegration, SaveSlotRoundTripAndIdempotentDelete)
{
    FTempSavePath path(L"slot_roundtrip");
    TSaveSlot<FSlotPayload> slot;
    slot.Init(path.Get());

    FSlotPayload saved{};
    saved.Score = 900u;
    saved.Count = 12u;
    EXPECT_TRUE(slot.Save(saved, 3u).IsOk());
    EXPECT_TRUE(slot.Exists());

    const auto loaded = slot.Load(3u);
    EXPECT_TRUE(loaded.IsOk());
    if (loaded.IsOk()) {
        EXPECT_EQ(loaded.Value().Score, saved.Score);
        EXPECT_EQ(loaded.Value().Count, saved.Count);
    }

    EXPECT_TRUE(slot.Delete().IsOk());
    EXPECT_TRUE(slot.Delete().IsOk());
    EXPECT_FALSE(slot.Exists());
}

ACS_TEST(SaveIntegration, SaveSlotSizeMismatchDoesNotChangeDirectHelperOutput)
{
    FTempSavePath path(L"slot_size");
    FSmallSlotPayload file_payload{};
    file_payload.Score = 7u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          &file_payload, sizeof(file_payload)).IsOk());

    FSlotPayload output{};
    output.Score = 0xA5A5A5A5u;
    output.Count = 0x5A5A5A5Au;
    const FSlotPayload before = output;
    const auto result = ::acs::game::detail::SaveSlot_LoadBytes(
        path.Get(), 1u, &output, sizeof(output));
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubBufferTooSmall));
    }
    EXPECT_TRUE(MemCmp(&output, &before, sizeof(output)) == 0);
}

ACS_TEST(SaveIntegration, SaveSlotVersionMismatchIsPropagatedWithoutOutputChange)
{
    FTempSavePath path(L"slot_version");
    FSlotPayload file_payload{};
    file_payload.Score = 55u;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          &file_payload, sizeof(file_payload)).IsOk());

    FSlotPayload output{};
    output.Score = 88u;
    output.Count = 99u;
    const FSlotPayload before = output;
    const auto result = ::acs::game::detail::SaveSlot_LoadBytes(
        path.Get(), 2u, &output, sizeof(output));
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubMigrationNeeded));
    }
    EXPECT_TRUE(MemCmp(&output, &before, sizeof(output)) == 0);
}

ACS_TEST(SaveIntegration, ProgressionRoundTripIsRegistrationOrderIndependent)
{
    FTempSavePath path(L"progression_roundtrip");

    FProgression source;
    RegisterTwoMilestones(source);
    source.AwardXp(15u);
    EXPECT_TRUE(source.Save(path.Get()).IsOk());

    FProgression destination;
    destination.RegisterMilestone({"milestone.beta", "Beta", 20u, nullptr});
    destination.RegisterMilestone({"milestone.alpha", "Alpha", 10u, nullptr});
    destination.AwardXp(30u);
    EXPECT_TRUE(destination.Load(path.Get()).IsOk());

    EXPECT_EQ(destination.CurrentXp(), 15u);
    EXPECT_TRUE(destination.IsMilestoneAchieved("milestone.alpha"));
    EXPECT_FALSE(destination.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionVersionMismatchPreservesCurrentState)
{
    FTempSavePath path(L"progression_version");
    FProgression file_state;
    RegisterTwoMilestones(file_state);
    file_state.AwardXp(15u);
    EXPECT_TRUE(file_state.Save(path.Get()).IsOk());

    const u8 version_two[4] = {2u, 0u, 0u, 0u};
    EXPECT_TRUE(PatchFileBytes(path.Get(), 8u, version_two, 4u));

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubMigrationNeeded));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.alpha"));
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionInternalTrailingBytesAreRejectedTransactionally)
{
    FTempSavePath path(L"progression_trailing");
    u8 payload[9] = {};
    WriteU32LE(payload + 0, 100u);
    WriteU32LE(payload + 4, 0u);
    payload[8] = 0xCCu;
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          payload, sizeof(payload)).IsOk());

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionNonCanonicalEntryIsRejectedTransactionally)
{
    FTempSavePath path(L"progression_noncanonical");
    u8 payload[24] = {};
    WriteU32LE(payload + 0, 100u);
    WriteU32LE(payload + 4, 1u);
    WriteU32LE(payload + 8, HashId("milestone.alpha"));
    payload[12] = 2u; // achieved は 0/1 のみ。
    WriteU64LE(payload + 16, 42u);
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          payload, sizeof(payload)).IsOk());

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionDuplicateEntryIsRejectedTransactionally)
{
    FTempSavePath path(L"progression_duplicate");
    u8 payload[40] = {};
    WriteU32LE(payload + 0, 100u);
    WriteU32LE(payload + 4, 2u);
    const u32 hash = HashId("milestone.alpha");
    WriteU32LE(payload + 8, hash);
    payload[12] = 1u;
    WriteU64LE(payload + 16, 10u);
    WriteU32LE(payload + 24, hash);
    payload[28] = 1u;
    WriteU64LE(payload + 32, 20u);
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          payload, sizeof(payload)).IsOk());

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionEntryLimitIsRejectedBeforeMutation)
{
    FTempSavePath path(L"progression_limit");
    u8 payload[8] = {};
    WriteU32LE(payload + 0, 100u);
    WriteU32LE(payload + 4, FProgression::kMaxPersistedMilestones + 1u);
    EXPECT_TRUE(FSaveArchive::WriteToFile(path.Get(), 1u,
                                          payload, sizeof(payload)).IsOk());

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(
            result.Error().subcode,
            ProgressionSub(EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, ProgressionCrcFailurePreservesCurrentState)
{
    FTempSavePath path(L"progression_crc");
    FProgression file_state;
    RegisterTwoMilestones(file_state);
    file_state.AwardXp(15u);
    EXPECT_TRUE(file_state.Save(path.Get()).IsOk());

    const u8 corrupt_xp = 0xFFu;
    EXPECT_TRUE(PatchFileBytes(path.Get(), FSaveArchive::kHeaderSize,
                               &corrupt_xp, 1u));

    FProgression current;
    RegisterTwoMilestones(current);
    current.AwardXp(25u);
    const auto result = current.Load(path.Get());
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubChecksumFail));
    }
    EXPECT_EQ(current.CurrentXp(), 25u);
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.alpha"));
    EXPECT_TRUE(current.IsMilestoneAchieved("milestone.beta"));
}

ACS_TEST(SaveIntegration, NullPathsUseDetailedInvalidArgumentDiagnostic)
{
    TSaveSlot<FSlotPayload> slot;
    FSlotPayload payload{};
    const auto slot_save = slot.Save(payload);
    EXPECT_TRUE(slot_save.IsErr()); // 未初期化slotは固有診断を維持。

    FProgression progression;
    const auto save = progression.Save(nullptr);
    const auto load = progression.Load(nullptr);
    EXPECT_TRUE(save.IsErr());
    EXPECT_TRUE(load.IsErr());
    if (save.IsErr()) {
        EXPECT_EQ(save.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubInvalidArgument));
    }
    if (load.IsErr()) {
        EXPECT_EQ(load.Error().subcode,
                  SaveSub(ESaveArchiveSubCode::kSubInvalidArgument));
    }
}
