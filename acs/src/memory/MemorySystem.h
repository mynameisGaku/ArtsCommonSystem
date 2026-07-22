// SPDX-License-Identifier: Apache-2.0
// セグメント別メモリ管理ファサード（mimalloc / frame arena + 予算 + 診断）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/Segment.h"

namespace acs {

/** 1 セグメントの初期化設定。 */
struct FSegmentConfig {
    /** 対象セグメントの種別。 */
    ESegment segment = ESegment::Default;

    /** 要求バイト合計のハード上限。0 は無制限。 */
    usize hard_budget_bytes = 0;

    /** true なら一括 Reset する frame arena、false なら mimalloc first-class heap。 */
    bool use_frame_allocator = false;
};

/** FMemorySystem 全体の初期化設定。 */
struct FMemorySystemConfig {
    /** セグメント種別ごとの設定 (ESegment::_Count 個)。 */
    FSegmentConfig segments[(usize)ESegment::_Count];

    /**
     * true なら Init で既定アロケータを Default セグメントへ差し替える。
     *
     * @details
     * コンテナ等の既定確保を mimalloc first-class heap (MT スケール / 予算 / 可観測性) に
     * 通し、Shutdown で元へ確実に復元する。Default セグメントの hard_budget_bytes は
     * ゲームのピーク汎用使用量に十分な値を設定すること。
     * 既定 false (= 従来どおり HeapAlloc)。
     */
    bool install_as_default_allocator = false;
};

/** 1 セグメントの統計スナップショット。 */
struct FSegmentStats {
    /** セグメント種別。 */
    ESegment segment;

    /** セグメント名 (ToString による静的文字列)。 */
    const char* segment_name = "";

    /** 実際に使われている具象アロケータ名。 */
    const char* allocator_name = "";

    /** 現在生存している確保の要求バイト合計。 */
    u64 requested_bytes = 0;

    /** 過去に同時生存した要求バイト合計の最大値。 */
    u64 peak_requested_bytes = 0;

    /** 要求バイト合計のハード上限。0 は無制限。 */
    u64 hard_budget_bytes = 0;

    /** 現在の未解放割り当て件数。 */
    u64 outstanding_allocation_count = 0;
};

/** 保守点でバックエンドを独立走査した 1 セグメントの診断結果。 */
struct FMemorySegmentInspection {
    /** 検査対象セグメント。 */
    ESegment segment = ESegment::Default;

    /** 実際に使われている具象アロケータ名。 */
    const char* allocator_name = "";

    /** バックエンド走査で列挙した仮想予約バイト数。 */
    u64 reserved_bytes = 0;

    /** バックエンド走査で列挙したコミット済みバイト数。 */
    u64 committed_bytes = 0;

    /** バックエンドが報告した生存ブロックの usable size 合計。 */
    u64 usable_bytes = 0;

    /** バックエンド走査で復元した生存確保の要求バイト合計。 */
    u64 requested_bytes = 0;

    /** バックエンド走査で列挙した生存確保件数。 */
    u64 outstanding_allocation_count = 0;

    /** この具象アロケータが独立走査を提供するか。 */
    bool backend_inspection_supported = false;

    /** バックエンド走査が完走したか。 */
    bool visit_succeeded = false;

    /** 全ブロックの ACS 管理メタデータが正しかったか。 */
    bool metadata_valid = false;

    /** 独立走査値が通常の atomic 統計と一致したか。 */
    bool matches_authoritative_statistics = false;
};

/** FMemorySystem の終了診断に使う未解放メモリ集計。 */
struct FMemoryLeakSummary {
    /** Temp を除く全セグメントの未解放バイト数。 */
    u64 outstanding_bytes = 0;

    /** 未解放メモリが存在するセグメント数。 */
    u32 leaking_segment_count = 0;

    /** Temp を除く全セグメントの未解放割り当て件数。 */
    u64 outstanding_allocation_count = 0;

    /** 未解放メモリが1バイト以上あるかを返す。 */
    constexpr bool HasLeaks() const noexcept
    {
        return outstanding_bytes != 0 || outstanding_allocation_count != 0;
    }
};

/** 割り当て元追跡の差分開始位置。 */
struct FMemoryTrackingCheckpoint {
    /** チェックポイント取得時点で最後に発行済みの割り当て番号。 */
    u64 allocation_sequence = 0;
};

/** デバッグ追跡表から取得する未解放割り当て情報。 */
struct FOutstandingMemoryAllocation {
    /** 利用者へ返したメモリアドレス。 */
    const void* address = nullptr;

    /** 呼び出し元が要求したバイト数。 */
    u64 requested_bytes = 0;

    /** 呼び出し元が要求したアライメント。 */
    u64 alignment = 0;

    /** この確保へ一意に付与した単調増加番号。 */
    u64 allocation_sequence = 0;

    /** 確保先のメモリセグメント。 */
    ESegment segment = ESegment::Default;

    /** 確保を呼び出したソースファイル。 */
    const char* source_file = "";

    /** 確保を呼び出した関数。 */
    const char* source_function = "";

    /** 確保を呼び出した行番号。 */
    u32 source_line = 0;

    /** 確保を呼び出した列番号。 */
    u32 source_column = 0;
};

/** 割り当て元追跡の収集結果。 */
struct FMemoryTrackingReport {
    /** チェックポイントより後に残っている割り当て件数。 */
    u64 outstanding_allocation_count = 0;

    /** 上記割り当ての要求サイズ合計。 */
    u64 outstanding_requested_bytes = 0;

    /** 出力配列へ実際に書き込んだ件数。 */
    u64 written_allocation_count = 0;

    /** これまでに発行した最新の割り当て番号。 */
    u64 newest_allocation_sequence = 0;

    /** 追跡表の確保失敗などで記録できなかったイベント数。 */
    u64 dropped_tracking_event_count = 0;

    /** このビルドで割り当て元追跡が有効か。 */
    bool tracking_enabled = false;

    /** 追跡イベントの欠落がなく、一覧を完全とみなせるか。 */
    constexpr bool IsComplete() const noexcept
    {
        return tracking_enabled && dropped_tracking_event_count == 0;
    }
};

/** セグメント別メモリ管理のファサード (mimalloc / frame arena + 予算 + 診断、全 static)。 */
class FMemorySystem {
public:
    /**
     * 全セグメントを設定で初期化する。
     *
     * @details 多重 Init はエラー。Shutdown とは直列化され、途中で失敗した場合は確保済みスロットをロールバックする。
     * 初期化が完了するまで、以前取得したセグメントアロケータを含む公開操作は失敗として返す。
     * install_as_default_allocator 時は既定アロケータも差し替える。
     * @param Configuration セグメントごとの設定。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    static TResult<void> Init(const FMemorySystemConfig& Configuration) noexcept;

    /**
     * 全セグメントを解放する。
     *
     * @details 新しい公開操作を拒否し、開始済みの Alloc / Free / Realloc / 統計取得が完了してから破棄する。
     * install_as_default_allocator で差し替えた既定アロケータは診断前に復元する。未初期化なら no-op。
     * 操作間で呼び出し元が保持するポインタの利用期間までは追跡しないため、利用中のジョブを先に停止すること。
     * 前回の初期化寿命で取得したポインタを、再初期化後の Realloc / Free へ渡してはならない。
     */
    static void Shutdown() noexcept;

    /**
     * 小規模テスト・すぐ動かす用の既定設定を返す。
     *
     * @return 各セグメントに実用的なハード予算を設定した FMemorySystemConfig。
     */
    static FMemorySystemConfig DefaultConfig() noexcept;

    /**
     * 指定セグメントのアロケータを返す。
     *
     * @param Segment セグメント種別。
     * @return セグメントのアロケータ (Init 前と Shutdown 開始後は nullptr)。
     * 返したアダプタのアドレスは再初期化後も安定しているが、非稼働中の操作は失敗する。
     */
    static FAllocator* Get(ESegment Segment) noexcept;

    /**
     * 現在のセグメント (FScopedMemorySegment が設定した TLS の値) を返す。
     *
     * @return 現在のセグメント種別。
     */
    static ESegment Current() noexcept;

    /**
     * 現在のセグメントのアロケータを返す。
     *
     * @return 現在セグメントのアロケータ (Init 前は nullptr)。
     */
    static FAllocator* CurrentAllocator() noexcept;

    /**
     * Temp セグメントを巻き戻す (フレーム先頭で 1 回呼ぶ)。
     *
     * @details
     * Temp の arena をページ保持のまま Reset し、予算予約と割り当て追跡も同じ排他区間で巻き戻す。
     * 開始済みの Temp Alloc/Free/Realloc が完了するまで待ち、Reset 開始後の新しい操作は失敗として返す。
     * Temp の Free / Realloc は現在世代の allocation 先頭だけを受理し、foreign / interior / 解放済みを拒否する。
     * Realloc のコピー量は呼び出し側の旧サイズではなく、確保時ヘッダへ記録したサイズで制限する。
     * Shutdown 開始後の呼び出しは何もせず、進行中の Reset は Shutdown の破棄前に完了する。
     * Reset 後は以前の Temp ポインタがすべて無効になるため、データを利用するジョブは呼び出し前に停止すること。
     */
    static void ResetTemp() noexcept;

    /**
     * 全セグメントの統計を Output に詰める。
     *
     * @param Output 統計を書き込む配列。
     * @param OutputCapacity Output の容量。
     * @return 実際に書き込んだ要素数。Init 前と Shutdown 開始後は 0。
     */
    static u32 GetStats(FSegmentStats* Output, u32 OutputCapacity) noexcept;

    /**
     * 指定セグメントの具象アロケータを独立走査する。
     *
     * @details 保守操作のため、対象セグメントへの Alloc / Free / Realloc を止めた状態で呼ぶこと。
     * frame arena は独立ブロック走査を提供しないため、通常統計だけを返す。
     * @param Segment 検査対象セグメント。
     * @return 仮想予約・コミット・生存ブロックと通常統計の整合結果。
     */
    static FMemorySegmentInspection InspectSegmentMemory(ESegment Segment) noexcept;

    /**
     * 一括Resetされるframe arenaを除く、通常ヒープの未解放メモリを集計する。
     * @return 終了時リーク判定に使える集計値。未初期化時はゼロ。
     */
    static FMemoryLeakSummary CaptureLeakSummary() noexcept;

    /**
     * 現在の割り当て番号を差分追跡の開始位置として取得する。
     *
     * @details Debug はこの後に確保・再確保された領域だけを収集できる。追跡無効ビルドではゼロを返す。
     * @return 差分収集へ渡せるチェックポイント。
     */
    static FMemoryTrackingCheckpoint CaptureMemoryTrackingCheckpoint() noexcept;

    /**
     * チェックポイントより後に残る割り当て元情報を、呼び出し側の固定長配列へ収集する。
     *
     * @details 追跡表自身は Win32 プロセスヒープを使うため、FMemorySystem へ再帰確保しない。
     * @param Checkpoint 差分開始位置。ゼロなら現在の全未解放割り当てを対象にする。
     * @param Output 書き込み先配列。件数だけ調べる場合は nullptr。
     * @param OutputCapacity Output の要素容量。
     * @return 総件数・要求バイト数・追跡完全性を含むレポート。
     */
    static FMemoryTrackingReport CollectOutstandingMemoryAllocations(FMemoryTrackingCheckpoint Checkpoint,
                                                                    FOutstandingMemoryAllocation* Output,
                                                                    u32 OutputCapacity) noexcept;

    /**
     * チェックポイントより後の未解放割り当てを ACS ログへ出力する。
     *
     * @param Checkpoint 差分開始位置。ゼロなら現在の全未解放割り当てを対象にする。
     * @param MaximumLoggedAllocationCount 詳細行の最大件数。ゼロなら集計行だけを出す。
     * @return ログ対象の総件数・要求バイト数・追跡完全性を含むレポート。
     */
    static FMemoryTrackingReport DumpOutstandingMemoryAllocations(FMemoryTrackingCheckpoint Checkpoint = {},
                                                                 u32 MaximumLoggedAllocationCount = 256) noexcept;

    /**
     * 指定した割り当て番号の確保時に、デバッガ接続中だけブレークするよう設定する。
     *
     * @details 0 を渡すと解除する。追跡無効ビルドでは no-op。
     * @param AllocationSequence ブレーク対象の割り当て番号。
     */
    static void SetBreakOnAllocationSequence(u64 AllocationSequence) noexcept;

    /**
     * このビルドでファイル・行・関数を含む割り当て元追跡が有効かを返す。
     *
     * @return 有効なら true。標準設定では Debug のみ true。
     */
    static bool IsAllocationSiteTrackingEnabled() noexcept;
};

/** RAII でカレントセグメントを切り替える (スコープ脱出で元に戻す)。 */
class FScopedMemorySegment {
public:
    /**
     * カレントセグメントを Segment に切り替える (旧値を退避)。
     *
     * @param Segment スコープ内で使うセグメント種別。
     */
    explicit FScopedMemorySegment(ESegment Segment) noexcept;

    /** カレントセグメントを退避していた値に戻す。 */
    ~FScopedMemorySegment() noexcept;

    /** コピー禁止 (スコープガードのため)。 */
    FScopedMemorySegment(const FScopedMemorySegment&) = delete;

    /** コピー代入も禁止。 */
    FScopedMemorySegment& operator=(const FScopedMemorySegment&) = delete;

private:
    /** 構築時のカレントセグメント (デストラクタで復元する)。 */
    ESegment m_Previous;
};

} // namespace acs
