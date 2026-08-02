// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/HotReload.h"
#include "foundation/Platform.h"
#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS) && !defined(ACS_GAME_SHIPPING)
#include "gameframework/HotReloadDiagnosticsInternal.h"
#endif

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace acs;
using namespace acs::game;

namespace acs::game::hot_reload_detail {

// 診断 counter が使う内部の飽和加算 seam。
void SaturatingIncrement(u64& value) noexcept;

// native Windows parser が使う内部の純粋検証 seam。
bool IsValidNativeNotificationFields(
    u32 action, usize file_name_bytes) noexcept;

} // namespace acs::game::hot_reload_detail

namespace {

struct FDispatchProbe {
    CHotReloadWatcher* watcher = nullptr;
    u32 first_calls = 0u;
    u32 second_calls = 0u;
    EHotReloadResult nested_tick = EHotReloadResult::Success;
    bool saw_expected_path = false;
};

void SecondCallback(void* user, const FHotReloadEvent&) noexcept {
    auto* probe = static_cast<FDispatchProbe*>(user);
    ++probe->second_calls;
}

void CountCallback(void* user, const FHotReloadEvent&) noexcept {
    auto* count = static_cast<u32*>(user);
    ++(*count);
}

void FirstCallback(void* user, const FHotReloadEvent& event) noexcept {
    auto* probe = static_cast<FDispatchProbe*>(user);
    ++probe->first_calls;
    probe->saw_expected_path =
        event.file_path != nullptr &&
        std::strcmp(event.file_path, "Assets/reentrant.txt") == 0;
    probe->nested_tick = probe->watcher->TryTick(0.0f);
    (void)probe->watcher->UnregisterCallback(&SecondCallback, user);
}

void ShutdownCallback(void* user, const FHotReloadEvent&) noexcept {
    static_cast<CHotReloadWatcher*>(user)->Shutdown();
}

struct FEnqueueProbe {
    CHotReloadWatcher* watcher = nullptr;
    u32 calls = 0u;
};

void EnqueueCallback(void* user, const FHotReloadEvent&) noexcept {
    auto* probe = static_cast<FEnqueueProbe*>(user);
    ++probe->calls;
    if (probe->calls == 1u) {
        (void)probe->watcher->TryEnqueueEvent("Assets/deferred.txt", 2u);
    }
}

struct FClearAndEnqueueProbe {
    CHotReloadWatcher* watcher = nullptr;
    u32 calls = 0u;
    EHotReloadResult enqueue_result = EHotReloadResult::Success;
    bool saw_initial = false;
    bool saw_deferred = false;
};

void ClearAndEnqueueCallback(
    void* user, const FHotReloadEvent& event) noexcept {
    auto* probe = static_cast<FClearAndEnqueueProbe*>(user);
    ++probe->calls;
    if (probe->calls == 1u) {
        probe->saw_initial =
            event.file_path != nullptr &&
            std::strcmp(event.file_path, "Assets/a.txt") == 0;
        probe->watcher->ClearEvents();
        probe->enqueue_result =
            probe->watcher->TryEnqueueEvent(
                "Assets/deferred_after_clear.txt", 3u);
        return;
    }
    probe->saw_deferred =
        event.file_path != nullptr &&
        std::strcmp(
            event.file_path,
            "Assets/deferred_after_clear.txt") == 0;
}

struct FConsumeAndEnqueueProbe {
    CHotReloadWatcher* watcher = nullptr;
    u32 calls = 0u;
    EHotReloadResult enqueue_result = EHotReloadResult::Success;
    bool saw_a = false;
    bool consumed_b = false;
    bool saw_c = false;
    bool saw_d = false;
};

void ConsumeAndEnqueueCallback(
    void* user, const FHotReloadEvent& event) noexcept {
    auto* probe = static_cast<FConsumeAndEnqueueProbe*>(user);
    ++probe->calls;
    if (probe->calls == 1u) {
        probe->saw_a =
            event.file_path != nullptr &&
            std::strcmp(event.file_path, "Assets/a.txt") == 0;
        FHotReloadEvent consumed{};
        probe->consumed_b =
            probe->watcher->ConsumeNextEvent(consumed) &&
            consumed.file_path != nullptr &&
            std::strcmp(consumed.file_path, "Assets/b.txt") == 0;
        probe->enqueue_result =
            probe->watcher->TryEnqueueEvent("Assets/d.txt", 4u);
        return;
    }
    if (probe->calls == 2u) {
        probe->saw_c =
            event.file_path != nullptr &&
            std::strcmp(event.file_path, "Assets/c.txt") == 0;
        return;
    }
    if (probe->calls == 3u) {
        probe->saw_d =
            event.file_path != nullptr &&
            std::strcmp(event.file_path, "Assets/d.txt") == 0;
    }
}

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS) && !defined(ACS_GAME_SHIPPING)

// FILE_NOTIFY_INFORMATION 1 件を DWORD 境界まで padding した synthetic buffer に書く。
usize BuildSyntheticNativeRecord(
    u8* destination, usize capacity, u32 action,
    const wchar_t* file_name, usize file_name_length,
    u32 next_entry_offset = 0u) noexcept {
    constexpr usize kPrefix =
        offsetof(FILE_NOTIFY_INFORMATION, FileName);
    const usize name_bytes = file_name_length * sizeof(wchar_t);
    const usize raw_bytes = kPrefix + name_bytes;
    const usize record_bytes =
        (raw_bytes + alignof(DWORD) - 1u) &
        ~(static_cast<usize>(alignof(DWORD)) - 1u);
    if (destination == nullptr || file_name == nullptr ||
        file_name_length == 0u || record_bytes > capacity) {
        return 0u;
    }

    std::memset(destination, 0, record_bytes);
    auto* record =
        reinterpret_cast<FILE_NOTIFY_INFORMATION*>(destination);
    record->NextEntryOffset = next_entry_offset;
    record->Action = action;
    record->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(record->FileName, file_name, name_bytes);
    return record_bytes;
}

void RejectingEnqueueCallback(
    void* user, const FHotReloadEvent&) noexcept {
    auto* watcher = static_cast<CHotReloadWatcher*>(user);
    (void)watcher->TryEnqueueEvent(nullptr, 99u);
}

#endif

} // namespace

ACS_TEST(HotReloadSafety, ResultNamesAreStable) {
    struct FCase {
        EHotReloadResult result;
        const char* expected;
    };
    const FCase cases[] = {
        {EHotReloadResult::Success, "Success"},
        {EHotReloadResult::AlreadyRegistered, "AlreadyRegistered"},
        {EHotReloadResult::NotRegistered, "NotRegistered"},
        {EHotReloadResult::InvalidArgument, "InvalidArgument"},
        {EHotReloadResult::InvalidUtf8, "InvalidUtf8"},
        {EHotReloadResult::PathTooLong, "PathTooLong"},
        {EHotReloadResult::LimitExceeded, "LimitExceeded"},
        {EHotReloadResult::OutOfMemory, "OutOfMemory"},
        {EHotReloadResult::OsError, "OsError"},
        {EHotReloadResult::ReentrantCall, "ReentrantCall"},
        {EHotReloadResult::NativeOverflow, "NativeOverflow"},
    };
    static_assert(
        sizeof(cases) / sizeof(cases[0]) ==
        static_cast<usize>(EHotReloadResult::Count));
    for (const FCase& item : cases) {
        EXPECT_TRUE(std::strcmp(
            HotReloadResultName(item.result), item.expected) == 0);
    }
    EXPECT_TRUE(std::strcmp(
        HotReloadResultName(EHotReloadResult::Count),
        "Unknown") == 0);
    EXPECT_TRUE(std::strcmp(
        HotReloadResultName(static_cast<EHotReloadResult>(0xFFu)),
        "Unknown") == 0);
}

ACS_TEST(HotReloadSafety, NativeNotificationFieldsRejectLossyRecords) {
    using hot_reload_detail::IsValidNativeNotificationFields;

    for (u32 action = FILE_ACTION_ADDED;
         action <= FILE_ACTION_RENAMED_NEW_NAME;
         ++action) {
        EXPECT_TRUE(IsValidNativeNotificationFields(
            action, sizeof(wchar_t)));
    }

    EXPECT_FALSE(IsValidNativeNotificationFields(
        FILE_ACTION_ADDED, 0u));
    EXPECT_FALSE(IsValidNativeNotificationFields(
        FILE_ACTION_ADDED, sizeof(wchar_t) - 1u));
    EXPECT_FALSE(IsValidNativeNotificationFields(
        0u, sizeof(wchar_t)));
    EXPECT_FALSE(IsValidNativeNotificationFields(
        FILE_ACTION_RENAMED_NEW_NAME + 1u, sizeof(wchar_t)));
    EXPECT_FALSE(IsValidNativeNotificationFields(
        (std::numeric_limits<u32>::max)(), sizeof(wchar_t)));
}

ACS_TEST(HotReloadSafety, DiagnosticCounterIncrementSaturates) {
    u64 value = (std::numeric_limits<u64>::max)() - 1u;
    hot_reload_detail::SaturatingIncrement(value);
    EXPECT_EQ(value, (std::numeric_limits<u64>::max)());

    hot_reload_detail::SaturatingIncrement(value);
    EXPECT_EQ(value, (std::numeric_limits<u64>::max)());
}

ACS_TEST(HotReloadSafety, DiagnosticsAreStickyAndExplicitlyCleared) {
    CHotReloadWatcher watcher;
    u32 callback_calls = 0u;
    EXPECT_EQ(
        watcher.TryWatchFile("Assets/watched.txt"),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(&CountCallback, &callback_calls),
        EHotReloadResult::Success);
    FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 0u);
    EXPECT_EQ(diagnostics.coalesced_event_count, 0u);
    EXPECT_EQ(diagnostics.dispatched_event_count, 0u);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.loss_incident_count, 0u);
    EXPECT_EQ(diagnostics.last_failure, EHotReloadResult::Success);
    EXPECT_FALSE(diagnostics.authoritative_rescan_required);

    EXPECT_EQ(
        watcher.TryEnqueueEvent(nullptr, 1u),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/accepted.txt", 2u),
        EHotReloadResult::Success);

    diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 1u);
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::InvalidArgument);
    EXPECT_FALSE(diagnostics.authoritative_rescan_required);

    watcher.ClearDiagnostics();
    diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 0u);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.last_failure, EHotReloadResult::Success);
    EXPECT_FALSE(diagnostics.authoritative_rescan_required);
    EXPECT_EQ(watcher.WatchedCount(), 1u);
    EXPECT_EQ(watcher.PendingEventCount(), 1u);
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(callback_calls, 1u);

    EXPECT_EQ(
        watcher.TryEnqueueEvent(nullptr, 3u),
        EHotReloadResult::InvalidArgument);
    watcher.Shutdown();
    diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::InvalidArgument);
}

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS) && !defined(ACS_GAME_SHIPPING)

ACS_TEST(HotReloadSafety, NativeOverflowAndRearmFailureRequireRescan) {
    using internal::ConfigureHotReloadNativeFaultForTesting;
    using internal::EHotReloadNativeFaultForTesting;
    using internal::ResetHotReloadNativeFaultForTesting;

    ResetHotReloadNativeFaultForTesting();
    CHotReloadWatcher watcher;
    EXPECT_TRUE(ConfigureHotReloadNativeFaultForTesting(
        EHotReloadNativeFaultForTesting::NativeOverflow));
    EXPECT_EQ(
        watcher.TryTick(0.0f), EHotReloadResult::NativeOverflow);

    FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::NativeOverflow);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);

    watcher.ClearDiagnostics();
    EXPECT_TRUE(ConfigureHotReloadNativeFaultForTesting(
        EHotReloadNativeFaultForTesting::RearmFailure));
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::OsError);

    diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.last_failure, EHotReloadResult::OsError);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);
    ResetHotReloadNativeFaultForTesting();
}

ACS_TEST(HotReloadSafety, SyntheticParserRejectsOffsetAtBufferEnd) {
    using internal::ConfigureHotReloadNativeFaultForTesting;
    using internal::EHotReloadNativeFaultForTesting;
    using internal::ResetHotReloadNativeFaultForTesting;

    alignas(FILE_NOTIFY_INFORMATION) u8 bytes[64] = {};
    const wchar_t file_name[] = L"x.txt";
    const usize byte_count = BuildSyntheticNativeRecord(
        bytes, sizeof(bytes), FILE_ACTION_MODIFIED,
        file_name, (sizeof(file_name) / sizeof(file_name[0])) - 1u);
    EXPECT_TRUE(byte_count > 0u);
    auto* record =
        reinterpret_cast<FILE_NOTIFY_INFORMATION*>(bytes);
    record->NextEntryOffset = static_cast<DWORD>(byte_count);

    ResetHotReloadNativeFaultForTesting();
    EXPECT_TRUE(ConfigureHotReloadNativeFaultForTesting(
        EHotReloadNativeFaultForTesting::SyntheticBuffer,
        bytes, byte_count));

    CHotReloadWatcher watcher;
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::OsError);
    EXPECT_EQ(watcher.PendingEventCount(), 1u);

    const FHotReloadDiagnostics diagnostics =
        watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 1u);
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_EQ(diagnostics.last_failure, EHotReloadResult::OsError);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);
    ResetHotReloadNativeFaultForTesting();
}

ACS_TEST(HotReloadSafety, SyntheticNativeConversionOomIsDistinguished) {
    using internal::ConfigureHotReloadNativeFaultForTesting;
    using internal::EHotReloadNativeFaultForTesting;
    using internal::ResetHotReloadNativeFaultForTesting;

    alignas(FILE_NOTIFY_INFORMATION) u8 bytes[64] = {};
    const wchar_t file_name[] = L"oom.txt";
    const usize byte_count = BuildSyntheticNativeRecord(
        bytes, sizeof(bytes), FILE_ACTION_ADDED,
        file_name, (sizeof(file_name) / sizeof(file_name[0])) - 1u);
    EXPECT_TRUE(byte_count > 0u);

    ResetHotReloadNativeFaultForTesting();
    EXPECT_TRUE(ConfigureHotReloadNativeFaultForTesting(
        EHotReloadNativeFaultForTesting::
            SyntheticBufferConversionOutOfMemory,
        bytes, byte_count));

    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TryTick(0.0f), EHotReloadResult::OutOfMemory);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);

    const FHotReloadDiagnostics diagnostics =
        watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 0u);
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::OutOfMemory);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);
    ResetHotReloadNativeFaultForTesting();
}

ACS_TEST(HotReloadSafety, TickReturnPriorityDoesNotRewriteLastFailure) {
    using internal::ConfigureHotReloadNativeFaultForTesting;
    using internal::EHotReloadNativeFaultForTesting;
    using internal::ResetHotReloadNativeFaultForTesting;

    ResetHotReloadNativeFaultForTesting();
    EXPECT_TRUE(ConfigureHotReloadNativeFaultForTesting(
        EHotReloadNativeFaultForTesting::NativeOverflow));

    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &RejectingEnqueueCallback, &watcher),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/dispatch.txt", 1u),
        EHotReloadResult::Success);

    // TryTick の代表結果は native overflow を優先する。一方、callback 内で後から
    // 観測した InvalidArgument が last_failure であり、終了時に上書きしてはならない。
    EXPECT_EQ(
        watcher.TryTick(0.0f), EHotReloadResult::NativeOverflow);
    const FHotReloadDiagnostics diagnostics =
        watcher.CaptureDiagnostics();
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::InvalidArgument);
    EXPECT_EQ(diagnostics.dispatched_event_count, 1u);
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);
    ResetHotReloadNativeFaultForTesting();
}

#endif

ACS_TEST(HotReloadSafety, CheckedFileRegistrationOwnsAndDeduplicatesPaths) {
    CHotReloadWatcher watcher;
    char mutable_path[] = "owned.txt";

    EXPECT_EQ(
        watcher.TryWatchFile(mutable_path),
        EHotReloadResult::Success);
    mutable_path[0] = 'X';

    EXPECT_EQ(
        watcher.TryWatchFile("owned.txt"),
        EHotReloadResult::AlreadyRegistered);
    EXPECT_EQ(watcher.WatchedCount(), 1u);

    watcher.Unwatch("owned.txt");
    EXPECT_EQ(watcher.WatchedCount(), 0u);
}

ACS_TEST(HotReloadSafety, InvalidUtf8AndBoundariesAreRejectedTransactionally) {
    CHotReloadWatcher watcher;
    const char malformed[] = {
        static_cast<char>(0xC3), static_cast<char>(0x28), '\0'};
    char control[] = {'a', '\n', 'b', '\0'};
    char too_long[CHotReloadWatcher::kMaxPathBytes + 2u] = {};
    for (u32 i = 0u; i < CHotReloadWatcher::kMaxPathBytes + 1u; ++i) {
        too_long[i] = 'a';
    }

    EXPECT_EQ(
        watcher.TryWatchFile(nullptr),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TryWatchFile(""),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TryWatchFile(control),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TryWatchFile(malformed),
        EHotReloadResult::InvalidUtf8);
    EXPECT_EQ(
        watcher.TryWatchFile(too_long),
        EHotReloadResult::PathTooLong);
    EXPECT_EQ(watcher.WatchedCount(), 0u);
}

ACS_TEST(HotReloadSafety, WatchedPathAndCallbackLimitsAreEnforced) {
    CHotReloadWatcher watcher;
    char path[64] = {};
    for (u32 i = 0u; i < CHotReloadWatcher::kMaxWatchedPaths; ++i) {
        (void)std::snprintf(path, sizeof(path), "Assets/item_%u.txt", i);
        EXPECT_EQ(
            watcher.TryWatchFile(path),
            EHotReloadResult::Success);
    }
    EXPECT_EQ(watcher.WatchedCount(), CHotReloadWatcher::kMaxWatchedPaths);
    EXPECT_EQ(
        watcher.TryWatchFile("Assets/overflow.txt"),
        EHotReloadResult::LimitExceeded);

    u32 callback_users[CHotReloadWatcher::kMaxCallbacks + 1u] = {};
    for (u32 i = 0u; i < CHotReloadWatcher::kMaxCallbacks; ++i) {
        EXPECT_EQ(
            watcher.TryRegisterCallback(&SecondCallback, &callback_users[i]),
            EHotReloadResult::Success);
    }
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &SecondCallback, &callback_users[CHotReloadWatcher::kMaxCallbacks]),
        EHotReloadResult::LimitExceeded);
}

ACS_TEST(HotReloadSafety, PendingQueueLimitIsEnforcedWithoutPartialAppend) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.0f),
        EHotReloadResult::Success);
    char path[64] = {};
    for (u32 i = 0u; i < CHotReloadWatcher::kMaxPendingEvents; ++i) {
        (void)std::snprintf(path, sizeof(path), "Assets/event_%u.txt", i);
        EXPECT_EQ(
            watcher.TryEnqueueEvent(path, static_cast<u64>(i)),
            EHotReloadResult::Success);
    }
    EXPECT_EQ(
        watcher.PendingEventCount(),
        CHotReloadWatcher::kMaxPendingEvents);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/event_overflow.txt", 9999u),
        EHotReloadResult::LimitExceeded);
    EXPECT_EQ(
        watcher.PendingEventCount(),
        CHotReloadWatcher::kMaxPendingEvents);

    const FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(
        diagnostics.enqueued_event_count,
        static_cast<u64>(CHotReloadWatcher::kMaxPendingEvents));
    EXPECT_EQ(diagnostics.rejected_event_count, 1u);
    EXPECT_EQ(diagnostics.loss_incident_count, 1u);
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::LimitExceeded);
    EXPECT_TRUE(diagnostics.authoritative_rescan_required);
}

ACS_TEST(HotReloadSafety, DirectoryOsFailureDoesNotPublishRegistration) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TryWatchDirectory("C:\\<>\\acs_hot_reload_missing"),
        EHotReloadResult::OsError);
    EXPECT_EQ(watcher.WatchedCount(), 0u);
}

ACS_TEST(HotReloadSafety, FileRegistrationCanPromoteToDirectoryWatcher) {
    wchar_t wide_path[MAX_PATH + 1u] = {};
    const DWORD count = ::GetTempPathW(MAX_PATH, wide_path);
    EXPECT_TRUE(count > 0u && count < MAX_PATH);
    if (count == 0u || count >= MAX_PATH) return;

    char utf8_path[(MAX_PATH + 1u) * 3u] = {};
    const int bytes = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_path,
        static_cast<int>(count), utf8_path,
        static_cast<int>(sizeof(utf8_path) - 1u),
        nullptr, nullptr);
    EXPECT_TRUE(bytes > 0);
    if (bytes <= 0) return;
    utf8_path[bytes] = '\0';

    CHotReloadWatcher watcher;
    EXPECT_EQ(watcher.TryWatchFile(utf8_path),
              EHotReloadResult::Success);
    EXPECT_EQ(watcher.TryWatchDirectory(utf8_path, false),
              EHotReloadResult::Success);
    EXPECT_EQ(watcher.WatchedCount(), 1u);
    EXPECT_EQ(watcher.TryWatchDirectory(utf8_path, false),
              EHotReloadResult::AlreadyRegistered);
}

ACS_TEST(HotReloadSafety, CallbackRegistrationIsCheckedAndRemovable) {
    CHotReloadWatcher watcher;
    int user = 0;

    EXPECT_EQ(
        watcher.TryRegisterCallback(nullptr, &user),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TryRegisterCallback(&SecondCallback, &user),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(&SecondCallback, &user),
        EHotReloadResult::AlreadyRegistered);
    EXPECT_EQ(
        watcher.UnregisterCallback(&SecondCallback, &user),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.UnregisterCallback(&SecondCallback, &user),
        EHotReloadResult::NotRegistered);
}

ACS_TEST(HotReloadSafety, DebounceConfigurationAndTickRejectNonFiniteValues) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.25f),
        EHotReloadResult::Success);
    EXPECT_NEAR(watcher.DebounceSeconds(), 0.25f, 1e-6f);

    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(-0.01f),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(
            std::numeric_limits<f32>::infinity()),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(
            std::numeric_limits<f32>::quiet_NaN()),
        EHotReloadResult::InvalidArgument);
    EXPECT_NEAR(watcher.DebounceSeconds(), 0.25f, 1e-6f);

    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/kept.txt", 10u),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryTick(std::numeric_limits<f32>::quiet_NaN()),
        EHotReloadResult::InvalidArgument);
    EXPECT_EQ(watcher.PendingEventCount(), 1u);
}

ACS_TEST(HotReloadSafety, SamePathBurstsCoalesceAndConsumeOwnsPath) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.10f),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/a.txt", 1000u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/a.txt", 1050u, true),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/b.txt", 1051u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(watcher.PendingEventCount(), 2u);

    const FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 2u);
    EXPECT_EQ(diagnostics.coalesced_event_count, 1u);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.loss_incident_count, 0u);

    FHotReloadEvent event{};
    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_TRUE(event.file_path != nullptr);
    EXPECT_TRUE(std::strcmp(event.file_path, "Assets/a.txt") == 0);
    EXPECT_EQ(event.modified_timestamp, 1050u);
    EXPECT_TRUE(event.removed);

    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_TRUE(std::strcmp(event.file_path, "Assets/b.txt") == 0);
    EXPECT_FALSE(event.removed);
    EXPECT_FALSE(watcher.ConsumeNextEvent(event));
}

ACS_TEST(HotReloadSafety, DebounceCoalescesWithNewestQueuedSamePath) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.05f),
        EHotReloadResult::Success);

    // 最初の pair は debounce 窓外なので両方を保持する。3件目は最新 event だけに
    // 近いため、末尾追加や最古 event との比較ではなく最新 event を更新する。
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/repeated.txt", 0u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/repeated.txt", 1000u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/repeated.txt", 1020u, true),
        EHotReloadResult::Success);
    EXPECT_EQ(watcher.PendingEventCount(), 2u);

    FHotReloadEvent event{};
    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_TRUE(std::strcmp(event.file_path, "Assets/repeated.txt") == 0);
    EXPECT_EQ(event.modified_timestamp, 0u);
    EXPECT_FALSE(event.removed);

    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_TRUE(std::strcmp(event.file_path, "Assets/repeated.txt") == 0);
    EXPECT_EQ(event.modified_timestamp, 1020u);
    EXPECT_TRUE(event.removed);
    EXPECT_FALSE(watcher.ConsumeNextEvent(event));
}

ACS_TEST(HotReloadSafety, PositiveSubMillisecondDebounceUsesOneClockTick) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.0005f),
        EHotReloadResult::Success);

    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/sub_ms.txt", 1000u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/sub_ms.txt", 1001u, true),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/sub_ms.txt", 1003u, false),
        EHotReloadResult::Success);
    EXPECT_EQ(watcher.PendingEventCount(), 2u);

    FHotReloadEvent event{};
    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_EQ(event.modified_timestamp, 1001u);
    EXPECT_TRUE(event.removed);
    EXPECT_TRUE(watcher.ConsumeNextEvent(event));
    EXPECT_EQ(event.modified_timestamp, 1003u);
    EXPECT_FALSE(event.removed);
    EXPECT_FALSE(watcher.ConsumeNextEvent(event));
}

ACS_TEST(HotReloadSafety, CallbackDispatchRejectsTickReentryAndHonorsUnregister) {
    CHotReloadWatcher watcher;
    FDispatchProbe probe{};
    probe.watcher = &watcher;

    EXPECT_EQ(
        watcher.TryRegisterCallback(&FirstCallback, &probe),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(&SecondCallback, &probe),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/reentrant.txt", 42u),
        EHotReloadResult::Success);
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);

    EXPECT_EQ(probe.first_calls, 1u);
    EXPECT_EQ(probe.second_calls, 0u);
    EXPECT_EQ(probe.nested_tick, EHotReloadResult::ReentrantCall);
    EXPECT_TRUE(probe.saw_expected_path);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);

    const FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 1u);
    EXPECT_EQ(diagnostics.dispatched_event_count, 1u);
    EXPECT_EQ(
        diagnostics.last_failure, EHotReloadResult::ReentrantCall);
    EXPECT_FALSE(diagnostics.authoritative_rescan_required);
}

ACS_TEST(HotReloadSafety, ShutdownFromCallbackAbortsRemainingDispatchSafely) {
    CHotReloadWatcher watcher;
    EXPECT_EQ(
        watcher.TryWatchFile("Assets/watched.txt"),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(&ShutdownCallback, &watcher),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/one.txt", 1u),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/two.txt", 2u),
        EHotReloadResult::Success);

    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(watcher.WatchedCount(), 0u);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);
}

ACS_TEST(HotReloadSafety, ClearAndEnqueueFromCallbackDefersReplacementEvent) {
    CHotReloadWatcher watcher;
    FClearAndEnqueueProbe probe{&watcher};
    u32 observer_calls = 0u;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.0f),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &ClearAndEnqueueCallback, &probe),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &CountCallback, &observer_calls),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/a.txt", 1u),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/b.txt", 2u),
        EHotReloadResult::Success);

    // A の callback が B を clear して C を enqueue しても、C は開始時の B の
    // dispatch 枠を消費しない。現在 A の残り callback だけは完走する。
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_TRUE(probe.saw_initial);
    EXPECT_EQ(probe.enqueue_result, EHotReloadResult::Success);
    EXPECT_EQ(observer_calls, 1u);
    EXPECT_EQ(watcher.PendingEventCount(), 1u);

    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 2u);
    EXPECT_TRUE(probe.saw_deferred);
    EXPECT_EQ(observer_calls, 2u);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);

    const FHotReloadDiagnostics diagnostics =
        watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 3u);
    EXPECT_EQ(diagnostics.dispatched_event_count, 2u);
}

ACS_TEST(HotReloadSafety, ConsumeAndEnqueueFromCallbackPreservesNextTickFifo) {
    CHotReloadWatcher watcher;
    FConsumeAndEnqueueProbe probe{&watcher};
    u32 observer_calls = 0u;
    EXPECT_EQ(
        watcher.TrySetDebounceSeconds(0.0f),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &ConsumeAndEnqueueCallback, &probe),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryRegisterCallback(
            &CountCallback, &observer_calls),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/a.txt", 1u),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/b.txt", 2u),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/c.txt", 3u),
        EHotReloadResult::Success);

    // A の callback が B を手動 consume して D を追加しても、現在 A の observer は
    // 完走する。開始時 queue に残る C と新規 D は、次 tick へ FIFO のまま延期する。
    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_TRUE(probe.saw_a);
    EXPECT_TRUE(probe.consumed_b);
    EXPECT_EQ(probe.enqueue_result, EHotReloadResult::Success);
    EXPECT_EQ(observer_calls, 1u);
    EXPECT_EQ(watcher.PendingEventCount(), 2u);

    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 3u);
    EXPECT_TRUE(probe.saw_c);
    EXPECT_TRUE(probe.saw_d);
    EXPECT_EQ(observer_calls, 3u);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);

    const FHotReloadDiagnostics diagnostics =
        watcher.CaptureDiagnostics();
    EXPECT_EQ(diagnostics.enqueued_event_count, 4u);
    EXPECT_EQ(diagnostics.coalesced_event_count, 0u);
    EXPECT_EQ(diagnostics.dispatched_event_count, 3u);
    EXPECT_EQ(diagnostics.rejected_event_count, 0u);
    EXPECT_EQ(diagnostics.loss_incident_count, 0u);
}

ACS_TEST(HotReloadSafety, CallbackEnqueuedEventsAreDeferredToNextTick) {
    CHotReloadWatcher watcher;
    FEnqueueProbe probe{&watcher, 0u};
    EXPECT_EQ(
        watcher.TryRegisterCallback(&EnqueueCallback, &probe),
        EHotReloadResult::Success);
    EXPECT_EQ(
        watcher.TryEnqueueEvent("Assets/initial.txt", 1u),
        EHotReloadResult::Success);

    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_EQ(watcher.PendingEventCount(), 1u);

    EXPECT_EQ(watcher.TryTick(0.0f), EHotReloadResult::Success);
    EXPECT_EQ(probe.calls, 2u);
    EXPECT_EQ(watcher.PendingEventCount(), 0u);
}
