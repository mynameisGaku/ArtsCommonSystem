// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — 上限付き Windows hot reload watcher。
//
// 開発ビルドでは ReadDirectoryChangesW を非同期 poll する。監視 path は UTF-8 の
// 所有コピーとして保持し、登録と event pair は transactional に commit する。
// 同一 path の burst は debounce でき、callback は安定 snapshot から dispatch する。
// 出荷ビルドは API を no-op shell として残し、呼び出し側の条件コンパイルを不要にする。
//
// 本型は単一 thread 専用。callback からの TryTick 再入は拒否するが、
// UnregisterCallback、ClearEvents、Shutdown は callback 中にも利用できる。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#ifndef ACS_GAME_SHIPPING
#include "container/String.h"     // FString — OS から払い出された path を UTF-8 で所有
#include "memory/UniquePtr.h"     // TUniquePtr<FWatchEntry> — OVERLAPPED のアドレスを安定化
#endif

namespace acs::game {

// 1 件の hot reload イベント (FS watcher → callback への引数)。
//
// trivially copyable な POD。file_path は watcher が所有する。callback 中はその
// 呼び出しの間だけ有効。ConsumeNextEvent で取得した場合は、次の
// ConsumeNextEvent、ClearEvents、Shutdown のいずれかまで有効。
struct FHotReloadEvent {
    // null 終端の変更パス (caller / watcher 所有、呼び出しスコープ中のみ valid)。
    const char* file_path         = nullptr;

    // 変更元の単調増加タイムスタンプ。単位はミリ秒。
    //
    // native event は GetTickCount64 を使う。独自変更元で debounce を有効にする
    // 場合も、同じ単位の単調増加 clock を使う。
    u64         modified_timestamp = 0;

    // true: ファイル削除イベント、false: 更新イベント。
    bool        removed            = false;
};

// hot reload の checked API が返す安定した結果分類。
enum class EHotReloadResult : u8 {
    Success,
    AlreadyRegistered,
    NotRegistered,
    InvalidArgument,
    InvalidUtf8,
    PathTooLong,
    LimitExceeded,
    OutOfMemory,
    OsError,
    ReentrantCall,
    NativeOverflow,
    // 実際の結果ではなく、列挙網羅性を検査する末尾 sentinel。
    Count,
};

// 結果値に対応する安定した診断名を返す。未知値は "Unknown"。
const char* HotReloadResultName(EHotReloadResult result) noexcept;

// FHotReloadWatcher の通知パイプライン診断スナップショット。
//
// 全カウンタは最大値で飽和し、折り返さない。last_failure と
// authoritative_rescan_required は sticky であり、成功した後続操作では解除されない。
// 確認済み状態へ戻すには ClearDiagnostics を明示的に呼ぶ。
struct FHotReloadDiagnostics {
    // 新しい pending event pair として queue へ commit できた累積件数。
    u64 enqueued_event_count = 0u;

    // debounce により既存 pending event へ統合した累積件数。
    u64 coalesced_event_count = 0u;

    // callback dispatch の対象として FIFO から取り出した累積 event 件数。
    u64 dispatched_event_count = 0u;

    // 検証または queue 追加に失敗して拒否した累積 event 件数。
    u64 rejected_event_count = 0u;

    // 通知集合の完全性を保証できなくなった累積 incident 件数。
    //
    // native overflow の実欠落 event 数は不明なため、event 数ではなく incident 数を
    // 数える。上限到達や OOM で有効 event を queue へ追加できなかった場合も 1 件。
    u64 loss_incident_count = 0u;

    // enqueue、native poll、dispatch の直近の非 Success 結果。
    //
    // Success はこの値を上書きしない。初期値と明示 clear 後だけ Success になる。
    EHotReloadResult last_failure = EHotReloadResult::Success;

    // true の間は、ディスクや asset DB などの正本から全量再走査する必要がある。
    bool authoritative_rescan_required = false;
};

// hot reload コールバックの型。
//
// ACS 規約により全 noexcept、関数ポインタのみ採用 (FDevConsole 等と同規約)。
// user は Register 時に渡したコンテキストポインタ (this 想定)。
// ev はイベントの詳細を表し、呼び出しスコープ中のみ有効。
using FHotReloadCallback =
    void(*)(void* user, const FHotReloadEvent& ev) noexcept;

// 互換 alias。新規コードでは FHotReloadCallback を使う。
using HotReloadCallback = FHotReloadCallback;

// 監視ディレクトリ 1 件あたりの OS watcher 状態 (前方宣言)。
//
// Windows では ReadDirectoryChangesW の HANDLE + OVERLAPPED + 受信バッファを持つ。
// OVERLAPPED のアドレスは I/O 発行から完了まで安定している必要があるため、本体は
// `.cpp` 側で定義し、ここでは前方宣言だけに留めて TUniquePtr で個別 heap 確保する
// (TArray 再確保による移動を回避)。
struct FWatchEntry;

// 開発時のファイル変更を監視し、登録済みコールバック群へ dispatch するハブ。
//
// watched パスのリスト・コールバック登録の集中点・pending event の FIFO バッファを
// 提供する。実 FS poll は Tick(f32) 内でプラットフォーム固有 watcher を駆動する。
// Ship build (ACS_GAME_SHIPPING 定義時) では全 public メソッドが no-op になり、
// event は 1 つも届かない (dev tool を Ship build で完全に消す方針)。所有する
// watch・callback・event コンテナの一意性を保つため非コピー・非ムーブ。
class FHotReloadWatcher {
public:
    // 信頼できない editor 入力による queue の無制限増加を防ぐ上限。
    static constexpr u32 kMaxWatchedPaths   = 256u;
    static constexpr u32 kMaxDirectoryWatches = 64u;
    static constexpr u32 kMaxCallbacks      = 64u;
    static constexpr u32 kMaxPendingEvents  = 1024u;
    static constexpr u32 kMaxPathBytes      = 4096u;
    static constexpr f32 kMaxDebounceSeconds = 60.0f;

    // 空状態で構築する。OS watcher ハンドルは TryWatchDirectory で開く。
    //
    // FWatchEntry の完全型が見える `.cpp` で定義し、コンストラクタの
    // 失敗後始末が不完全型の TUniquePtr 破棄を外部 TU で実体化しないようにする。
    FHotReloadWatcher() noexcept;

    // 破棄する (out-of-line)。
    //
    // TUniquePtr<FWatchEntry> の解放には完全型が要るが FWatchEntry は `.cpp` でのみ
    // 完全になるため、デストラクタは out-of-line で定義する (ship build では空)。
    ~FHotReloadWatcher() noexcept;

    // コピー禁止 (所有する watch・callback・event 状態を一意に保つため)。
    FHotReloadWatcher(const FHotReloadWatcher&)            = delete;

    // コピー代入も禁止。
    FHotReloadWatcher& operator=(const FHotReloadWatcher&) = delete;

    // ムーブ禁止。
    FHotReloadWatcher(FHotReloadWatcher&&)                 = delete;

    // ムーブ代入も禁止。
    FHotReloadWatcher& operator=(FHotReloadWatcher&&)      = delete;

    // 内部バッファを予約する。OS watcher は開かない。多重呼び出し可。
    void Init() noexcept;

    // 後始末する (多重呼び出し可)。
    //
    // watched paths / callbacks / pending events を全クリアし、OS watcher
    // ハンドルも閉じる。
    void Shutdown() noexcept;

    // ディレクトリを監視対象に追加する。
    //
    // path を検証して所有コピーする。この互換 wrapper は checked 結果を無視するため、
    // 新規コードでは TryWatchDirectory を使う。
    // dir_path は監視するディレクトリの UTF-8 パス。
    // recursive が true ならサブディレクトリも含めて監視する希望を立てる。
    void WatchDirectory(const char* dir_path, bool recursive = true) noexcept;

    // checked directory 登録。
    //
    // UTF-8 path を所有コピーする。文字変換、確保、OS handle 作成、最初の
    // 非同期 read がすべて成功した後だけ WatchedCount を変更する。
    EHotReloadResult TryWatchDirectory(
        const char* dir_path, bool recursive = true) noexcept;

    // 単一ファイルを監視対象に追加する。
    //
    // WatchDirectory との違いは「単一 path のみ」という意図表明だけで、内部的には
    // watched paths に積む。path は検証して所有コピーする。失敗理由が必要な新規コード
    // では TryWatchFile を使う。
    // file_path は監視するファイルの UTF-8 パス。
    void WatchFile(const char* file_path) noexcept;

    // 所有する UTF-8 file path を上限付きで checked 登録する。
    EHotReloadResult TryWatchFile(const char* file_path) noexcept;

    // 指定 path を監視対象から外す。
    //
    // 文字列の完全一致のみ削除。未登録 / null は no-op。
    // path は監視解除するパス。
    void Unwatch(const char* path) noexcept;

    // (cb, user) のペアをコールバックとして登録する。
    //
    // 同一 (cb, user) の重複登録は no-op で弾く。cb が null なら登録せず警告ログ。
    // cb は変更検出時に呼ばれるコールバック。
    // user は cb に渡すコンテキストポインタであり、所有しない。
    void RegisterCallback(FHotReloadCallback cb, void* user) noexcept;

    // callback を上限付きで checked 登録する。
    EHotReloadResult TryRegisterCallback(
        FHotReloadCallback cb, void* user) noexcept;

    // 完全一致する (callback, user) pair を 1 件解除する。
    EHotReloadResult UnregisterCallback(
        FHotReloadCallback cb, void* user) noexcept;

    // 同一 path event の coalescing 間隔を設定する。
    //
    // seconds には [0, kMaxDebounceSeconds] の有限値を指定する。
    EHotReloadResult TrySetDebounceSeconds(f32 seconds) noexcept;

    // 現在の同一 path event coalescing 間隔を返す。
    f32 DebounceSeconds() const noexcept;

    // 独自または非 Windows の変更元から event を queue へ追加する。
    //
    // path は検証して所有コピーする。queue 上限と debounce は native event
    // と共通。`modified_timestamp` は単調増加するミリ秒値で、同じ変更元かつ同じ
    // path の timestamp 同士だけを比較する。次に成功した Tick で dispatch する。
    EHotReloadResult TryEnqueueEvent(
        const char* file_path, u64 modified_timestamp,
        bool removed = false) noexcept;

    // 1 フレーム進める。
    //
    // OS watcher の poll → pending events への push → 登録済み callback への
    // dispatch を行う。dt は前フレームからの経過秒。
    void Tick(f32 dt) noexcept;

    // checked frame 更新。無効または負の dt と Tick 再入は、poll や dispatch を行わず
    // 拒否する。NativeOverflow は Windows が通知欠落を報告した状態で、監視は再 arm
    // するが呼び出し側で authoritative rescan が必要。OsError は poll または再 arm
    // の失敗を表す。
    EHotReloadResult TryTick(f32 dt) noexcept;

    // 現在監視中の path 数を返す。
    u32 WatchedCount() const noexcept;

    // pending event バッファに溜まった未処理 event 数を返す。
    u32 PendingEventCount() const noexcept;

    // 現在の通知パイプライン診断値を allocation-free で値コピーする。
    //
    // 読み取り専用 snapshot であり、watcher の queue や診断状態を変更しない。
    // 単一 thread 専用契約は他の API と同じ。
    FHotReloadDiagnostics CaptureDiagnostics() const noexcept;

    // 診断 counter、直近失敗、authoritative rescan 要求を明示的にクリアする。
    //
    // 監視登録、callback、pending event には触れない。Shutdown も診断値を暗黙 clear
    // しないため、復旧処理と観測が完了した時点で呼ぶ。
    void ClearDiagnostics() noexcept;

    // pending event を 1 件取り出す (FIFO)。
    //
    // file_path は次の ConsumeNextEvent、ClearEvents、Shutdown のいずれかを呼ぶまで
    // 有効。取り出した event を out へ書き込み、成功時は true、空なら false を返す。
    bool ConsumeNextEvent(FHotReloadEvent& out) noexcept;

    // pending event を全クリアする (callback dispatch 済みかは問わない)。
    //
    // callback 中に呼んだ場合、現在 event の残り callback は完走するが、外側の
    // FIFO drain はその event 後に止まる。続けて enqueue した event は次の
    // TryTick まで配信しない。
    void ClearEvents() noexcept;

private:
#ifndef ACS_GAME_SHIPPING
    // pending event 先頭 1 件を物理削除する。
    //
    // m_PendingEvents / m_EventPaths を lockstep で先頭 shift-left する。
    // FIFO 順を保つため swap ではなく shift を使う。
    void RemoveFrontEventPair() noexcept;

    // 検証済み所有 event を lockstep を崩さず追加または coalesce する。
    EHotReloadResult TryQueueEvent(
        FString&& path, u64 timestamp, bool removed) noexcept;

    // 完了済み ReadDirectoryChangesW buffer を検証・変換して queue へ反映する。
    //
    // native record の拒否と通知欠落は本関数内で診断へ記録する。返却値は同じ
    // buffer 内で観測した失敗のうち、TryTick の返却優先度が最も高い結果。
    EHotReloadResult ProcessNativeNotificationBuffer(
        const void* bytes, usize byte_count, FStringView directory_path,
        bool force_conversion_out_of_memory = false) noexcept;

    // native 通知欠落後の再 arm 結果を診断へ記録し、公開結果へ変換する。
    //
    // 実 watcher と test fault seam の両方が本経路を使い、loss counter・rescan latch・
    // last_failure の配線を一致させる。
    EHotReloadResult HandleNativeOverflowCompletion(
        bool rearm_succeeded) noexcept;

    // 非 Success 結果と、event 拒否・通知欠落の診断値を飽和加算する。
    void RecordDiagnosticFailure(
        EHotReloadResult result, bool rejected_event,
        bool notification_lost) noexcept;

    // コールバックエントリ (POD、trivially copyable)。
    struct FCallbackEntry {
        // 呼び出すコールバック関数ポインタ。
        FHotReloadCallback cb   = nullptr;

        // cb に渡すコンテキストポインタ。
        void*             user = nullptr;
    };

    // 所有する UTF-8 監視 path 群。
    TArray<FString>           m_WatchedPaths;

    // 登録済み (cb, user) ペアの集合。
    TArray<FCallbackEntry>    m_Callbacks;

    // pending event の FIFO バッファ (Tick で push、Consume で pop)。
    TArray<FHotReloadEvent>   m_PendingEvents;

    // pending event が指す path 文字列の実体 (OS 由来の WCHAR を UTF-8 化して所有)。
    //
    // m_PendingEvents と常に lockstep: Tick で同時 push、Consume / dispatch で同時に
    // 先頭 shift、Clear で同時消し。file_path は cache せず Consume / dispatch 時に
    // m_EventPaths[0].Data() から解決する (TArray 再確保で SSO 文字列のアドレスが
    // 動いても dangling しないため)。
    TArray<FString>          m_EventPaths;

    // 直近の ConsumeNextEvent が返した path を所有する。
    FString                  m_LastConsumedPath;

    // OS watcher 状態 (WatchDirectory ごとに 1 entry)。
    //
    // Shutdown で HANDLE を閉じる。OVERLAPPED のアドレス安定化のため
    // TUniquePtr で個別 heap 確保する。
    TArray<TUniquePtr<FWatchEntry>> m_Watchers;

    f32  m_DebounceSeconds = 0.05f;
    bool m_Dispatching = false;
    bool m_AbortDispatch = false;
    bool m_StopDrainAfterCurrentEvent = false;

    // 成功では消えない通知パイプライン診断値。
    FHotReloadDiagnostics m_Diagnostics{};
#endif
};

} // namespace acs::game
