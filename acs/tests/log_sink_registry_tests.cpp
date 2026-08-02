// SPDX-License-Identifier: Apache-2.0
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "foundation/Platform.h"
#include "test/Expect.h"
#include "test/Test.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

using namespace acs;

#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
namespace acs::log_sink_test_detail {
/** 次回の購読表確保だけを失敗させる。 */
void FailNextSinkRegistryAllocation() noexcept;
/** 未初期化時の枠別世代を試験値へ置き換える。 */
bool SetSinkGeneration(u32 slot_index, u32 generation) noexcept;
/** 外部解除が callback 完了待機へ入ったことを通知する event を設定する。 */
void SetUnsubscribeWaitEvent(void* event_handle) noexcept;
/** Shutdown が writer 終了待機へ入ることを通知する event を設定する。 */
void SetShutdownWaitEvent(void* event_handle) noexcept;
/** 購読一覧コピーを registry 共有 lock 内で同期する event 対を設定する。 */
void SetCopySinkHandlesBarrierEvents(void* entered_event, void* release_event) noexcept;
}
#endif

namespace {

/** legacy と複数購読の呼び出し順を記録する共有先。 */
struct FLogSinkOrderContext {
    /** 呼び出された識別番号の列。 */
    u32 values[16]{};

    /** values に記録済みの要素数。 */
    u32 count = 0u;
};

/** callback 中の解除と追加を検証する共有状態。 */
struct FLogSinkMutationContext {
    /** 変更 callback より先に呼ばれる購読。 */
    FLogSinkHandle earlier{};

    /** 先行購読の後に変更を実行する自身の購読。 */
    FLogSinkHandle self{};

    /** 最初の巡回から除外する後続購読。 */
    FLogSinkHandle later{};

    /** callback 中に追加した次回用購読。 */
    FLogSinkHandle added{};

    /** 巡回完了後に再利用した購読。 */
    FLogSinkHandle recycled{};

    /** 呼び出し順の記録先。 */
    FLogSinkOrderContext order{};
};

/** 外部解除の callback 完了待機を検証する共有状態。 */
struct FLogSinkWaitContext {
    /** callback 開始を通知する manual-reset event。 */
    HANDLE entered_event = nullptr;

    /** callback の終了を許可する manual-reset event。 */
    HANDLE release_event = nullptr;

    /** 外部解除が復帰したか。 */
    TAtomic<u32> removed{0u};

    /** 外部スレッドが解除する購読。 */
    FLogSinkHandle handle{};
};

/** callback 内 lifecycle 操作の拒否と所有権破棄を検証する共有状態。 */
struct FLogSinkCallbackLifecycleContext {
    /** callback 内から再初期化を試す設定。 */
    FLogConfig config{};

    /** callback 内で明示破棄する所有権。 */
    FLogSinkSubscription* owned = nullptr;

    /** callback が全操作を終えたか。 */
    TAtomic<u32> completed{0u};
};

/** 別スレッド Shutdown の callback 完了待機を検証する共有状態。 */
struct FLogSinkShutdownContext {
    /** callback 開始を通知する manual-reset event。 */
    HANDLE entered_event = nullptr;

    /** callback の終了を許可する manual-reset event。 */
    HANDLE release_event = nullptr;

    /** Shutdown が復帰したか。 */
    TAtomic<u32> completed{0u};
};

/** nested Write と callback 中追加のレコード境界を記録する1件。 */
struct FNestedSinkEntry {
    /** 呼ばれた sink の識別番号。 */
    u32 sink = 0u;

    /** レコードに保存されていた重大度。 */
    ELogSeverity severity = ELogSeverity::Off;

    /** callback 中に所有コピーした null終端本文。 */
    char message[48]{};
};

/** nested Write と callback 中追加の記録先。 */
struct FNestedSinkContext {
    /** writer が通知した順序付きレコード。 */
    FNestedSinkEntry entries[8]{};

    /** entries に記録済みの要素数。 */
    u32 count = 0u;

    /** outer callback から nested Write を発行済みか。 */
    bool nested_written = false;

    /** outer callback 中に追加した購読。 */
    FLogSinkHandle added{};
};

/** message の所有コピー・重大度・null終端を event 同期で検証する状態。 */
struct FLogSinkMessageContext {
    /** callback 開始を通知する manual-reset event。 */
    HANDLE entered_event = nullptr;

    /** callback の本文読み取りを許可する manual-reset event。 */
    HANDLE release_event = nullptr;

    /** callback が受け取った重大度。 */
    ELogSeverity severity = ELogSeverity::Off;

    /** callback 中に保持用として複製した本文。 */
    char copied_message[64]{};

    /** 渡された本文が配列内で null終端されていたか。 */
    bool null_terminated = false;
};

/** allocation failure 後も legacy sink へ実送信できることを記録する状態。 */
struct FLegacyDeliveryContext {
    /** callback が受け取った重大度。 */
    ELogSeverity severity = ELogSeverity::Off;

    /** callback 中に複製した本文。 */
    char message[64]{};

    /** callback 呼び出し回数。 */
    u32 count = 0u;
};

/** TryCopySinkHandles と同時 mutation のコピー側状態。 */
struct FCopySinkHandlesContext {
    /** コピーされた登録順ハンドル。 */
    FLogSinkHandle output[4]{};

    /** コピーされたハンドル数。 */
    u32 output_count = 99u;

    /** TryCopySinkHandles の結果。 */
    bool copied = false;
};

/** TryCopySinkHandles と同時に解除・追加する側の状態。 */
struct FCopyMutationContext {
    /** 解除する既存購読。 */
    FLogSinkHandle removed{};

    /** 解除後に追加された購読。 */
    FLogSinkHandle added{};

    /** mutation 呼び出し開始を通知する event。 */
    HANDLE started_event = nullptr;

    /** 解除結果。 */
    bool remove_succeeded = false;

    /** worker が全処理を終えたか。 */
    TAtomic<u32> completed{0u};
};

/** 有限並行 lifecycle stress の共有状態。 */
struct FLogSinkStressContext {
    /** 全 worker が開始 barrier へ到達した数。 */
    TAtomic<u32> ready_count{0u};

    /** 全 worker の反復開始を許可する barrier。 */
    TAtomic<u32> start{0u};

    /** 有効 lifecycle で実行された callback 数。 */
    TAtomic<u32> callback_count{0u};

    /** lifecycle worker が再初期化に使う設定。 */
    FLogConfig config{};
};

/** legacy callback が使用する呼び出し順の記録先。 */
FLogSinkOrderContext* g_legacy_order = nullptr;

/** allocation failure 後の legacy 実送信を記録する非所有先。 */
FLegacyDeliveryContext* g_legacy_delivery = nullptr;

/** ファイルや画面へ出さない Logger 設定を返す。 */
FLogConfig QuietLogConfig() noexcept
{
    FLogConfig config{};
    config.console = false;
    config.debug_output = false;
    config.min_severity = ELogSeverity::Trace;
    config.ring_capacity = 32u;
    return config;
}

/** 後続テスト用に標準の Logger 状態を復元する。 */
void RestoreLogSinkTestLogger() noexcept
{
    CLogger::Shutdown();
    FLogConfig config{};
    config.console = true;
    config.debug_output = false;
    config.min_severity = ELogSeverity::Info;
    CLogger::Init(config);
}

/**
 * 指定識別番号を呼び出し順へ追記する。
 * @param context 追記先。
 * @param value 追記する識別番号。
 */
void AppendOrder(FLogSinkOrderContext& context, u32 value) noexcept
{
    if (context.count < 16u) context.values[context.count++] = value;
}

/** manual-reset の試験用 event を未通知状態で作る。 */
HANDLE CreateLogSinkTestEvent() noexcept
{
    return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

/**
 * 試験用 event の通知を有限時間だけ待つ。
 * @param event_handle 待機する event。
 * @return 5秒以内に通知された場合は true。
 */
bool WaitForLogSinkTestEvent(HANDLE event_handle) noexcept
{
    return event_handle != nullptr && ::WaitForSingleObject(event_handle, 5000u) == WAIT_OBJECT_0;
}

/**
 * callback 中だけ有効な本文を保持用配列へ複製する。
 * @param output null終端するコピー先。
 * @param output_capacity output の byte 数。
 * @param message callback が受け取った本文。
 * @return output_capacity 内で null終端を確認できた場合は true。
 */
bool CopyLogSinkMessage(char* output, usize output_capacity, const char* message) noexcept
{
    if (output == nullptr || output_capacity == 0u || message == nullptr) return false;
    /** 終端を探しながらコピーする本文位置。 */
    usize character_index = 0u;
    while (character_index + 1u < output_capacity && message[character_index] != '\0') {
        output[character_index] = message[character_index];
        ++character_index;
    }
    output[character_index] = '\0';
    return message[character_index] == '\0';
}

/** legacy SetSink の呼び出しを1番として記録する。 */
void LegacyOrderSink(ELogSeverity, const char*) noexcept
{
    if (g_legacy_order != nullptr) AppendOrder(*g_legacy_order, 1u);
}

/** allocation failure 後の legacy sink 実送信を記録する。 */
void LegacyDeliverySink(ELogSeverity severity, const char* message) noexcept
{
    if (g_legacy_delivery == nullptr) return;
    g_legacy_delivery->severity = severity;
    CopyLogSinkMessage(g_legacy_delivery->message, sizeof(g_legacy_delivery->message), message);
    ++g_legacy_delivery->count;
}

/** 複数購読の識別番号を user から読み、呼び出し順へ記録する。 */
void OrderedSink(ELogSeverity, const char*, void* user) noexcept
{
    auto* const values = static_cast<u32*>(user);
    if (g_legacy_order != nullptr && values != nullptr) AppendOrder(*g_legacy_order, *values);
}

/** 容量試験で通知を行わない購読先。 */
void EmptySink(ELogSeverity, const char*, void*) noexcept
{
}

/** 変更処理から追加された購読の呼び出しを記録する。 */
void AddedSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkMutationContext*>(user);
    AppendOrder(context.order, 4u);
}

/** 変更 callback より先に呼ばれ、同じ巡回中の後続解除と分離される。 */
void EarlierSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkMutationContext*>(user);
    AppendOrder(context.order, 1u);
}

/** callback 中に先行・自身・後続を解除し、次レコード用購読を追加する。 */
void MutatingSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkMutationContext*>(user);
    AppendOrder(context.order, 2u);
    CLogger::UnsubscribeSink(context.earlier);
    CLogger::UnsubscribeSink(context.self);
    CLogger::UnsubscribeSink(context.later);
    context.added = CLogger::SubscribeSink(AddedSink, &context);
}

/** 解除対象になる後続購読の呼び出しを記録する。 */
void LaterSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkMutationContext*>(user);
    AppendOrder(context.order, 3u);
}

/** 外部解除が待機する間 callback を明示許可まで止める。 */
void BlockingSubscribedSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkWaitContext*>(user);
    ::SetEvent(context.entered_event);
    ::WaitForSingleObject(context.release_event, INFINITE);
}

/** 別スレッドから購読を解除し、復帰を記録する。 */
void RemoveSubscribedSinkWorker(void* user) noexcept
{
    auto& context = *static_cast<FLogSinkWaitContext*>(user);
    if (CLogger::UnsubscribeSink(context.handle)) context.removed.Store(1u);
}

/** callback 内からの Init、Shutdown、所有権破棄が安全に戻ることを記録する。 */
void CallbackLifecycleSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkCallbackLifecycleContext*>(user);
    CLogger::Init(context.config);
    CLogger::Shutdown();
    if (context.owned != nullptr) context.owned->~FLogSinkSubscription();
    context.completed.Store(1u);
}

/** 別スレッド Shutdown が待つ間 callback を明示許可まで止める。 */
void BlockingShutdownSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkShutdownContext*>(user);
    ::SetEvent(context.entered_event);
    ::WaitForSingleObject(context.release_event, INFINITE);
}

/** 別スレッドから Logger を終了し、復帰を記録する。 */
void ShutdownLoggerWorker(void* user) noexcept
{
    auto& context = *static_cast<FLogSinkShutdownContext*>(user);
    CLogger::Shutdown();
    context.completed.Store(1u);
}

/**
 * nested callback の通知内容を順序付き配列へ保持コピーする。
 * @param context 記録先。
 * @param sink sink の識別番号。
 * @param severity レコードに保存された重大度。
 * @param message callback 中だけ有効な本文。
 */
void AppendNestedSinkEntry(FNestedSinkContext& context, u32 sink, ELogSeverity severity, const char* message) noexcept
{
    if (context.count >= 8u) return;
    FNestedSinkEntry& entry = context.entries[context.count++];
    entry.sink = sink;
    entry.severity = severity;
    CopyLogSinkMessage(entry.message, sizeof(entry.message), message);
}

/** outer callback 中に追加され、次レコードから通知される sink。 */
void NestedAddedSink(ELogSeverity severity, const char* message, void* user) noexcept;

/** outer callback 中に購読を追加して nested レコードを1件積む。 */
void NestedOuterSink(ELogSeverity severity, const char* message, void* user) noexcept
{
    auto& context = *static_cast<FNestedSinkContext*>(user);
    AppendNestedSinkEntry(context, 1u, severity, message);
    if (!context.nested_written) {
        context.nested_written = true;
        context.added = CLogger::SubscribeSink(&NestedAddedSink, &context);
        CLogger::Write(ELogSeverity::Error, FSourceLoc::Current(), "nested record");
    }
}

/** outer callback 中に追加され、次レコードから通知される sink。 */
void NestedAddedSink(ELogSeverity severity, const char* message, void* user) noexcept
{
    auto& context = *static_cast<FNestedSinkContext*>(user);
    AppendNestedSinkEntry(context, 2u, severity, message);
}

/** 呼び出し側本文の変更を待ってから callback 引数を保持コピーする。 */
void MessageLifetimeSink(ELogSeverity severity, const char* message, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkMessageContext*>(user);
    ::SetEvent(context.entered_event);
    ::WaitForSingleObject(context.release_event, INFINITE);
    context.severity = severity;
    context.null_terminated = CopyLogSinkMessage(context.copied_message, sizeof(context.copied_message), message);
}

/** registry 共有 lock 内で同期される TryCopySinkHandles を実行する。 */
void CopySinkHandlesWorker(void* user) noexcept
{
    auto& context = *static_cast<FCopySinkHandlesContext*>(user);
    context.copied = CLogger::TryCopySinkHandles(context.output, 4u, context.output_count);
}

/** 購読一覧コピーと同時に既存購読を解除し、新しい購読を追加する。 */
void CopyMutationWorker(void* user) noexcept
{
    auto& context = *static_cast<FCopyMutationContext*>(user);
    ::SetEvent(context.started_event);
    context.remove_succeeded = CLogger::UnsubscribeSink(context.removed);
    context.added = CLogger::SubscribeSink(&EmptySink, nullptr);
    context.completed.Store(1u);
}

/** stress worker 共通の開始 barrier へ到達し、全 worker の開始許可を待つ。 */
void WaitForLogSinkStressStart(FLogSinkStressContext& context) noexcept
{
    context.ready_count.FetchAdd(1u);
    while (context.start.Load() == 0u)
        Yield();
}

/** 有効 lifecycle で実行された stress callback を数える。 */
void StressSink(ELogSeverity, const char*, void* user) noexcept
{
    auto& context = *static_cast<FLogSinkStressContext*>(user);
    context.callback_count.FetchAdd(1u);
}

/** 有限回の Subscribe と外部 Unsubscribe を繰り返す。 */
void SubscribeStressWorker(void* user) noexcept
{
    auto& context = *static_cast<FLogSinkStressContext*>(user);
    WaitForLogSinkStressStart(context);
    /** lifecycle 競合を有限時間で反復する回数。 */
    constexpr u32 kIterationCount = 256u;
    for (u32 iteration = 0u; iteration < kIterationCount; ++iteration) {
        /** この反復で所有する購読。 */
        const FLogSinkHandle handle = CLogger::SubscribeSink(&StressSink, &context);
        if (handle.IsValid()) CLogger::UnsubscribeSink(handle);
    }
}

/** lifecycle 切替と同時に有限件のログレコードを投入する。 */
void LogStressWorker(void* user) noexcept
{
    auto& context = *static_cast<FLogSinkStressContext*>(user);
    WaitForLogSinkStressStart(context);
    /** lifecycle 競合を有限時間で反復するレコード数。 */
    constexpr u32 kIterationCount = 1024u;
    for (u32 iteration = 0u; iteration < kIterationCount; ++iteration) {
        CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "log sink lifecycle stress");
    }
}

/** 有限回の Shutdown と Init を同時処理へ重ねる。 */
void LifecycleStressWorker(void* user) noexcept
{
    auto& context = *static_cast<FLogSinkStressContext*>(user);
    WaitForLogSinkStressStart(context);
    /** 資源解放と再確保を有限時間で反復する回数。 */
    constexpr u32 kIterationCount = 24u;
    for (u32 iteration = 0u; iteration < kIterationCount; ++iteration) {
        CLogger::Shutdown();
        CLogger::Init(context.config);
    }
}

} // namespace

static_assert(sizeof(FLogSinkHandle) == 8u);
static_assert(alignof(FLogSinkHandle) == 4u);
static_assert(offsetof(FLogSinkHandle, slot) == 0u);
static_assert(offsetof(FLogSinkHandle, generation) == 4u);
static_assert(std::is_standard_layout_v<FLogSinkHandle>);
static_assert(std::is_trivially_copyable_v<FLogSinkHandle>);
static_assert(!std::is_copy_constructible_v<FLogSinkSubscription>);
static_assert(std::is_move_constructible_v<FLogSinkSubscription>);
static_assert(TGenerationHandleLayoutTraits<FLogSinkHandle>::kStorageBytes == 8u);
static_assert(sizeof(FLogConfig) == 24u);
static_assert(alignof(FLogConfig) == 8u);
static_assert(offsetof(FLogConfig, file_path) == 0u);
static_assert(offsetof(FLogConfig, min_severity) == 8u);
static_assert(offsetof(FLogConfig, ring_capacity) == 12u);
static_assert(offsetof(FLogConfig, console) == 16u);
static_assert(offsetof(FLogConfig, debug_output) == 17u);

ACS_TEST(LogSinkRegistry, LegacyRunsBeforeRegisteredOrder)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkOrderContext order{};
    u32 second = 2u;
    u32 third = 3u;
    g_legacy_order = &order;
    CLogger::SetSink(&LegacyOrderSink);
    const FLogSinkHandle first = CLogger::SubscribeSink(&OrderedSink, &second);
    const FLogSinkHandle second_handle = CLogger::SubscribeSink(&OrderedSink, &third);

    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "ordered sinks");
    CLogger::Flush();

    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(second_handle.IsValid());
    EXPECT_EQ(order.count, 3u);
    EXPECT_EQ(order.values[0], 1u);
    EXPECT_EQ(order.values[1], 2u);
    EXPECT_EQ(order.values[2], 3u);
    g_legacy_order = nullptr;
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, CallbackMutationUsesStableRecordBoundary)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkMutationContext context{};
    context.earlier = CLogger::SubscribeSink(&EarlierSink, &context);
    context.self = CLogger::SubscribeSink(&MutatingSink, &context);
    context.later = CLogger::SubscribeSink(&LaterSink, &context);
    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "first mutation record");
    CLogger::Flush();

    EXPECT_EQ(context.order.count, 2u);
    EXPECT_EQ(context.order.values[0], 1u);
    EXPECT_EQ(context.order.values[1], 2u);
    EXPECT_FALSE(CLogger::IsSinkSubscribed(context.earlier));
    EXPECT_FALSE(CLogger::IsSinkSubscribed(context.self));
    EXPECT_FALSE(CLogger::IsSinkSubscribed(context.later));
    EXPECT_TRUE(CLogger::IsSinkSubscribed(context.added));
    EXPECT_FALSE(context.added.slot == context.earlier.slot);
    EXPECT_FALSE(context.added.slot == context.self.slot);
    EXPECT_FALSE(context.added.slot == context.later.slot);

    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "second mutation record");
    CLogger::Flush();
    EXPECT_EQ(context.order.count, 3u);
    EXPECT_EQ(context.order.values[2], 4u);

    context.recycled = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_TRUE(context.recycled.IsValid());
    EXPECT_EQ(context.recycled.slot, context.later.slot);
    EXPECT_EQ(context.recycled.generation, context.later.generation + 1u);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, NestedWriteAndCallbackAdditionStartAtNextRecord)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FNestedSinkContext context{};
    EXPECT_TRUE(CLogger::SubscribeSink(&NestedOuterSink, &context).IsValid());
    CLogger::Write(ELogSeverity::Warn, FSourceLoc::Current(), "outer record");
    CLogger::Flush();
    CLogger::Flush();

    EXPECT_TRUE(context.added.IsValid());
    EXPECT_EQ(context.count, 3u);
    EXPECT_EQ(context.entries[0].sink, 1u);
    EXPECT_EQ(context.entries[0].severity, ELogSeverity::Warn);
    EXPECT_TRUE(::strcmp(context.entries[0].message, "outer record") == 0);
    EXPECT_EQ(context.entries[1].sink, 1u);
    EXPECT_EQ(context.entries[1].severity, ELogSeverity::Error);
    EXPECT_TRUE(::strcmp(context.entries[1].message, "nested record") == 0);
    EXPECT_EQ(context.entries[2].sink, 2u);
    EXPECT_EQ(context.entries[2].severity, ELogSeverity::Error);
    EXPECT_TRUE(::strcmp(context.entries[2].message, "nested record") == 0);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, RecordOwnsMessageAndSeverityAndCallbackCopiesNullTerminatedText)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkMessageContext context{};
    context.entered_event = CreateLogSinkTestEvent();
    context.release_event = CreateLogSinkTestEvent();
    EXPECT_TRUE(context.entered_event != nullptr);
    EXPECT_TRUE(context.release_event != nullptr);
    EXPECT_TRUE(CLogger::SubscribeSink(&MessageLifetimeSink, &context).IsValid());

    ELogSeverity caller_severity = ELogSeverity::Warn;
    {
        char caller_message[32] = "caller owned message";
        CLogger::Write(caller_severity, FSourceLoc::Current(), caller_message);
        EXPECT_TRUE(WaitForLogSinkTestEvent(context.entered_event));
        caller_severity = ELogSeverity::Fatal;
        ::memset(caller_message, 'x', sizeof(caller_message) - 1u);
        caller_message[sizeof(caller_message) - 1u] = '\0';
    }
    ::SetEvent(context.release_event);
    CLogger::Flush();

    EXPECT_EQ(context.severity, ELogSeverity::Warn);
    EXPECT_TRUE(context.null_terminated);
    EXPECT_TRUE(::strcmp(context.copied_message, "caller owned message") == 0);
    if (context.release_event != nullptr) ::CloseHandle(context.release_event);
    if (context.entered_event != nullptr) ::CloseHandle(context.entered_event);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, CopyValidatesNullAlignmentOverflowAndAliasesBeforeWriting)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkHandle empty_output{23u, 29u};
    u32 empty_count = 77u;
    EXPECT_FALSE(CLogger::TryCopySinkHandles(nullptr, 0u, empty_count));
    EXPECT_EQ(empty_count, 77u);
    EXPECT_TRUE(CLogger::TryCopySinkHandles(&empty_output, 1u, empty_count));
    EXPECT_EQ(empty_count, 0u);
    EXPECT_EQ(empty_output.slot, 23u);
    EXPECT_EQ(empty_output.generation, 29u);

    const FLogSinkHandle active = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_TRUE(active.IsValid());
    u32 null_count = 81u;
    EXPECT_FALSE(CLogger::TryCopySinkHandles(nullptr, 1u, null_count));
    EXPECT_EQ(null_count, 81u);

    alignas(FLogSinkHandle) u8 misaligned_bytes[sizeof(FLogSinkHandle) + 1u]{};
    u8 misaligned_before[sizeof(misaligned_bytes)]{};
    ::memset(misaligned_bytes, 0xA5, sizeof(misaligned_bytes));
    ::memcpy(misaligned_before, misaligned_bytes, sizeof(misaligned_bytes));
    u32 misaligned_count = 83u;
    /** 整列済み領域から1 byteずらした無効なコピー先。 */
    auto* const misaligned_output = reinterpret_cast<FLogSinkHandle*>(misaligned_bytes + 1u);
    EXPECT_FALSE(CLogger::TryCopySinkHandles(misaligned_output, 1u, misaligned_count));
    EXPECT_EQ(misaligned_count, 83u);
    EXPECT_TRUE(::memcmp(misaligned_bytes, misaligned_before, sizeof(misaligned_bytes)) == 0);

    /** UINT32_MAX 要素の宣言範囲内へ output_count を配置する検証領域。 */
    struct FHugeCapacityOutput {
        /** 変更されてはならない先頭2要素。 */
        FLogSinkHandle output[2]{{31u, 37u}, {41u, 43u}};

        /** 巨大な宣言範囲と重なる件数出力先。 */
        u32 count = 89u;
    };
    FHugeCapacityOutput huge{};
    const FHugeCapacityOutput huge_before = huge;
    EXPECT_FALSE(CLogger::TryCopySinkHandles(huge.output, 0xFFFFFFFFu, huge.count));
    EXPECT_TRUE(::memcmp(&huge, &huge_before, sizeof(huge)) == 0);

    /** 1要素の終端加算がポインタ幅を越える整列済みの疑似アドレス。 */
    const uptr near_end_address = (~uptr{0}) - ((~uptr{0}) % alignof(FLogSinkHandle));
    /** 実際には参照せず、事前の終端桁あふれ検証だけへ渡す疑似コピー先。 */
    auto* const near_end_output = reinterpret_cast<FLogSinkHandle*>(near_end_address);
    u32 near_end_count = 97u;
    EXPECT_FALSE(CLogger::TryCopySinkHandles(near_end_output, 1u, near_end_count));
    EXPECT_EQ(near_end_count, 97u);

    FLogSinkHandle slot_alias[2]{{47u, 53u}, {59u, 61u}};
    FLogSinkHandle slot_alias_before[2]{};
    ::memcpy(slot_alias_before, slot_alias, sizeof(slot_alias));
    EXPECT_FALSE(CLogger::TryCopySinkHandles(slot_alias, 2u, slot_alias[0].slot));
    EXPECT_TRUE(::memcmp(slot_alias, slot_alias_before, sizeof(slot_alias)) == 0);

    FLogSinkHandle generation_alias[2]{{67u, 71u}, {73u, 79u}};
    FLogSinkHandle generation_alias_before[2]{};
    ::memcpy(generation_alias_before, generation_alias, sizeof(generation_alias));
    EXPECT_FALSE(CLogger::TryCopySinkHandles(generation_alias, 2u, generation_alias[0].generation));
    EXPECT_TRUE(::memcmp(generation_alias, generation_alias_before, sizeof(generation_alias)) == 0);

    alignas(FLogSinkHandle) u8 partial_alias[sizeof(FLogSinkHandle) + sizeof(u32)]{};
    u8 partial_alias_before[sizeof(partial_alias)]{};
    ::memset(partial_alias, 0x6B, sizeof(partial_alias));
    ::memcpy(partial_alias_before, partial_alias, sizeof(partial_alias));
    /** ハンドル範囲の末尾1 byteだけに重なる疑似件数出力先。 */
    u32& partial_count = *reinterpret_cast<u32*>(partial_alias + sizeof(FLogSinkHandle) - 1u);
    EXPECT_FALSE(CLogger::TryCopySinkHandles(reinterpret_cast<FLogSinkHandle*>(partial_alias), 1u, partial_count));
    EXPECT_TRUE(::memcmp(partial_alias, partial_alias_before, sizeof(partial_alias)) == 0);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, FixedCapacityAndCopyAreAllOrNoneInRegistrationOrder)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    constexpr u32 kCapacity = 4096u;
    FLogSinkHandle registered[kCapacity]{};
    /** 固定表を登録順に埋める要素番号。 */
    for (u32 index = 0; index < kCapacity; ++index)
        registered[index] = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_EQ(CLogger::SinkCount(), kCapacity);
    EXPECT_FALSE(CLogger::SubscribeSink(&EmptySink, nullptr).IsValid());

    FLogSinkHandle unchanged[2]{{7u, 9u}, {8u, 10u}};
    FLogSinkHandle unchanged_before[2]{};
    ::memcpy(unchanged_before, unchanged, sizeof(unchanged));
    u32 unchanged_count = 77u;
    EXPECT_FALSE(CLogger::TryCopySinkHandles(unchanged, 2u, unchanged_count));
    EXPECT_TRUE(::memcmp(unchanged, unchanged_before, sizeof(unchanged)) == 0);
    EXPECT_EQ(unchanged_count, 77u);

    FLogSinkHandle copied[kCapacity]{};
    u32 copied_count = 0u;
    EXPECT_TRUE(CLogger::TryCopySinkHandles(copied, kCapacity, copied_count));
    EXPECT_EQ(copied_count, kCapacity);
    /** コピー結果と元の登録順を比較する要素番号。 */
    for (u32 index = 0; index < kCapacity; ++index) EXPECT_TRUE(copied[index] == registered[index]);
    RestoreLogSinkTestLogger();
}

#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
ACS_TEST(LogSinkRegistry, CopySnapshotExcludesConcurrentMutationUntilSharedLockRelease)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    const FLogSinkHandle first = CLogger::SubscribeSink(&EmptySink, nullptr);
    const FLogSinkHandle second = CLogger::SubscribeSink(&EmptySink, nullptr);
    HANDLE copy_entered_event = CreateLogSinkTestEvent();
    HANDLE copy_release_event = CreateLogSinkTestEvent();
    HANDLE mutation_started_event = CreateLogSinkTestEvent();
    EXPECT_TRUE(copy_entered_event != nullptr);
    EXPECT_TRUE(copy_release_event != nullptr);
    EXPECT_TRUE(mutation_started_event != nullptr);

    FCopySinkHandlesContext copy{};
    FCopyMutationContext mutation{};
    mutation.removed = first;
    mutation.started_event = mutation_started_event;
    log_sink_test_detail::SetCopySinkHandlesBarrierEvents(copy_entered_event, copy_release_event);
    auto copy_worker = FThread::Spawn(&CopySinkHandlesWorker, &copy);
    EXPECT_TRUE(copy_worker.IsOk());
    if (copy_worker.IsErr() || !WaitForLogSinkTestEvent(copy_entered_event)) {
        ::SetEvent(copy_release_event);
        log_sink_test_detail::SetCopySinkHandlesBarrierEvents(nullptr, nullptr);
        if (copy_worker.IsOk()) copy_worker.Value().Join();
        if (mutation_started_event != nullptr) ::CloseHandle(mutation_started_event);
        if (copy_release_event != nullptr) ::CloseHandle(copy_release_event);
        if (copy_entered_event != nullptr) ::CloseHandle(copy_entered_event);
        RestoreLogSinkTestLogger();
        return;
    }

    auto mutation_worker = FThread::Spawn(&CopyMutationWorker, &mutation);
    EXPECT_TRUE(mutation_worker.IsOk());
    EXPECT_TRUE(WaitForLogSinkTestEvent(mutation_started_event));
    EXPECT_EQ(mutation.completed.Load(), 0u);
    ::SetEvent(copy_release_event);
    copy_worker.Value().Join();
    if (mutation_worker.IsOk()) mutation_worker.Value().Join();
    log_sink_test_detail::SetCopySinkHandlesBarrierEvents(nullptr, nullptr);

    EXPECT_TRUE(copy.copied);
    EXPECT_EQ(copy.output_count, 2u);
    EXPECT_TRUE(copy.output[0] == first);
    EXPECT_TRUE(copy.output[1] == second);
    EXPECT_TRUE(mutation.remove_succeeded);
    EXPECT_TRUE(mutation.added.IsValid());
    EXPECT_EQ(mutation.completed.Load(), 1u);
    EXPECT_FALSE(CLogger::IsSinkSubscribed(first));
    EXPECT_TRUE(CLogger::IsSinkSubscribed(second));
    EXPECT_TRUE(CLogger::IsSinkSubscribed(mutation.added));

    if (mutation_started_event != nullptr) ::CloseHandle(mutation_started_event);
    if (copy_release_event != nullptr) ::CloseHandle(copy_release_event);
    if (copy_entered_event != nullptr) ::CloseHandle(copy_entered_event);
    RestoreLogSinkTestLogger();
}
#endif

ACS_TEST(LogSinkRegistry, ExternalRemovalWaitsForInFlightCallback)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkWaitContext context{};
    context.entered_event = CreateLogSinkTestEvent();
    context.release_event = CreateLogSinkTestEvent();
    HANDLE wait_entered_event = CreateLogSinkTestEvent();
    EXPECT_TRUE(context.entered_event != nullptr);
    EXPECT_TRUE(context.release_event != nullptr);
    EXPECT_TRUE(wait_entered_event != nullptr);
    context.handle = CLogger::SubscribeSink(&BlockingSubscribedSink, &context);
    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "blocking subscribed sink");
    EXPECT_TRUE(WaitForLogSinkTestEvent(context.entered_event));

#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
    log_sink_test_detail::SetUnsubscribeWaitEvent(wait_entered_event);
#endif
    auto worker = FThread::Spawn(&RemoveSubscribedSinkWorker, &context);
    EXPECT_TRUE(worker.IsOk());
    if (worker.IsErr()) {
        ::SetEvent(context.release_event);
#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
        log_sink_test_detail::SetUnsubscribeWaitEvent(nullptr);
#endif
        if (wait_entered_event != nullptr) ::CloseHandle(wait_entered_event);
        if (context.release_event != nullptr) ::CloseHandle(context.release_event);
        if (context.entered_event != nullptr) ::CloseHandle(context.entered_event);
        RestoreLogSinkTestLogger();
        return;
    }

    EXPECT_TRUE(WaitForLogSinkTestEvent(wait_entered_event));
    EXPECT_EQ(context.removed.Load(), 0u);
    ::SetEvent(context.release_event);
    worker.Value().Join();
#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
    log_sink_test_detail::SetUnsubscribeWaitEvent(nullptr);
#endif
    EXPECT_EQ(context.removed.Load(), 1u);
    EXPECT_FALSE(CLogger::IsSinkSubscribed(context.handle));
    if (wait_entered_event != nullptr) ::CloseHandle(wait_entered_event);
    if (context.release_event != nullptr) ::CloseHandle(context.release_event);
    if (context.entered_event != nullptr) ::CloseHandle(context.entered_event);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, OwnedSubscriptionMoveAssignmentAndDestructorsLeaveCountZero)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkHandle moved_handle{};
    FLogSinkHandle replaced_handle{};
    {
        FLogSinkSubscription first = CLogger::SubscribeSinkOwned(&EmptySink, nullptr);
        FLogSinkSubscription second = CLogger::SubscribeSinkOwned(&EmptySink, nullptr);
        moved_handle = first.Handle();
        replaced_handle = second.Handle();
        EXPECT_EQ(CLogger::SinkCount(), 2u);

        second = Move(first);
        EXPECT_FALSE(first.IsValid());
        EXPECT_TRUE(second.IsValid());
        EXPECT_TRUE(second.Handle() == moved_handle);
        EXPECT_FALSE(CLogger::IsSinkSubscribed(replaced_handle));
        EXPECT_EQ(CLogger::SinkCount(), 1u);
    }
    EXPECT_FALSE(CLogger::IsSinkSubscribed(moved_handle));
    EXPECT_EQ(CLogger::SinkCount(), 0u);

    {
        FLogSinkSubscription scoped = CLogger::SubscribeSinkOwned(&EmptySink, nullptr);
        EXPECT_TRUE(scoped.IsValid());
        EXPECT_EQ(CLogger::SinkCount(), 1u);
    }
    EXPECT_EQ(CLogger::SinkCount(), 0u);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, CallbackLifecycleCallsAndOwnedDestructionAreSafe)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkCallbackLifecycleContext context{};
    context.config = config;
    FLogSinkSubscription owned;
    context.owned = &owned;
    owned = CLogger::SubscribeSinkOwned(&CallbackLifecycleSink, &context);
    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "callback lifecycle");
    CLogger::Flush();

    EXPECT_EQ(context.completed.Load(), 1u);
    EXPECT_TRUE(CLogger::IsInitialized());
    EXPECT_EQ(CLogger::SinkCount(), 0u);
    new (&owned) FLogSinkSubscription{};
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, ExternalShutdownWaitsForDispatchCompletion)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    FLogSinkShutdownContext context{};
    context.entered_event = CreateLogSinkTestEvent();
    context.release_event = CreateLogSinkTestEvent();
    HANDLE join_entered_event = CreateLogSinkTestEvent();
    EXPECT_TRUE(context.entered_event != nullptr);
    EXPECT_TRUE(context.release_event != nullptr);
    EXPECT_TRUE(join_entered_event != nullptr);
    EXPECT_TRUE(CLogger::SubscribeSink(&BlockingShutdownSink, &context).IsValid());
    CLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "shutdown waits dispatch");
    EXPECT_TRUE(WaitForLogSinkTestEvent(context.entered_event));

#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
    log_sink_test_detail::SetShutdownWaitEvent(join_entered_event);
#endif
    auto worker = FThread::Spawn(&ShutdownLoggerWorker, &context);
    EXPECT_TRUE(worker.IsOk());
    if (worker.IsErr()) {
        ::SetEvent(context.release_event);
#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
        log_sink_test_detail::SetShutdownWaitEvent(nullptr);
#endif
        if (join_entered_event != nullptr) ::CloseHandle(join_entered_event);
        if (context.release_event != nullptr) ::CloseHandle(context.release_event);
        if (context.entered_event != nullptr) ::CloseHandle(context.entered_event);
        RestoreLogSinkTestLogger();
        return;
    }
    EXPECT_TRUE(WaitForLogSinkTestEvent(join_entered_event));
    EXPECT_EQ(context.completed.Load(), 0u);
    ::SetEvent(context.release_event);
    worker.Value().Join();
#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
    log_sink_test_detail::SetShutdownWaitEvent(nullptr);
#endif
    EXPECT_EQ(context.completed.Load(), 1u);
    EXPECT_FALSE(CLogger::IsInitialized());
    if (join_entered_event != nullptr) ::CloseHandle(join_entered_event);
    if (context.release_event != nullptr) ::CloseHandle(context.release_event);
    if (context.entered_event != nullptr) ::CloseHandle(context.entered_event);
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, ShutdownRejectsStaleHandlesAfterReinitialize)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);
    const FLogSinkHandle stale = CLogger::SubscribeSink(&EmptySink, nullptr);
    CLogger::Shutdown();
    EXPECT_FALSE(CLogger::IsSinkSubscribed(stale));
    EXPECT_FALSE(CLogger::UnsubscribeSink(stale));

    CLogger::Init(config);
    const FLogSinkHandle current = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_TRUE(current.IsValid());
    EXPECT_FALSE(current == stale);
    EXPECT_FALSE(CLogger::UnsubscribeSink(stale));
    EXPECT_TRUE(CLogger::IsSinkSubscribed(current));
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, FiniteBarrierStressKeepsLifecycleAndStaleHandlesSafe)
{
    CLogger::Shutdown();
    FLogSinkStressContext context{};
    context.config = QuietLogConfig();
    CLogger::Init(context.config);
    const FLogSinkHandle stale = CLogger::SubscribeSink(&StressSink, &context);
    EXPECT_TRUE(stale.IsValid());

    auto subscribe_worker = FThread::Spawn(&SubscribeStressWorker, &context);
    auto log_worker = FThread::Spawn(&LogStressWorker, &context);
    auto lifecycle_worker = FThread::Spawn(&LifecycleStressWorker, &context);
    EXPECT_TRUE(subscribe_worker.IsOk());
    EXPECT_TRUE(log_worker.IsOk());
    EXPECT_TRUE(lifecycle_worker.IsOk());
    if (subscribe_worker.IsErr() || log_worker.IsErr() || lifecycle_worker.IsErr()) {
        context.start.Store(1u);
        if (subscribe_worker.IsOk()) subscribe_worker.Value().Join();
        if (log_worker.IsOk()) log_worker.Value().Join();
        if (lifecycle_worker.IsOk()) lifecycle_worker.Value().Join();
        if (!CLogger::IsInitialized()) CLogger::Init(context.config);
        RestoreLogSinkTestLogger();
        return;
    }

    while (context.ready_count.Load() != 3u)
        Yield();
    context.start.Store(1u);
    subscribe_worker.Value().Join();
    log_worker.Value().Join();
    lifecycle_worker.Value().Join();

    if (!CLogger::IsInitialized()) CLogger::Init(context.config);
    CLogger::Flush();
    EXPECT_FALSE(CLogger::IsSinkSubscribed(stale));
    EXPECT_FALSE(CLogger::UnsubscribeSink(stale));
    EXPECT_EQ(CLogger::SinkCount(), 0u);
    const FLogSinkHandle current = CLogger::SubscribeSink(&StressSink, &context);
    EXPECT_TRUE(current.IsValid());
    EXPECT_FALSE(current == stale);
    RestoreLogSinkTestLogger();
}

#if defined(ACS_FOUNDATION_LOG_TEST_HOOKS)
ACS_TEST(LogSinkRegistry, AllocationFailureKeepsLegacyLoggerAndCanRetry)
{
    CLogger::Shutdown();
    const FLogConfig config = QuietLogConfig();
    log_sink_test_detail::FailNextSinkRegistryAllocation();
    CLogger::Init(config);
    EXPECT_TRUE(CLogger::IsInitialized());
    EXPECT_EQ(CLogger::SinkCount(), 0u);
    EXPECT_FALSE(CLogger::SubscribeSink(&EmptySink, nullptr).IsValid());

    FLegacyDeliveryContext delivery{};
    g_legacy_delivery = &delivery;
    CLogger::SetSink(&LegacyDeliverySink);
    CLogger::Write(ELogSeverity::Fatal, FSourceLoc::Current(), "legacy survives registry allocation failure");
    CLogger::Flush();
    EXPECT_EQ(delivery.count, 1u);
    EXPECT_EQ(delivery.severity, ELogSeverity::Fatal);
    EXPECT_TRUE(::strcmp(delivery.message, "legacy survives registry allocation failure") == 0);
    CLogger::SetSink(nullptr);
    g_legacy_delivery = nullptr;

    CLogger::Shutdown();
    CLogger::Init(config);
    EXPECT_TRUE(CLogger::IsInitialized());
    EXPECT_TRUE(CLogger::SubscribeSink(&EmptySink, nullptr).IsValid());
    RestoreLogSinkTestLogger();
}

ACS_TEST(LogSinkRegistry, ActiveMaximumGenerationRetiresAcrossShutdownAndReinitialize)
{
    CLogger::Shutdown();
    EXPECT_TRUE(log_sink_test_detail::SetSinkGeneration(0u, 0xFFFFFFFEu));
    const FLogConfig config = QuietLogConfig();
    CLogger::Init(config);

    const FLogSinkHandle maximum = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_EQ(maximum.slot, 0u);
    EXPECT_EQ(maximum.generation, 0xFFFFFFFFu);
    CLogger::Shutdown();
    EXPECT_FALSE(CLogger::IsSinkSubscribed(maximum));
    EXPECT_FALSE(CLogger::UnsubscribeSink(maximum));

    CLogger::Init(config);
    const FLogSinkHandle next = CLogger::SubscribeSink(&EmptySink, nullptr);
    EXPECT_TRUE(next.IsValid());
    EXPECT_EQ(next.slot, 1u);
    EXPECT_FALSE(CLogger::UnsubscribeSink(maximum));

    CLogger::Shutdown();
    EXPECT_TRUE(log_sink_test_detail::SetSinkGeneration(0u, 0u));
    RestoreLogSinkTestLogger();
}
#endif
