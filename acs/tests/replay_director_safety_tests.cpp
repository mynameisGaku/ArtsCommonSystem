// SPDX-License-Identifier: Apache-2.0
// ReplayDirector / InputRecorder / Lockstep の永続化安全契約。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Platform.h"
#include "gameframework/InputRecorder.h"
#include "gameframework/Lockstep.h"
#include "gameframework/ReplayDirector.h"
#include "memory/Memory.h"

using namespace acs;
using namespace acs::game;

namespace {

class FSwitchableAllocator final : public IAllocator {
public:
    explicit FSwitchableAllocator(IAllocator& backing) noexcept : m_Backing(&backing) {}

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }
    void Free(void* pointer) noexcept override { m_Backing->Free(pointer); }
    void SetFailing(bool failing) noexcept { m_Failing = failing; }

private:
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

class FSelectiveFailAllocator final : public IAllocator {
public:
    explicit FSelectiveFailAllocator(IAllocator& backing) noexcept : m_Backing(&backing) {}

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        if (m_Armed && size == m_ObservedSize) ++m_ObservedSuccesses;
        if (m_Armed && size == m_FailSize) {
            ++m_InjectedFailures;
            return nullptr;
        }
        return m_Backing->Alloc(size, alignment, location);
    }
    void Free(void* pointer) noexcept override { m_Backing->Free(pointer); }
    void Arm(usize observed_size, usize fail_size) noexcept
    {
        m_ObservedSize = observed_size;
        m_FailSize = fail_size;
        m_ObservedSuccesses = 0;
        m_InjectedFailures = 0;
        m_Armed = true;
    }
    void Disarm() noexcept { m_Armed = false; }
    u32 ObservedSuccesses() const noexcept { return m_ObservedSuccesses; }
    u32 InjectedFailures() const noexcept { return m_InjectedFailures; }

private:
    IAllocator* m_Backing = nullptr;
    usize m_ObservedSize = 0;
    usize m_FailSize = 0;
    u32 m_ObservedSuccesses = 0;
    u32 m_InjectedFailures = 0;
    bool m_Armed = false;
};

class FDefaultAllocatorScope {
public:
    explicit FDefaultAllocatorScope(IAllocator& replacement) noexcept : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&replacement);
    }
    ~FDefaultAllocatorScope() noexcept { SetDefaultAllocator(m_Previous); }

private:
    IAllocator* m_Previous = nullptr;
};

struct FTempReplayPath {
    explicit FTempReplayPath(const wchar_t* tag) noexcept
    {
        usize position = ::GetTempPathW(MAX_PATH, path);
        Append(position, L"acs_replay_safety_");
        AppendNumber(position, static_cast<u32>(::GetCurrentProcessId()));
        Append(position, L"_");
        Append(position, tag);
        Append(position, L".acrp");
        path[position] = L'\0';
        ::DeleteFileW(path);
    }
    ~FTempReplayPath() noexcept { ::DeleteFileW(path); }

    void Append(usize& position, const wchar_t* text) noexcept
    {
        while (*text != L'\0' && position + 1u < sizeof(path) / sizeof(path[0])) path[position++] = *text++;
    }
    void AppendNumber(usize& position, u32 value) noexcept
    {
        wchar_t reversed[10] = {};
        usize count = 0;
        do {
            reversed[count++] = static_cast<wchar_t>(L'0' + value % 10u);
            value /= 10u;
        } while (value != 0);
        while (count > 0 && position + 1u < sizeof(path) / sizeof(path[0])) {
            path[position++] = reversed[--count];
        }
    }

    wchar_t path[MAX_PATH + 96] = {};
};

u32 Crc32(const void* data, u64 size) noexcept
{
    const u8* bytes = static_cast<const u8*>(data);
    u32 crc = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (u32 bit = 0; bit < 8u; ++bit) crc = (crc & 1u) ? 0xEDB88320u ^ (crc >> 1u) : crc >> 1u;
    }
    return crc ^ 0xFFFFFFFFu;
}

void WriteU32(u8* destination, u32 value) noexcept { MemCopy(destination, &value, sizeof(value)); }

f32 FloatFromBits(u32 bits) noexcept
{
    f32 value = 0.0f;
    MemCopy(&value, &bits, sizeof(value));
    return value;
}

bool WriteFileBytes(const wchar_t* path, const void* data, u32 size) noexcept
{
    HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = ::WriteFile(file, data, size, &written, nullptr) && written == size;
    const bool flushed = wrote && ::FlushFileBuffers(file);
    const bool closed = ::CloseHandle(file) != 0;
    return wrote && flushed && closed;
}

bool ReadFileBytes(const wchar_t* path, TArray<u8>& bytes) noexcept
{
    HANDLE file = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024 ||
        !bytes.TryResize(static_cast<usize>(size.QuadPart))) {
        ::CloseHandle(file);
        return false;
    }
    DWORD read = 0;
    const bool read_all = ::ReadFile(file, bytes.Data(), static_cast<DWORD>(bytes.Size()), &read, nullptr) &&
                          read == bytes.Size();
    const bool closed = ::CloseHandle(file) != 0;
    return read_all && closed;
}

bool TextEquals(const char* text, const char* expected) noexcept
{
    if (text == nullptr || expected == nullptr) return text == expected;
    usize index = 0;
    while (text[index] != '\0' && expected[index] != '\0' && text[index] == expected[index]) ++index;
    return text[index] == expected[index];
}

FReplayMetadata Metadata(u64 seed, const char* version = "v1") noexcept
{
    FReplayMetadata metadata{};
    metadata.game_version = version;
    metadata.level_id = "level";
    metadata.seed = seed;
    metadata.timestamp = 1234;
    metadata.duration_ticks = 90;
    metadata.player_name = "p";
    metadata.checksum_hex = "0123456789abcdef";
    return metadata;
}

void PopulateSources(CInputRecorder& recorder, CLockstep& lockstep, u32 count) noexcept
{
    recorder.StartRecording(60u);
    lockstep.Init(ENetMode::Local, 60u);
    for (u32 i = 0; i < count; ++i) {
        FInputSample sample{};
        sample.tick = i;
        sample.key_codes_changed[0] = static_cast<u8>(i + 1u);
        recorder.Capture(sample);
        FInputFrame frame{};
        frame.tick = i;
        frame.player_id = i & 1u;
        frame.buttons = static_cast<u8>(i + 1u);
        lockstep.RecordInput(frame);
    }
    recorder.StopRecording();
}

bool SaveReplayFile(const wchar_t* path, u64 seed, CInputRecorder* recorder = nullptr,
                    CLockstep* lockstep = nullptr, const char* version = "v1") noexcept
{
    CReplayDirector director;
    director.Init();
    director.SetSources(recorder, lockstep);
    if (director.TryStartRecording(Metadata(seed, version)).IsErr()) return false;
    if (director.StopRecording().IsErr()) return false;
    return director.TrySaveReplay(path).IsOk();
}

} // namespace

ACS_TEST(ReplayDirectorSafety, RecordingOwnsBoundedMetadataTransactionally)
{
    CReplayDirector director;
    director.Init();
    char version[] = "local";
    FReplayMetadata metadata = Metadata(7u, version);
    EXPECT_TRUE(director.TryStartRecording(metadata).IsOk());
    version[0] = 'X';
    EXPECT_TRUE(TextEquals(director.Metadata().game_version, "local"));
    EXPECT_TRUE(director.StopRecording().IsOk());

    FReplayMetadata invalid = Metadata(8u);
    invalid.checksum_hex = "not-hex";
    EXPECT_TRUE(director.TryStartRecording(invalid).IsErr());
    EXPECT_EQ(director.Metadata().seed, 7u);
    EXPECT_EQ(static_cast<u32>(director.CurrentMode()), static_cast<u32>(EReplayMode::Idle));
}

ACS_TEST(ReplayDirectorSafety, SaveLoadRoundTripUsesOwnedMetadata)
{
    FTempReplayPath path(L"roundtrip");
    EXPECT_TRUE(SaveReplayFile(path.path, 0x11223344u));

    CReplayDirector loaded;
    loaded.Init();
    EXPECT_TRUE(loaded.TryLoadReplay(path.path).IsOk());
    EXPECT_EQ(loaded.Metadata().seed, 0x11223344u);
    EXPECT_TRUE(TextEquals(loaded.Metadata().game_version, "v1"));
    EXPECT_TRUE(TextEquals(loaded.Metadata().checksum_hex, "0123456789abcdef"));
    EXPECT_EQ(static_cast<u32>(loaded.CurrentMode()), static_cast<u32>(EReplayMode::Idle));
}

ACS_TEST(ReplayDirectorSafety, StrictLoadRejectsCrcTruncationTrailingAndOversizedMetadata)
{
    FTempReplayPath valid_path(L"strict_valid");
    FTempReplayPath corrupt_path(L"strict_corrupt");
    EXPECT_TRUE(SaveReplayFile(valid_path.path, 1u));
    TArray<u8> valid;
    EXPECT_TRUE(ReadFileBytes(valid_path.path, valid));
    EXPECT_TRUE(valid.Size() >= 56u);

    CReplayDirector target;
    target.Init();
    EXPECT_TRUE(target.TryStartRecording(Metadata(99u)).IsOk());
    EXPECT_TRUE(target.StopRecording().IsOk());

    TArray<u8> variant = valid.Clone();
    variant[8] ^= 1u;
    EXPECT_TRUE(WriteFileBytes(corrupt_path.path, variant.Data(), static_cast<u32>(variant.Size())));
    EXPECT_TRUE(target.TryLoadReplay(corrupt_path.path).IsErr());
    EXPECT_EQ(target.Metadata().seed, 99u);

    EXPECT_TRUE(WriteFileBytes(corrupt_path.path, valid.Data(), 32u));
    EXPECT_TRUE(target.TryLoadReplay(corrupt_path.path).IsErr());
    EXPECT_EQ(target.Metadata().seed, 99u);

    variant.Resize(valid.Size() + 1u);
    MemCopy(variant.Data(), valid.Data(), valid.Size() - 4u);
    variant[valid.Size() - 4u] = 0xA5u;
    WriteU32(variant.Data() + variant.Size() - 4u, Crc32(variant.Data(), variant.Size() - 4u));
    EXPECT_TRUE(WriteFileBytes(corrupt_path.path, variant.Data(), static_cast<u32>(variant.Size())));
    EXPECT_TRUE(target.TryLoadReplay(corrupt_path.path).IsErr());
    EXPECT_EQ(target.Metadata().seed, 99u);

    variant = valid.Clone();
    WriteU32(variant.Data() + 28u, kReplayMaximumGameVersionBytes + 1u);
    WriteU32(variant.Data() + variant.Size() - 4u, Crc32(variant.Data(), variant.Size() - 4u));
    EXPECT_TRUE(WriteFileBytes(corrupt_path.path, variant.Data(), static_cast<u32>(variant.Size())));
    const TResult<void> oversized = target.TryLoadReplay(corrupt_path.path);
    EXPECT_TRUE(oversized.IsErr());
    if (oversized.IsErr()) EXPECT_EQ(oversized.Error().subcode,
                                     static_cast<u16>(CReplayDirector::kSub_BadMetadata));
    EXPECT_EQ(target.Metadata().seed, 99u);
}

ACS_TEST(ReplayDirectorSafety, OversizedSparseFileIsRejectedBeforeAllocation)
{
    FTempReplayPath path(L"sparse");
    HANDLE file = ::CreateFileW(path.path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(kReplayMaximumContainerBytes + 1u);
        EXPECT_TRUE(::SetFilePointerEx(file, end, nullptr, FILE_BEGIN) != 0);
        EXPECT_TRUE(::SetEndOfFile(file) != 0);
        EXPECT_TRUE(::CloseHandle(file) != 0);
    }
    CReplayDirector director;
    director.Init();
    const TResult<void> result = director.TryLoadReplay(path.path);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) EXPECT_EQ(result.Error().subcode,
                                  static_cast<u16>(CReplayDirector::kSub_LimitExceeded));
}

ACS_TEST(ReplayDirectorSafety, SourceCheckedLoadsPreserveStateOnOomAndSaveDoesNotPartiallyWrite)
{
    CInputRecorder source_recorder;
    CLockstep source_lockstep;
    PopulateSources(source_recorder, source_lockstep, 2u);
    u8 recorder_blob[256] = {};
    u8 lockstep_blob[256] = {};
    u32 recorder_size = 0;
    u32 lockstep_size = 0;
    EXPECT_TRUE(source_recorder.SaveToBuffer(recorder_blob, sizeof(recorder_blob), recorder_size).IsOk());
    EXPECT_TRUE(source_lockstep.SaveToBuffer(lockstep_blob, sizeof(lockstep_blob), lockstep_size).IsOk());

    FSwitchableAllocator allocator(DefaultAllocator());
    {
        FDefaultAllocatorScope scope(allocator);
        CInputRecorder target_recorder;
        CLockstep target_lockstep;
        PopulateSources(target_recorder, target_lockstep, 1u);
        allocator.SetFailing(true);
        EXPECT_TRUE(target_recorder.TryLoadFromBuffer(recorder_blob, recorder_size).IsErr());
        EXPECT_TRUE(target_lockstep.TryLoadFromBuffer(lockstep_blob, lockstep_size).IsErr());
        EXPECT_EQ(target_recorder.SampleCount(), 1u);
        EXPECT_EQ(target_lockstep.InputCount(), 1u);
        allocator.SetFailing(false);
    }

    u8 recorder_output[8] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
    u8 lockstep_output[8] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    u32 written = 77u;
    EXPECT_TRUE(source_recorder.SaveToBuffer(recorder_output, sizeof(recorder_output), written).IsErr());
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(recorder_output[0], static_cast<u8>(0x5Au));
    written = 88u;
    EXPECT_TRUE(source_lockstep.SaveToBuffer(lockstep_output, sizeof(lockstep_output), written).IsErr());
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(lockstep_output[7], static_cast<u8>(0xA5u));
}

ACS_TEST(ReplayDirectorSafety, SourceSavesRejectNonFiniteValuesBeforeWriting)
{
    CInputRecorder recorder;
    recorder.StartRecording(60u);
    FInputSample sample{};
    sample.mouse_pos.x = FloatFromBits(0x7FC00000u);
    recorder.Capture(sample);
    recorder.StopRecording();

    CLockstep lockstep;
    lockstep.Init(ENetMode::Local, 60u);
    FInputFrame frame{};
    frame.axis.y = FloatFromBits(0x7F800000u);
    lockstep.RecordInput(frame);

    u8 recorder_output[128];
    u8 lockstep_output[128];
    MemSet(recorder_output, 0x5A, sizeof(recorder_output));
    MemSet(lockstep_output, 0xA5, sizeof(lockstep_output));
    u32 written = 77u;
    const TResult<void> recorder_result =
        recorder.SaveToBuffer(recorder_output, sizeof(recorder_output), written);
    EXPECT_TRUE(recorder_result.IsErr());
    if (recorder_result.IsErr()) {
        EXPECT_EQ(recorder_result.Error().subcode,
                  static_cast<u16>(CInputRecorder::kSub_BadValue));
    }
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(recorder_output[0], static_cast<u8>(0x5Au));
    EXPECT_EQ(recorder_output[sizeof(recorder_output) - 1u], static_cast<u8>(0x5Au));

    written = 88u;
    const TResult<void> lockstep_result =
        lockstep.SaveToBuffer(lockstep_output, sizeof(lockstep_output), written);
    EXPECT_TRUE(lockstep_result.IsErr());
    if (lockstep_result.IsErr()) {
        EXPECT_EQ(lockstep_result.Error().subcode,
                  static_cast<u16>(CLockstep::kSub_BadValue));
    }
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(lockstep_output[0], static_cast<u8>(0xA5u));
    EXPECT_EQ(lockstep_output[sizeof(lockstep_output) - 1u], static_cast<u8>(0xA5u));
}

ACS_TEST(ReplayDirectorSafety, ReplayStagesBothSourcesBeforeNoFailCommit)
{
    FTempReplayPath path(L"source_transaction");
    CInputRecorder file_recorder;
    CLockstep file_lockstep;
    PopulateSources(file_recorder, file_lockstep, 2u);
    EXPECT_TRUE(SaveReplayFile(path.path, 55u, &file_recorder, &file_lockstep));

    FSelectiveFailAllocator allocator(DefaultAllocator());
    {
        FDefaultAllocatorScope scope(allocator);
        CInputRecorder target_recorder;
        CLockstep target_lockstep;
        PopulateSources(target_recorder, target_lockstep, 1u);
        CReplayDirector target;
        target.Init();
        target.SetSources(&target_recorder, &target_lockstep);
        EXPECT_TRUE(target.TryStartRecording(Metadata(99u)).IsOk());
        EXPECT_TRUE(target.StopRecording().IsOk());

        EXPECT_NE(sizeof(FInputSample) * 2u, sizeof(FInputFrame) * 2u);
        allocator.Arm(sizeof(FInputSample) * 2u, sizeof(FInputFrame) * 2u);
        const TResult<void> result = target.TryLoadReplay(path.path);
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) EXPECT_EQ(result.Error().subcode, static_cast<u16>(CReplayDirector::kSub_Oom));
        EXPECT_EQ(allocator.ObservedSuccesses(), 1u);
        EXPECT_EQ(allocator.InjectedFailures(), 1u);
        EXPECT_EQ(target_recorder.SampleCount(), 1u);
        EXPECT_EQ(target_lockstep.InputCount(), 1u);
        EXPECT_EQ(target.Metadata().seed, 99u);

        allocator.Disarm();
        EXPECT_TRUE(target.TryLoadReplay(path.path).IsOk());
        EXPECT_EQ(target_recorder.SampleCount(), 2u);
        EXPECT_EQ(target_lockstep.InputCount(), 2u);
        EXPECT_EQ(target.Metadata().seed, 55u);
    }
}

ACS_TEST(ReplayDirectorSafety, AtomicReplaceFailurePreservesExistingReplay)
{
    FTempReplayPath path(L"atomic");
    EXPECT_TRUE(SaveReplayFile(path.path, 11u));
    HANDLE reader = ::CreateFileW(path.path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(reader != INVALID_HANDLE_VALUE);

    CReplayDirector replacement;
    replacement.Init();
    EXPECT_TRUE(replacement.TryStartRecording(Metadata(22u)).IsOk());
    EXPECT_TRUE(replacement.StopRecording().IsOk());
    const TResult<void> save = replacement.TrySaveReplay(path.path);
    EXPECT_TRUE(save.IsErr());
    if (reader != INVALID_HANDLE_VALUE) EXPECT_TRUE(::CloseHandle(reader) != 0);

    CReplayDirector verifier;
    verifier.Init();
    EXPECT_TRUE(verifier.TryLoadReplay(path.path).IsOk());
    EXPECT_EQ(verifier.Metadata().seed, 11u);
}

ACS_TEST(ReplayDirectorSafety, ShareDeleteReaderKeepsOldSnapshotAcrossAtomicReplace)
{
    FTempReplayPath path(L"atomic_reader");
    EXPECT_TRUE(SaveReplayFile(path.path, 101u));
    HANDLE reader = ::CreateFileW(path.path, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(reader != INVALID_HANDLE_VALUE);

    CReplayDirector replacement;
    replacement.Init();
    EXPECT_TRUE(replacement.TryStartRecording(Metadata(202u)).IsOk());
    EXPECT_TRUE(replacement.StopRecording().IsOk());
    EXPECT_TRUE(replacement.TrySaveReplay(path.path).IsOk());

    u8 old_header[16] = {};
    DWORD read = 0;
    if (reader != INVALID_HANDLE_VALUE) {
        EXPECT_TRUE(::ReadFile(reader, old_header, sizeof(old_header), &read, nullptr) != 0);
        EXPECT_EQ(read, static_cast<DWORD>(sizeof(old_header)));
        u64 old_seed = 0;
        MemCopy(&old_seed, old_header + 8u, sizeof(old_seed));
        EXPECT_EQ(old_seed, 101u);
        EXPECT_TRUE(::CloseHandle(reader) != 0);
    }

    CReplayDirector current;
    current.Init();
    EXPECT_TRUE(current.TryLoadReplay(path.path).IsOk());
    EXPECT_EQ(current.Metadata().seed, 202u);
}
