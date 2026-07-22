// SPDX-License-Identifier: Apache-2.0
// FLocalFileAssetLocking の所有権境界・敵対 record テスト。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/StudioWorkflow.h"
#include "foundation/Platform.h"

using namespace acs;
using namespace acs::game;

namespace {

bool TextEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) return false;
    }
    return *a == *b;
}

struct FLockTempPath {
    explicit FLockTempPath(const wchar_t* tag) noexcept {
        usize at = static_cast<usize>(::GetTempPathW(MAX_PATH, AssetWide));
        Append(at, L"acs_studio_lock_");
        AppendU32(at, static_cast<u32>(::GetCurrentProcessId()));
        Append(at, L"_");
        Append(at, tag);
        AssetWide[at] = L'\0';

        for (usize i = 0; i <= at; ++i) LockWide[i] = AssetWide[i];
        usize lock_at = at;
        Append(lock_at, L".lock", LockWide);
        LockWide[lock_at] = L'\0';
        ::DeleteFileW(LockWide);

        const int converted = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, AssetWide, -1, AssetUtf8,
            static_cast<int>(sizeof(AssetUtf8)), nullptr, nullptr);
        if (converted <= 0) AssetUtf8[0] = '\0';
    }

    ~FLockTempPath() noexcept { ::DeleteFileW(LockWide); }

    void Append(usize& at, const wchar_t* text) noexcept {
        Append(at, text, AssetWide);
    }

    static void Append(usize& at, const wchar_t* text,
                       wchar_t* output) noexcept {
        while (*text != L'\0' && at + 1u < MAX_PATH + 128u) {
            output[at++] = *text++;
        }
    }

    void AppendU32(usize& at, u32 value) noexcept {
        wchar_t reversed[10] = {};
        usize count = 0;
        do {
            reversed[count++] = static_cast<wchar_t>(L'0' + value % 10u);
            value /= 10u;
        } while (value != 0);
        while (count != 0 && at + 1u < MAX_PATH + 128u) {
            AssetWide[at++] = reversed[--count];
        }
    }

    bool WriteLock(const void* bytes, usize size) const noexcept {
        HANDLE file = ::CreateFileW(LockWide, GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok =
            size <= 0xFFFFFFFFu &&
            ::WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr) != 0 &&
            written == static_cast<DWORD>(size) &&
            ::FlushFileBuffers(file) != 0;
        return ::CloseHandle(file) != 0 && ok;
    }

    bool Exists() const noexcept {
        return ::GetFileAttributesW(LockWide) != INVALID_FILE_ATTRIBUTES;
    }

    wchar_t AssetWide[MAX_PATH + 128] = {};
    wchar_t LockWide[MAX_PATH + 128] = {};
    char AssetUtf8[(MAX_PATH + 128) * 3] = {};
};

struct FRaceAcquire {
    FLocalFileAssetLocking Backend;
    const char* Path = nullptr;
    const char* Owner = nullptr;
    HANDLE Start = nullptr;
    FLocalAssetLockResult Result{};
};

DWORD WINAPI RaceAcquireProc(void* value) noexcept {
    FRaceAcquire* context = static_cast<FRaceAcquire*>(value);
    ::WaitForSingleObject(context->Start, INFINITE);
    context->Result = context->Backend.TryLockAsset(context->Path, context->Owner);
    return 0;
}

} // namespace

ACS_TEST(StudioWorkflowLockSafety, CheckedAcquireQueryAndReleaseRoundTrip)
{
    FLockTempPath path(L"round_trip");
    FLocalFileAssetLocking backend;

    const FLocalAssetLockResult acquired =
        backend.TryLockAsset(path.AssetUtf8, "artist_a");
    EXPECT_TRUE(acquired.Succeeded());
    EXPECT_TRUE(acquired.token.IsValid());
    EXPECT_TRUE(acquired.lock_time != 0);
    EXPECT_TRUE(TextEquals(
        LocalAssetLockErrorName(ELocalAssetLockError::TokenMismatch),
        "TokenMismatch"));

    FAssetLockInfo info{};
    const FLocalAssetLockResult queried =
        backend.TryQueryLock(path.AssetUtf8, info);
    EXPECT_TRUE(queried.Succeeded());
    EXPECT_EQ(queried.token.high, acquired.token.high);
    EXPECT_EQ(queried.token.low, acquired.token.low);
    EXPECT_TRUE(TextEquals(info.asset_path, path.AssetUtf8));
    EXPECT_TRUE(TextEquals(info.locker_user, "artist_a"));

    EXPECT_EQ(backend.TryLockAsset(path.AssetUtf8, "artist_a").error,
              ELocalAssetLockError::AlreadyLocked);
    EXPECT_TRUE(backend.TryUnlockAsset(
        path.AssetUtf8, "artist_a", acquired.token).Succeeded());
    EXPECT_FALSE(path.Exists());
}

ACS_TEST(StudioWorkflowLockSafety, WrongOwnerOrGenerationNeverDeletesLock)
{
    FLockTempPath path(L"ownership");
    FLocalFileAssetLocking owner_backend;
    FLocalFileAssetLocking other_backend;
    const FLocalAssetLockResult acquired =
        owner_backend.TryLockAsset(path.AssetUtf8, "owner");
    EXPECT_TRUE(acquired.Succeeded());

    EXPECT_EQ(other_backend.TryUnlockAsset(
                  path.AssetUtf8, "intruder", acquired.token).error,
              ELocalAssetLockError::OwnerMismatch);
    EXPECT_TRUE(path.Exists());

    FLocalAssetLockToken stale = acquired.token;
    stale.low ^= 1u;
    EXPECT_EQ(other_backend.TryUnlockAsset(
                  path.AssetUtf8, "owner", stale).error,
              ELocalAssetLockError::TokenMismatch);
    EXPECT_TRUE(path.Exists());
    EXPECT_TRUE(other_backend.UnlockAsset(path.AssetUtf8).IsErr());
    EXPECT_TRUE(path.Exists());

    EXPECT_TRUE(owner_backend.TryUnlockAsset(
        path.AssetUtf8, "owner", acquired.token).Succeeded());

    const FLocalAssetLockResult reacquired =
        owner_backend.TryLockAsset(path.AssetUtf8, "owner");
    EXPECT_TRUE(reacquired.Succeeded());
    EXPECT_FALSE(reacquired.token.high == acquired.token.high &&
                 reacquired.token.low == acquired.token.low);
    EXPECT_EQ(other_backend.TryUnlockAsset(
                  path.AssetUtf8, "owner", acquired.token).error,
              ELocalAssetLockError::TokenMismatch);
    EXPECT_TRUE(path.Exists());
    EXPECT_TRUE(owner_backend.TryUnlockAsset(
        path.AssetUtf8, "owner", reacquired.token).Succeeded());
}

ACS_TEST(StudioWorkflowLockSafety, MaliciousRecordsAreRejectedTransactionally)
{
    FLockTempPath path(L"corrupt");
    FLocalFileAssetLocking backend;
    FAssetLockInfo sentinel{};
    sentinel.asset_path = "preserved_path";
    sentinel.locker_user = "preserved_owner";
    sentinel.lock_time = 77;

    const char partial[] = "ACSLOCK/1\nOWNER:x\nTOKEN:0123";
    EXPECT_TRUE(path.WriteLock(partial, sizeof(partial) - 1u));
    EXPECT_EQ(backend.TryQueryLock(path.AssetUtf8, sentinel).error,
              ELocalAssetLockError::CorruptRecord);
    EXPECT_TRUE(TextEquals(sentinel.asset_path, "preserved_path"));
    EXPECT_EQ(sentinel.lock_time, 77u);

    const char embedded_nul[] =
        "ACSLOCK/1\nOWNER:bad\0owner\n"
        "TOKEN:0123456789ABCDEF0123456789ABCDEF\nTIME:1\n";
    EXPECT_TRUE(path.WriteLock(embedded_nul, sizeof(embedded_nul) - 1u));
    EXPECT_EQ(backend.TryQueryLock(path.AssetUtf8, sentinel).error,
              ELocalAssetLockError::CorruptRecord);

    const char trailing[] =
        "ACSLOCK/1\nOWNER:x\n"
        "TOKEN:0123456789ABCDEF0123456789ABCDEF\nTIME:1\nTRAILING";
    EXPECT_TRUE(path.WriteLock(trailing, sizeof(trailing) - 1u));
    EXPECT_EQ(backend.TryQueryLock(path.AssetUtf8, sentinel).error,
              ELocalAssetLockError::CorruptRecord);

    const char overflow[] =
        "ACSLOCK/1\nOWNER:x\n"
        "TOKEN:0123456789ABCDEF0123456789ABCDEF\n"
        "TIME:18446744073709551616\n";
    EXPECT_TRUE(path.WriteLock(overflow, sizeof(overflow) - 1u));
    EXPECT_EQ(backend.TryQueryLock(path.AssetUtf8, sentinel).error,
              ELocalAssetLockError::CorruptRecord);

    char oversized[FLocalFileAssetLocking::kMaxRecordBytes + 1] = {};
    EXPECT_TRUE(path.WriteLock(oversized, sizeof(oversized)));
    EXPECT_EQ(backend.TryQueryLock(path.AssetUtf8, sentinel).error,
              ELocalAssetLockError::RecordTooLarge);
    EXPECT_TRUE(path.Exists()); // 破損・古い記録は fail closed とし、自動回収しない
}

ACS_TEST(StudioWorkflowLockSafety, BoundedInputsAndExistingFileFailClosed)
{
    FLockTempPath path(L"bounds");
    FLocalFileAssetLocking backend;

    char long_owner[FLocalFileAssetLocking::kMaxUserChars + 1] = {};
    for (int i = 0; i < FLocalFileAssetLocking::kMaxUserChars; ++i) {
        long_owner[i] = 'a';
    }
    EXPECT_EQ(backend.TryLockAsset(path.AssetUtf8, long_owner).error,
              ELocalAssetLockError::OwnerTooLong);

    char long_path[FLocalFileAssetLocking::kMaxPathChars + 1] = {};
    for (int i = 0; i < FLocalFileAssetLocking::kMaxPathChars; ++i) {
        long_path[i] = 'p';
    }
    EXPECT_EQ(backend.TryLockAsset(long_path, "owner").error,
              ELocalAssetLockError::PathTooLong);

    const char legacy[] = "someone\n1\n";
    EXPECT_TRUE(path.WriteLock(legacy, sizeof(legacy) - 1u));
    EXPECT_EQ(backend.TryLockAsset(path.AssetUtf8, "owner").error,
              ELocalAssetLockError::AlreadyLocked);
    EXPECT_TRUE(path.Exists());
    EXPECT_TRUE(backend.UnlockAssetAs(path.AssetUtf8, "someone").IsErr());
    EXPECT_TRUE(path.Exists());
}

ACS_TEST(StudioWorkflowLockSafety, ConcurrentCreateNewHasSingleWinner)
{
    FLockTempPath path(L"race");
    HANDLE start = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    EXPECT_TRUE(start != nullptr);
    if (start == nullptr) return;

    FRaceAcquire first{};
    first.Path = path.AssetUtf8;
    first.Owner = "first";
    first.Start = start;
    FRaceAcquire second{};
    second.Path = path.AssetUtf8;
    second.Owner = "second";
    second.Start = start;

    HANDLE thread_a = ::CreateThread(nullptr, 0, RaceAcquireProc, &first, 0, nullptr);
    HANDLE thread_b = ::CreateThread(nullptr, 0, RaceAcquireProc, &second, 0, nullptr);
    EXPECT_TRUE(thread_a != nullptr);
    EXPECT_TRUE(thread_b != nullptr);
    ::SetEvent(start);
    if (thread_a == nullptr || thread_b == nullptr) {
        if (thread_a != nullptr) {
            ::WaitForSingleObject(thread_a, INFINITE);
            ::CloseHandle(thread_a);
        }
        if (thread_b != nullptr) {
            ::WaitForSingleObject(thread_b, INFINITE);
            ::CloseHandle(thread_b);
        }
        ::CloseHandle(start);
        return;
    }
    if (thread_a != nullptr) ::WaitForSingleObject(thread_a, INFINITE);
    if (thread_b != nullptr) ::WaitForSingleObject(thread_b, INFINITE);
    if (thread_a != nullptr) ::CloseHandle(thread_a);
    if (thread_b != nullptr) ::CloseHandle(thread_b);
    ::CloseHandle(start);

    const int successes =
        (first.Result.Succeeded() ? 1 : 0) +
        (second.Result.Succeeded() ? 1 : 0);
    EXPECT_EQ(successes, 1);
    FRaceAcquire& winner =
        first.Result.Succeeded() ? first : second;
    const FRaceAcquire& loser =
        first.Result.Succeeded() ? second : first;
    EXPECT_EQ(loser.Result.error, ELocalAssetLockError::AlreadyLocked);
    EXPECT_TRUE(winner.Backend.TryUnlockAsset(
        path.AssetUtf8, winner.Owner, winner.Result.token).Succeeded());
}
