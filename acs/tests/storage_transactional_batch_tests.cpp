// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "container/StringView.h"
#include "foundation/Platform.h"
#include "memory/SystemAllocator.h"
#include "platform/FileSystem.h"
#include "platform/Storage.h"
#include "platform/StorageStringBatchEntry.h"

#include <cstddef>
#include <cstdio>
#include <type_traits>

using namespace acs;

static_assert(sizeof(FStorageStringBatchEntry) == sizeof(const char*) * 2u);
static_assert(alignof(FStorageStringBatchEntry) == alignof(const char*));
static_assert(offsetof(FStorageStringBatchEntry, key) == 0u);
static_assert(offsetof(FStorageStringBatchEntry, value) == sizeof(const char*));
static_assert(std::is_standard_layout_v<FStorageStringBatchEntry>);
static_assert(std::is_trivially_copyable_v<FStorageStringBatchEntry>);
static_assert(std::is_aggregate_v<FStorageStringBatchEntry>);
static_assert(FStorage::kMaximumStringBatchEntryCount == 4096u);
#if defined(_WIN64)
static_assert(sizeof(FStorage) == 40u);
#endif

namespace {

/** 指定した確保要求だけを失敗させる試験用 allocator。 */
class CStorageStringBatchFailAllocator final : public IAllocator {
public:
    /**
     * 次の一括設定で失敗させる確保要求番号を指定する。
     *
     * @param request 一から始まる確保要求番号。0 は失敗なし。
     */
    void FailOnRequest(u64 request) noexcept
    {
        m_RequestCount = 0u;
        m_FailingRequest = request;
    }

    /** 確保失敗を解除し、要求番号を数え直す。 */
    void DisableFailure() noexcept
    {
        m_RequestCount = 0u;
        m_FailingRequest = 0u;
    }

    /**
     * 確保要求を数え、指定番号以外を実 allocator へ委譲する。
     *
     * @param size 確保するバイト数。
     * @param alignment 要求する境界位置。
     * @param location 確保要求の発行位置。
     * @return 指定番号なら nullptr、それ以外は実確保結果。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        ++m_RequestCount;
        if (m_FailingRequest != 0u && m_RequestCount == m_FailingRequest) return nullptr;
        return m_Backing.Alloc(size, alignment, location);
    }

    /**
     * 試験中に確保した領域を実 allocator へ返す。
     *
     * @param pointer 解放する領域。
     */
    void Free(void* pointer) noexcept override
    {
        m_Backing.Free(pointer);
    }

    /** 現在生存している試験用確保の合計バイト数を返す。 */
    u64 BytesAllocated() const noexcept override
    {
        return m_Backing.BytesAllocated();
    }

    /** FailOnRequest 以降に受けた確保要求数を返す。 */
    u64 RequestCount() const noexcept
    {
        return m_RequestCount;
    }

private:
    /** 実際の確保と追跡を担う allocator。 */
    CSystemAllocator m_Backing;

    /** 現在の一括設定で受けた確保要求数。 */
    u64 m_RequestCount = 0u;

    /** 意図的に失敗させる一から始まる要求番号。 */
    u64 m_FailingRequest = 0u;
};

/** 二つの終端文字列が同じ内容かを返す。 */
bool StorageBatchTextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return left == right;
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

/** 一意な絶対一時ファイルを所有し、テスト終了時のcleanup失敗を記録する。 */
class CStorageBatchTemporaryFile final {
public:
    /** Win32の一時directoryに空の通常ファイルを作り、競合しない絶対pathを確保する。 */
    CStorageBatchTemporaryFile() noexcept
    {
        /** OSが返す一時directory。 */
        wchar_t temporaryDirectory[MAX_PATH] = {};

        /** Win32へ渡すpathバッファ容量。 */
        constexpr DWORD kCapacity = static_cast<DWORD>(MAX_PATH);

        /** 一時directory pathの文字数。 */
        const DWORD directoryLength = ::GetTempPathW(kCapacity, temporaryDirectory);
        if (directoryLength == 0u || directoryLength >= kCapacity) return;
        if (::GetTempFileNameW(temporaryDirectory, L"acs", 0u, m_Path) == 0u) m_Path[0] = L'\0';
    }

    /** 一時pathのcleanup責務を複製しない。 */
    CStorageBatchTemporaryFile(const CStorageBatchTemporaryFile&) = delete;
    CStorageBatchTemporaryFile& operator=(const CStorageBatchTemporaryFile&) = delete;
    CStorageBatchTemporaryFile(CStorageBatchTemporaryFile&&) = delete;
    CStorageBatchTemporaryFile& operator=(CStorageBatchTemporaryFile&&) = delete;

    /** 所有する通常ファイルを削除し、失敗を現在のtestへ記録する。 */
    ~CStorageBatchTemporaryFile() noexcept
    {
        if (!TryCleanup()) test::RecordFailure(FSourceLoc::Current(), "storage batch temporary file cleanup", "Win32 cleanup failed");
    }

    /** 一意な絶対pathを確保できた場合にtrueを返す。 */
    bool IsValid() const noexcept
    {
        return m_Path[0] != L'\0';
    }

    /** 所有する一時ファイルの絶対pathを返す。 */
    const wchar_t* Get() const noexcept
    {
        return m_Path;
    }

private:
    /** 通常ファイルの不存在を成功扱いにして、残存時だけ削除する。 */
    bool TryCleanup() noexcept
    {
        if (!IsValid()) return true;
        if (::DeleteFileW(m_Path) != FALSE) return true;

        /** 削除失敗時のWin32 error。 */
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    /** GetTempFileNameWが作った一意な通常ファイルの絶対path。 */
    wchar_t m_Path[MAX_PATH] = {};
};

} // namespace

ACS_TEST(StorageTransactionalBatch, AcceptsZeroMaximumAndRejectsBeyondLimit)
{
    /** 上限時の無確保経路も検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** 上限境界を検証する保存領域。 */
    FStorage storage(allocator);

    /** 空入力の成功結果。 */
    auto emptyResult = storage.TrySetStringBatch(nullptr, 0u);
    EXPECT_TRUE(emptyResult.IsOk());
    if (emptyResult.IsOk()) EXPECT_EQ(emptyResult.Value(), static_cast<usize>(0u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(0u));

    /** 上限件数の各キーを保持する固定領域。 */
    char keys[FStorage::kMaximumStringBatchEntryCount][24] = {};

    /** 上限件数の一括設定項目。 */
    FStorageStringBatchEntry entries[FStorage::kMaximumStringBatchEntryCount] = {};
    for (usize index = 0u; index < FStorage::kMaximumStringBatchEntryCount; ++index) {
        std::snprintf(keys[index], sizeof(keys[index]), "batch.key.%04zu", index);
        entries[index] = FStorageStringBatchEntry{keys[index], "stored"};
    }

    /** 上限ちょうどの一括設定結果。 */
    auto maximumResult = storage.TrySetStringBatch(entries, FStorage::kMaximumStringBatchEntryCount);
    EXPECT_TRUE(maximumResult.IsOk());
    if (maximumResult.IsOk()) EXPECT_EQ(maximumResult.Value(), FStorage::kMaximumStringBatchEntryCount);
    EXPECT_EQ(storage.Count(), FStorage::kMaximumStringBatchEntryCount);
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString(keys[0], nullptr), "stored"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString(keys[FStorage::kMaximumStringBatchEntryCount - 1u], nullptr), "stored"));

    /** 上限時の no-op 前から借用している値。 */
    const char* const borrowedBeforeNoopAtLimit = storage.GetString(keys[0], nullptr);

    /** 上限時の no-op 前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeNoopAtLimit = allocator.BytesAllocated();

    /** 上限件数を変えない全同値項目。 */
    const FStorageStringBatchEntry noopAtLimit{keys[0], "stored"};
    allocator.FailOnRequest(1u);

    /** 上限時の全同値入力結果。 */
    auto noopAtLimitResult = storage.TrySetStringBatch(&noopAtLimit, 1u);

    /** 上限時の全同値入力が発行した確保要求数。 */
    const u64 noopAtLimitRequestCount = allocator.RequestCount();
    allocator.DisableFailure();
    EXPECT_TRUE(noopAtLimitResult.IsOk());
    if (noopAtLimitResult.IsOk()) EXPECT_EQ(noopAtLimitResult.Value(), static_cast<usize>(0u));
    EXPECT_EQ(noopAtLimitRequestCount, static_cast<u64>(0u));
    EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeNoopAtLimit);
    EXPECT_TRUE(storage.GetString(keys[0], nullptr) == borrowedBeforeNoopAtLimit);

    /** 上限件数のまま既存値だけを更新する項目。 */
    const FStorageStringBatchEntry updateAtLimit{keys[FStorage::kMaximumStringBatchEntryCount - 1u], "updated"};

    /** 最終件数を増やさない上限時更新の結果。 */
    auto updateAtLimitResult = storage.TrySetStringBatch(&updateAtLimit, 1u);
    EXPECT_TRUE(updateAtLimitResult.IsOk());
    if (updateAtLimitResult.IsOk()) EXPECT_EQ(updateAtLimitResult.Value(), static_cast<usize>(1u));
    EXPECT_EQ(storage.Count(), FStorage::kMaximumStringBatchEntryCount);
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString(keys[FStorage::kMaximumStringBatchEntryCount - 1u], nullptr), "updated"));

    /** 上限超過失敗前から借用している値。 */
    const char* const borrowedValue = storage.GetString(keys[0], nullptr);

    /** 最終件数を一件超過させる新規項目。 */
    const FStorageStringBatchEntry newBeyondLimit{"batch.key.overflow", "rejected"};
    allocator.FailOnRequest(1u);

    /** 一件入力でも最終件数が上限を超える拒否結果。 */
    auto newBeyondLimitResult = storage.TrySetStringBatch(&newBeyondLimit, 1u);

    /** 上限超過の拒否までに発行した確保要求数。 */
    const u64 newBeyondLimitRequestCount = allocator.RequestCount();
    allocator.DisableFailure();
    EXPECT_TRUE(newBeyondLimitResult.IsErr());
    if (newBeyondLimitResult.IsErr()) {
        EXPECT_TRUE(newBeyondLimitResult.Error().category == EErrCategory::Container);
        EXPECT_EQ(newBeyondLimitResult.Error().subcode, static_cast<u16>(147u));
    }
    EXPECT_EQ(newBeyondLimitRequestCount, static_cast<u64>(0u));
    EXPECT_EQ(storage.Count(), FStorage::kMaximumStringBatchEntryCount);
    EXPECT_TRUE(storage.GetString(keys[0], nullptr) == borrowedValue);

    /** 上限を一件超える拒否結果。 */
    auto beyondLimitResult = storage.TrySetStringBatch(entries, FStorage::kMaximumStringBatchEntryCount + 1u);
    EXPECT_TRUE(beyondLimitResult.IsErr());
    EXPECT_EQ(storage.Count(), FStorage::kMaximumStringBatchEntryCount);
    EXPECT_TRUE(storage.GetString(keys[0], nullptr) == borrowedValue);
}

ACS_TEST(StorageTransactionalBatch, RejectsEveryNonEmptyBatchWhenExistingStateExceedsLimit)
{
    /** 既存上限超過の拒否が無確保であることを検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** 旧単体 setter で上限を超えさせる保存領域。 */
    FStorage storage(allocator);

    /** 全拒否操作の前後で借用する長い既存値。 */
    constexpr const char* kBorrowedValue = "stable borrowed value across every rejected oversized batch";

    /** nullptr 変更候補の前後で維持する非空値。 */
    constexpr const char* kNullChangeValue = "non-empty value used by the null change candidate";

    /** 旧 TrySetString へ順次渡すキー。 */
    char key[48] = {};
    for (usize index = 0u; index < FStorage::kMaximumStringBatchEntryCount; ++index) {
        std::snprintf(key, sizeof(key), "oversized.key.%04zu", index);

        /** 境界拒否後も維持する現在キーの初期値。 */
        const char* initialValue = "ordinary existing value";
        if (index == 0u) {
            initialValue = kBorrowedValue;
        } else if (index == 1u) {
            initialValue = nullptr;
        } else if (index == 2u) {
            initialValue = kNullChangeValue;
        }
        EXPECT_TRUE(storage.TrySetString(key, initialValue));
    }

    /** 旧 SetString で追加する上限超過分のキー。 */
    constexpr const char* kLegacyAddedKey = "oversized.key.4096";

    /** 旧 SetString で追加する上限超過分の値。 */
    constexpr const char* kLegacyAddedValue = "legacy SetString value beyond the batch limit";
    storage.SetString(kLegacyAddedKey, kLegacyAddedValue);

    /** 旧 setter で作った上限超過件数。 */
    constexpr usize kOversizedCount = FStorage::kMaximumStringBatchEntryCount + 1u;
    EXPECT_EQ(storage.Count(), kOversizedCount);

    /** 全拒否操作の前から借用している値。 */
    const char* const borrowedValue = storage.GetString("oversized.key.0000", nullptr);

    /** 全拒否操作の前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeRejectedBatches = allocator.BytesAllocated();

    /** 各入力が最終件数検査で無確保拒否され、全公開状態を維持することを確認する。 */
    const auto expectRejectedWithoutMutation = [&](const FStorageStringBatchEntry* entries, usize count) noexcept {
        allocator.FailOnRequest(1u);

        /** 現在の上限超過ストアへ一括設定した結果。 */
        auto result = storage.TrySetStringBatch(entries, count);

        /** 最終件数拒否までに発行した確保要求数。 */
        const u64 requestCount = allocator.RequestCount();
        allocator.DisableFailure();
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) {
            EXPECT_TRUE(result.Error().category == EErrCategory::Container);
            EXPECT_EQ(result.Error().subcode, static_cast<u16>(147u));
        }
        EXPECT_EQ(requestCount, static_cast<u64>(0u));
        EXPECT_EQ(storage.Count(), kOversizedCount);
        EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeRejectedBatches);
        EXPECT_TRUE(storage.GetString("oversized.key.0000", nullptr) == borrowedValue);
        EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("oversized.key.0000", nullptr), kBorrowedValue));
        EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("oversized.key.0001", nullptr), ""));
        EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("oversized.key.0002", nullptr), kNullChangeValue));
        EXPECT_TRUE(StorageBatchTextEquals(storage.GetString(kLegacyAddedKey, nullptr), kLegacyAddedValue));
        EXPECT_FALSE(storage.Has("oversized.key.new"));
    };

    /** 複数の既存値がすべて同じ全 no-op 入力。 */
    const FStorageStringBatchEntry allNoopEntries[] = {{"oversized.key.0000", kBorrowedValue}, {"oversized.key.0002", kNullChangeValue}, {kLegacyAddedKey, kLegacyAddedValue}};
    expectRejectedWithoutMutation(allNoopEntries, sizeof(allNoopEntries) / sizeof(allNoopEntries[0]));

    /** 既存空値へ nullptr を設定する no-op 入力。 */
    const FStorageStringBatchEntry nullAsEmptyNoop{"oversized.key.0001", nullptr};
    expectRejectedWithoutMutation(&nullAsEmptyNoop, 1u);

    /** 既存非空値を nullptr で空にする変更候補。 */
    const FStorageStringBatchEntry nullAsEmptyChange{"oversized.key.0002", nullptr};
    expectRejectedWithoutMutation(&nullAsEmptyChange, 1u);

    /** 既存値を別の非空値へ変える更新候補。 */
    const FStorageStringBatchEntry existingUpdate{"oversized.key.0000", "replacement that must never be committed"};
    expectRejectedWithoutMutation(&existingUpdate, 1u);

    /** 最終件数をさらに増やす新規追加候補。 */
    const FStorageStringBatchEntry newEntry{"oversized.key.new", "new value that must never be committed"};
    expectRejectedWithoutMutation(&newEntry, 1u);
}

ACS_TEST(StorageTransactionalBatch, RejectsInvalidRangesKeysAndDuplicatesWithoutMutation)
{
    /** 失敗時不変を確認する保存領域。 */
    FStorage storage;
    EXPECT_TRUE(storage.TrySetString("stable.key", "stable value that remains borrowed"));

    /** 全拒否操作の前から借用している値。 */
    const char* const borrowedValue = storage.GetString("stable.key", nullptr);

    /** 不正な二バイト UTF-8 列。 */
    const char invalidUtf8[] = {static_cast<char>(0xC0u), static_cast<char>(0xAFu), '\0'};

    /** ASCII 制御文字を含むキー。 */
    const char controlKey[] = {'b', 'a', 'd', static_cast<char>(0x01u), '\0'};

    /** Unicode の改行区切りを含むキー。 */
    const char unicodeLineSeparatorKey[] = {'b', 'a', 'd', static_cast<char>(0xE2u), static_cast<char>(0x80u), static_cast<char>(0xA8u), '\0'};

    /** 一件ずつ拒否する不正キー。 */
    const char* const invalidKeys[] = {nullptr, "", invalidUtf8, "line\nbreak", "has=equals", controlKey, unicodeLineSeparatorKey, "#comment", ";comment", "[section]"};
    for (usize index = 0u; index < sizeof(invalidKeys) / sizeof(invalidKeys[0]); ++index) {
        /** 現在検証している不正項目。 */
        const FStorageStringBatchEntry invalidEntry{invalidKeys[index], "rejected"};

        /** 不正キーの拒否結果。 */
        auto invalidResult = storage.TrySetStringBatch(&invalidEntry, 1u);
        EXPECT_TRUE(invalidResult.IsErr());
        EXPECT_EQ(storage.Count(), static_cast<usize>(1u));
        EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);
        EXPECT_TRUE(StorageBatchTextEquals(borrowedValue, "stable value that remains borrowed"));
    }

    /** 同じキーを二度指定する不正入力。 */
    const FStorageStringBatchEntry duplicateEntries[] = {{"duplicate", "first"}, {"duplicate", "second"}};

    /** 重複キーの拒否結果。 */
    auto duplicateResult = storage.TrySetStringBatch(duplicateEntries, 2u);
    EXPECT_TRUE(duplicateResult.IsErr());
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);

    /** 非整列配列を作るための固定バイト領域。 */
    alignas(FStorageStringBatchEntry) u8 misalignedBytes[sizeof(FStorageStringBatchEntry) + alignof(FStorageStringBatchEntry)] = {};

    /** 一バイトずらした非整列入力位置。 */
    const auto* const misalignedEntries = reinterpret_cast<const FStorageStringBatchEntry*>(misalignedBytes + 1u);

    /** 非整列配列の拒否結果。 */
    auto misalignedResult = storage.TrySetStringBatch(misalignedEntries, 1u);
    EXPECT_TRUE(misalignedResult.IsErr());
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);

    /** 配列バイト数が usize を超える項目数。 */
    const usize byteOverflowCount = (~usize(0)) / sizeof(FStorageStringBatchEntry) + 1u;

    /** 配列バイト数超過の拒否結果。 */
    auto byteOverflowResult = storage.TrySetStringBatch(duplicateEntries, byteOverflowCount);
    EXPECT_TRUE(byteOverflowResult.IsErr());
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);

    /** 整列を保ちながら配列終端が数値アドレスを超える先頭位置。 */
    const uptr overflowingAddress = (~uptr(0)) & ~(static_cast<uptr>(alignof(FStorageStringBatchEntry)) - 1u);

    /** 数値アドレス超過を起こす入力位置。 */
    const auto* const overflowingEntries = reinterpret_cast<const FStorageStringBatchEntry*>(overflowingAddress);

    /** 配列アドレス超過の拒否結果。 */
    auto addressOverflowResult = storage.TrySetStringBatch(overflowingEntries, 2u);
    EXPECT_TRUE(addressOverflowResult.IsErr());
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);

    /** null 配列と非0件数の拒否結果。 */
    auto nullEntriesResult = storage.TrySetStringBatch(nullptr, 1u);
    EXPECT_TRUE(nullEntriesResult.IsErr());
    EXPECT_EQ(storage.Count(), static_cast<usize>(1u));
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedValue);
}

ACS_TEST(StorageTransactionalBatch, RejectsLoaderTrimmedBoundarySpacesWithoutAllocationOrMutation)
{
    /** key 境界の事前拒否が無確保であることを検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** loader trim と衝突する入力の拒否前後を比較する保存領域。 */
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString("stable.key", "stable value that remains borrowed"));
    EXPECT_TRUE(storage.TrySetString("foo", "canonical foo value"));

    /** 全拒否操作の前から借用している安定値。 */
    const char* const borrowedStableValue = storage.GetString("stable.key", nullptr);

    /** trim 後の衝突対象から借用している既存値。 */
    const char* const borrowedFooValue = storage.GetString("foo", nullptr);

    /** 全拒否操作の前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeRejectedKeys = allocator.BytesAllocated();

    /** 境界 space を含む入力が事前検査で無確保拒否されることを確認する。 */
    const auto expectRejectedWithoutMutation = [&](const FStorageStringBatchEntry* entries, usize count) noexcept {
        allocator.FailOnRequest(1u);

        /** 現在の境界 space 入力を一括設定した結果。 */
        auto result = storage.TrySetStringBatch(entries, count);

        /** 事前拒否までに発行した確保要求数。 */
        const u64 requestCount = allocator.RequestCount();
        allocator.DisableFailure();
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) {
            EXPECT_TRUE(result.Error().category == EErrCategory::Container);
            EXPECT_EQ(result.Error().subcode, static_cast<u16>(145u));
        }
        EXPECT_EQ(requestCount, static_cast<u64>(0u));
        EXPECT_EQ(storage.Count(), static_cast<usize>(2u));
        EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeRejectedKeys);
        EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedStableValue);
        EXPECT_TRUE(storage.GetString("foo", nullptr) == borrowedFooValue);
        EXPECT_TRUE(StorageBatchTextEquals(borrowedStableValue, "stable value that remains borrowed"));
        EXPECT_TRUE(StorageBatchTextEquals(borrowedFooValue, "canonical foo value"));
    };

    /** loader が identity を変える先頭・末尾 ASCII space の不正キー。 */
    const char* const boundarySpaceKeys[] = {" foo", "foo ", " ", " #comment", "[section] "};
    for (usize index = 0u; index < sizeof(boundarySpaceKeys) / sizeof(boundarySpaceKeys[0]); ++index) {
        /** 現在検証している境界 space 項目。 */
        const FStorageStringBatchEntry invalidEntry{boundarySpaceKeys[index], "rejected"};
        expectRejectedWithoutMutation(&invalidEntry, 1u);
    }

    /** Save/Load 後に二つの foo へ縮退する組み合わせ。 */
    const FStorageStringBatchEntry trimCollisionEntries[] = {{"foo", "canonical foo value"}, {"foo ", "must not become a duplicate"}};
    expectRejectedWithoutMutation(trimCollisionEntries, sizeof(trimCollisionEntries) / sizeof(trimCollisionEntries[0]));
    EXPECT_FALSE(storage.Has("foo "));
}

ACS_TEST(StorageTransactionalBatch, RejectsLegacyExistingAndBatchTrimCollisionWithoutAllocationOrMutation)
{
    /** legacy key 衝突の事前拒否が無確保であることを検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** 単体 setter で末尾 space 付き key を保持する保存領域。 */
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString("foo ", "legacy spaced value that remains borrowed"));
    EXPECT_TRUE(storage.TrySetString("stable.key", "stable value that remains borrowed"));

    /** batch の foo と正規化衝突する legacy 値の借用位置。 */
    const char* const borrowedLegacyValue = storage.GetString("foo ", nullptr);

    /** 衝突と無関係な既存値の借用位置。 */
    const char* const borrowedStableValue = storage.GetString("stable.key", nullptr);

    /** 拒否操作前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeCollision = allocator.BytesAllocated();

    /** loader trim 後に legacy の foo と衝突する canonical 入力。 */
    const FStorageStringBatchEntry collidingEntry{"foo", "replacement that must never be committed"};
    allocator.FailOnRequest(1u);

    /** 既存 key と batch key の正規化衝突結果。 */
    auto result = storage.TrySetStringBatch(&collidingEntry, 1u);

    /** 衝突拒否までに発行した確保要求数。 */
    const u64 requestCount = allocator.RequestCount();
    allocator.DisableFailure();
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_TRUE(result.Error().category == EErrCategory::Container);
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(158u));
    }
    EXPECT_EQ(requestCount, static_cast<u64>(0u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(2u));
    EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeCollision);
    EXPECT_TRUE(storage.GetString("foo ", nullptr) == borrowedLegacyValue);
    EXPECT_TRUE(storage.GetString("stable.key", nullptr) == borrowedStableValue);
    EXPECT_TRUE(StorageBatchTextEquals(borrowedLegacyValue, "legacy spaced value that remains borrowed"));
    EXPECT_TRUE(StorageBatchTextEquals(borrowedStableValue, "stable value that remains borrowed"));
    EXPECT_FALSE(storage.Has("foo"));
}

ACS_TEST(StorageTransactionalBatch, RejectsAlreadyCollidingExistingKeysWithoutAllocationOrMutation)
{
    /** 既存同士の衝突拒否が無確保であることを検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** legacy setter で loader trim 後に衝突する二つの key を保持する保存領域。 */
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString("foo", "canonical value that remains borrowed"));
    EXPECT_TRUE(storage.TrySetString("\tfoo ", "legacy duplicate value that remains borrowed"));

    /** canonical key から借用している既存値。 */
    const char* const borrowedCanonicalValue = storage.GetString("foo", nullptr);

    /** 境界 tab と space を持つ legacy key から借用している既存値。 */
    const char* const borrowedLegacyValue = storage.GetString("\tfoo ", nullptr);

    /** 拒否操作前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeCollision = allocator.BytesAllocated();

    /** 既存同士の衝突とは無関係な canonical 入力。 */
    const FStorageStringBatchEntry unrelatedEntry{"unrelated", "must not be committed"};
    allocator.FailOnRequest(1u);

    /** 既存 key 同士の正規化衝突結果。 */
    auto result = storage.TrySetStringBatch(&unrelatedEntry, 1u);

    /** 衝突拒否までに発行した確保要求数。 */
    const u64 requestCount = allocator.RequestCount();
    allocator.DisableFailure();
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_TRUE(result.Error().category == EErrCategory::Container);
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(157u));
    }
    EXPECT_EQ(requestCount, static_cast<u64>(0u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(2u));
    EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeCollision);
    EXPECT_TRUE(storage.GetString("foo", nullptr) == borrowedCanonicalValue);
    EXPECT_TRUE(storage.GetString("\tfoo ", nullptr) == borrowedLegacyValue);
    EXPECT_TRUE(StorageBatchTextEquals(borrowedCanonicalValue, "canonical value that remains borrowed"));
    EXPECT_TRUE(StorageBatchTextEquals(borrowedLegacyValue, "legacy duplicate value that remains borrowed"));
    EXPECT_FALSE(storage.Has("unrelated"));
}

ACS_TEST(StorageTransactionalBatch, AllowsUniqueLegacyBoundaryKeyAndLoadsItsTrimmedIdentity)
{
    /** 衝突しない legacy key の保存と読込を確認する保存先。 */
    CStorageBatchTemporaryFile savedPath;
    EXPECT_TRUE(savedPath.IsValid());
    if (!savedPath.IsValid()) return;

    /** 単体 setter の非 canonical key と canonical batch key を保持する保存領域。 */
    FStorage storage;
    EXPECT_TRUE(storage.TrySetString("foo ", "unique legacy value"));

    /** legacy key と正規化衝突しない一括設定項目。 */
    const FStorageStringBatchEntry unrelatedEntry{"bar", "canonical batch value"};

    /** 衝突しない legacy key を拒否せず canonical key を追加する結果。 */
    auto batchResult = storage.TrySetStringBatch(&unrelatedEntry, 1u);
    EXPECT_TRUE(batchResult.IsOk());
    if (batchResult.IsOk()) EXPECT_EQ(batchResult.Value(), static_cast<usize>(1u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(2u));
    EXPECT_TRUE(storage.Has("foo "));
    EXPECT_FALSE(storage.Has("foo"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("foo ", nullptr), "unique legacy value"));

    /** legacy key の raw identity を含む実ファイル保存結果。 */
    auto saveResult = storage.Save(savedPath.Get());
    EXPECT_TRUE(saveResult.IsOk());

    /** loader trim 後の衝突がない状態を復元する保存領域。 */
    FStorage loaded;

    /** unique legacy key を canonical identity へ trim する読込結果。 */
    auto loadResult = loaded.Load(savedPath.Get());
    EXPECT_TRUE(loadResult.IsOk());
    EXPECT_EQ(loaded.Count(), static_cast<usize>(2u));
    EXPECT_TRUE(loaded.Has("foo"));
    EXPECT_FALSE(loaded.Has("foo "));
    EXPECT_TRUE(loaded.Has("bar"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("foo", nullptr), "unique legacy value"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("bar", nullptr), "canonical batch value"));
}

ACS_TEST(StorageTransactionalBatch, ReportsOnlyActualChangesAndKeepsAllNoopPointers)
{
    /** no-op 時の無確保を検証する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** 変更、同値、新規を混在させる保存領域。 */
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString("update", "before"));
    EXPECT_TRUE(storage.TrySetString("same", "unchanged"));

    /** 実変更二件と同値一件を含む入力。 */
    const FStorageStringBatchEntry mixedEntries[] = {{"update", "after"}, {"same", "unchanged"}, {"new.empty", nullptr}};

    /** 混在入力の一括設定結果。 */
    auto mixedResult = storage.TrySetStringBatch(mixedEntries, 3u);
    EXPECT_TRUE(mixedResult.IsOk());
    if (mixedResult.IsOk()) EXPECT_EQ(mixedResult.Value(), static_cast<usize>(2u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(3u));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("update", nullptr), "after"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("same", nullptr), "unchanged"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("new.empty", nullptr), ""));

    /** 全同値操作の前から借用している値。 */
    const char* const borrowedSameValue = storage.GetString("same", nullptr);

    /** 全同値操作前に生存する確保の合計バイト数。 */
    const u64 bytesBeforeNoop = allocator.BytesAllocated();

    /** 単体 setter と同じ null 値契約を含む全同値入力。 */
    const FStorageStringBatchEntry noopEntries[] = {{"update", "after"}, {"same", "unchanged"}, {"new.empty", nullptr}};

    allocator.FailOnRequest(1u);

    /** 全同値入力の成功結果。 */
    auto noopResult = storage.TrySetStringBatch(noopEntries, 3u);
    allocator.DisableFailure();
    EXPECT_TRUE(noopResult.IsOk());
    if (noopResult.IsOk()) EXPECT_EQ(noopResult.Value(), static_cast<usize>(0u));
    EXPECT_TRUE(storage.GetString("same", nullptr) == borrowedSameValue);
    EXPECT_EQ(allocator.BytesAllocated(), bytesBeforeNoop);
}

ACS_TEST(StorageTransactionalBatch, PreservesNonNullValueBytesWithoutAdditionalValidation)
{
    /** byte 列の追加制約がないことを検証する保存領域。 */
    FStorage storage;

    /** UTF-8、INI key、制御文字の制約を値へ誤適用してはならない byte 列。 */
    const char valueBytes[] = {' ', '=', '\t', static_cast<char>(0xC0u), static_cast<char>(0x01u), '\0'};

    /** 既存単体 setter と同じ byte 列契約で追加する項目。 */
    const FStorageStringBatchEntry entry{"value.bytes", valueBytes};

    /** byte 列を追加検査なしで反映する結果。 */
    auto result = storage.TrySetStringBatch(&entry, 1u);
    EXPECT_TRUE(result.IsOk());
    if (result.IsOk()) EXPECT_EQ(result.Value(), static_cast<usize>(1u));
    EXPECT_EQ(storage.Count(), static_cast<usize>(1u));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("value.bytes", nullptr), valueBytes));
}

ACS_TEST(StorageTransactionalBatch, PreservesInternalAsciiSpaceKeyThroughSaveAndLoad)
{
    /** 内部 ASCII space を持つ key の往復確認先。 */
    CStorageBatchTemporaryFile savedPath;
    EXPECT_TRUE(savedPath.IsValid());
    if (!savedPath.IsValid()) return;

    /** 有効な内部 space key を保存する領域。 */
    FStorage storage;

    /** 境界ではなく内部にだけ ASCII space を持つ項目。 */
    const FStorageStringBatchEntry entry{"foo bar", "internal space value"};

    /** 内部 space key の一括設定結果。 */
    auto result = storage.TrySetStringBatch(&entry, 1u);
    EXPECT_TRUE(result.IsOk());
    if (result.IsOk()) EXPECT_EQ(result.Value(), static_cast<usize>(1u));

    /** 内部 space key の実ファイル保存結果。 */
    auto saveResult = storage.Save(savedPath.Get());
    EXPECT_TRUE(saveResult.IsOk());

    /** 保存した key の byte identity を確認する読み取り結果。 */
    auto savedTextResult = CFileSystem::ReadAllText(savedPath.Get());
    EXPECT_TRUE(savedTextResult.IsOk());
    if (savedTextResult.IsOk()) {
        EXPECT_TRUE(StorageBatchTextEquals(savedTextResult.Value().Data(), "# acs FStorage\nfoo bar=internal space value\n"));
    }

    /** 実ファイルから内部 space key を復元する保存領域。 */
    FStorage loaded;

    /** 内部 space key の読み戻し結果。 */
    auto loadResult = loaded.Load(savedPath.Get());
    EXPECT_TRUE(loadResult.IsOk());
    EXPECT_EQ(loaded.Count(), static_cast<usize>(1u));
    EXPECT_TRUE(loaded.Has("foo bar"));
    EXPECT_FALSE(loaded.Has("foo"));
    EXPECT_FALSE(loaded.Has("foobar"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("foo bar", nullptr), "internal space value"));
}

ACS_TEST(StorageTransactionalBatch, SaveKeepsExistingOrderAppendsBatchOrderAndRoundTrips)
{
    /** 一括反映後の実ファイル内容を確認する保存先。 */
    CStorageBatchTemporaryFile savedPath;

    /** Load 後に同値内容を再保存する確認先。 */
    CStorageBatchTemporaryFile roundTripPath;
    EXPECT_TRUE(savedPath.IsValid());
    EXPECT_TRUE(roundTripPath.IsValid());
    if (!savedPath.IsValid() || !roundTripPath.IsValid()) return;

    /** 既存順序、更新位置、新規追加順を組み立てる保存領域。 */
    FStorage storage;
    EXPECT_TRUE(storage.TrySetString("existing.first", "first bytes"));
    EXPECT_TRUE(storage.TrySetString("existing.update", "before bytes"));
    EXPECT_TRUE(storage.TrySetString("existing.last", "last bytes"));

    /** update を挟んだ二つの新規項目が batch 内順序で末尾へ並ぶ入力。 */
    const FStorageStringBatchEntry entries[] = {{"new.second", "second appended bytes"}, {"existing.update", "after bytes"}, {"new.first", "first appended bytes"}, {"existing.first", "first bytes"}};

    /** 更新一件と新規二件を反映する結果。 */
    auto result = storage.TrySetStringBatch(entries, sizeof(entries) / sizeof(entries[0]));
    EXPECT_TRUE(result.IsOk());
    if (result.IsOk()) EXPECT_EQ(result.Value(), static_cast<usize>(3u));

    /** 既存順序と batch 内新規順序を固定する実ファイル内容。 */
    constexpr const char* kExpectedSavedText = "# acs FStorage\n"
                                               "existing.first=first bytes\n"
                                               "existing.update=after bytes\n"
                                               "existing.last=last bytes\n"
                                               "new.second=second appended bytes\n"
                                               "new.first=first appended bytes\n";

    /** 一括反映後の実ファイル保存結果。 */
    auto saveResult = storage.Save(savedPath.Get());
    EXPECT_TRUE(saveResult.IsOk());

    /** 一括反映直後に保存した実ファイルの読み取り結果。 */
    auto savedTextResult = CFileSystem::ReadAllText(savedPath.Get());
    EXPECT_TRUE(savedTextResult.IsOk());
    if (savedTextResult.IsOk()) {
        EXPECT_TRUE(StorageBatchTextEquals(savedTextResult.Value().Data(), kExpectedSavedText));
    }

    /** 実ファイルから同値状態を復元する保存領域。 */
    FStorage loaded;

    /** 実ファイルの読み戻し結果。 */
    auto loadResult = loaded.Load(savedPath.Get());
    EXPECT_TRUE(loadResult.IsOk());
    EXPECT_EQ(loaded.Count(), static_cast<usize>(5u));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("existing.first", nullptr), "first bytes"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("existing.update", nullptr), "after bytes"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("existing.last", nullptr), "last bytes"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("new.second", nullptr), "second appended bytes"));
    EXPECT_TRUE(StorageBatchTextEquals(loaded.GetString("new.first", nullptr), "first appended bytes"));

    /** 読み戻した状態を別の実ファイルへ保存する結果。 */
    auto roundTripSaveResult = loaded.Save(roundTripPath.Get());
    EXPECT_TRUE(roundTripSaveResult.IsOk());

    /** Load/Save 後の同値ファイル内容を読み取る結果。 */
    auto roundTripTextResult = CFileSystem::ReadAllText(roundTripPath.Get());
    EXPECT_TRUE(roundTripTextResult.IsOk());
    if (roundTripTextResult.IsOk()) {
        EXPECT_TRUE(StorageBatchTextEquals(roundTripTextResult.Value().Data(), kExpectedSavedText));
    }
}

ACS_TEST(StorageTransactionalBatch, OwnsKeysAndValuesBorrowedFromTheSameStorage)
{
    /** 内部値を入力キーと入力値へ再利用する保存領域。 */
    FStorage storage;
    EXPECT_TRUE(storage.TrySetString("alias.key.source", "aliased.target.key"));
    EXPECT_TRUE(storage.TrySetString("alias.value.source", "aliased value copied before commit"));

    /** 保存領域内部から借用した設定キー。 */
    const char* const aliasedKey = storage.GetString("alias.key.source", nullptr);

    /** 保存領域内部から借用した設定値。 */
    const char* const aliasedValue = storage.GetString("alias.value.source", nullptr);
    EXPECT_TRUE(aliasedKey != nullptr);
    EXPECT_TRUE(aliasedValue != nullptr);

    /** 借用入力の追加と借用元の更新を同時に行う項目列。 */
    const FStorageStringBatchEntry aliasedEntries[] = {{aliasedKey, aliasedValue}, {"alias.key.source", "key source replaced"}, {"alias.value.source", aliasedKey}};

    /** 借用入力を所有化してから反映する結果。 */
    auto aliasedResult = storage.TrySetStringBatch(aliasedEntries, 3u);
    EXPECT_TRUE(aliasedResult.IsOk());
    if (aliasedResult.IsOk()) EXPECT_EQ(aliasedResult.Value(), static_cast<usize>(3u));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("aliased.target.key", nullptr), "aliased value copied before commit"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("alias.key.source", nullptr), "key source replaced"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("alias.value.source", nullptr), "aliased.target.key"));
}

ACS_TEST(StorageTransactionalBatch, EveryAllocationFailurePreservesStateAndBorrowedPointers)
{
    /** 確保要求番号ごとに失敗を注入する allocator。 */
    CStorageStringBatchFailAllocator allocator;

    /** 失敗前後の状態を比較する保存領域。 */
    FStorage storage(allocator);
    EXPECT_TRUE(storage.TrySetString("existing.key.with.allocator.backed.storage", "original value that remains valid through every failed request"));
    EXPECT_TRUE(storage.TrySetString("same.key.with.allocator.backed.storage", "same value that remains a no-op"));

    /** 全失敗中に同じアドレスで残る必要がある借用値。 */
    const char* const borrowedValue = storage.GetString("existing.key.with.allocator.backed.storage", nullptr);

    /** 更新、新規、同値を混在させた確保失敗検証入力。 */
    const FStorageStringBatchEntry entries[] = {{"existing.key.with.allocator.backed.storage", "replacement value that requires allocator backed storage"}, {"new.key.with.allocator.backed.storage", "new value that requires allocator backed storage"}, {"same.key.with.allocator.backed.storage", "same value that remains a no-op"}};

    /** 少なくとも一つの確保失敗を通過したか。 */
    bool observedFailure = false;

    /** 全確保を通過して一括設定に成功したか。 */
    bool observedSuccess = false;
    for (u64 request = 1u; request <= 64u; ++request) {
        /** 呼び出し前に保存領域が所有しているバイト数。 */
        const u64 bytesBefore = allocator.BytesAllocated();
        allocator.FailOnRequest(request);

        /** 現在の確保要求だけを失敗させた一括設定結果。 */
        auto result = storage.TrySetStringBatch(entries, 3u);
        allocator.DisableFailure();
        if (result.IsOk()) {
            observedSuccess = true;
            EXPECT_EQ(result.Value(), static_cast<usize>(2u));
            break;
        }

        observedFailure = true;
        EXPECT_TRUE(result.Error().category == EErrCategory::Memory);
        EXPECT_EQ(storage.Count(), static_cast<usize>(2u));
        EXPECT_EQ(allocator.BytesAllocated(), bytesBefore);
        EXPECT_TRUE(storage.GetString("existing.key.with.allocator.backed.storage", nullptr) == borrowedValue);
        EXPECT_TRUE(StorageBatchTextEquals(borrowedValue, "original value that remains valid through every failed request"));
        EXPECT_FALSE(storage.Has("new.key.with.allocator.backed.storage"));
    }

    EXPECT_TRUE(observedFailure);
    EXPECT_TRUE(observedSuccess);
    EXPECT_EQ(storage.Count(), static_cast<usize>(3u));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("existing.key.with.allocator.backed.storage", nullptr), "replacement value that requires allocator backed storage"));
    EXPECT_TRUE(StorageBatchTextEquals(storage.GetString("new.key.with.allocator.backed.storage", nullptr), "new value that requires allocator backed storage"));
}
