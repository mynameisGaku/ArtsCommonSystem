// SPDX-License-Identifier: Apache-2.0
// セグメント別メモリ管理ファサード（VM 予約 + アロケータ + 予算）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/Segment.h"

namespace acs {

/** 1 セグメントの初期化設定。 */
struct SegmentConfig {
    /** 対象セグメントの種別。 */
    ESegment      segment;

    /** 仮想予約サイズ (バイト)。 */
    usize        reserve_bytes;

    /** 初回コミット量 (バイト)。 */
    usize        commit_initial;

    /** ハード予算 (バイト、超過すると alloc が失敗する)。 */
    usize        budget_hard_cap;

    /** true ならフレーム/スクラッチ用途 (lock-free arena)、false なら汎用シャード化 TLSF。 */
    bool         use_linear;
};

/** FMemorySystem 全体の初期化設定。 */
struct MemorySystemConfig {
    /** セグメント種別ごとの設定 (ESegment::_Count 個)。 */
    SegmentConfig segments[(usize)ESegment::_Count];

    /**
     * true なら Init で既定アロケータを Default セグメントへ差し替える。
     *
     * @details
     * コンテナ等の既定確保をシャード化 VM アロケータ (MT スケール / 安全ガード / 予算 / 可観測性) に
     * 通し、Shutdown で元へ確実に復元する。Default セグメントは reserve 上限を持つ (HeapAlloc のように
     * 無制限には増えない) ため、有効化する場合は reserve をゲームのピーク汎用使用量に十分に取ること。
     * 既定 false (= 従来どおり HeapAlloc)。
     */
    bool make_default = false;
};

/** 1 セグメントの統計スナップショット。 */
struct SegmentStats {
    /** セグメント種別。 */
    ESegment     segment;

    /** セグメント名 (ToString による静的文字列)。 */
    const char* name;

    /** 仮想予約サイズ (バイト)。 */
    u64         reserve;

    /** コミット量 (バイト、設定値)。 */
    u64         committed;

    /** 現在の使用バイト数。 */
    u64         used;

    /** ピーク使用バイト数。 */
    u64         peak;

    /** ハード予算 (バイト)。 */
    u64         budget;
};

/** セグメント別メモリ管理のファサード (VM 予約 + アロケータ + 予算、全 static)。 */
class FMemorySystem {
public:
    /**
     * 全セグメントを設定で初期化する。
     *
     * @details 多重 Init はエラー。途中で失敗した場合は確保済みスロットをロールバックする。make_default 時は既定アロケータも差し替える。
     * @param cfg セグメントごとの設定。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    static TResult<void> Init(const MemorySystemConfig& cfg) noexcept;

    /**
     * 全セグメントを解放する。
     *
     * @details make_default で差し替えた既定アロケータを最初に復元してから各セグメントを破棄する。未初期化なら no-op。
     */
    static void Shutdown() noexcept;

    /**
     * 小規模テスト・すぐ動かす用の既定設定を返す。
     *
     * @return 各セグメントに無難な reserve/commit/budget を設定した MemorySystemConfig。
     */
    static MemorySystemConfig DefaultConfig() noexcept;

    /**
     * 指定セグメントのアロケータを返す。
     *
     * @param s セグメント種別。
     * @return セグメントのアロケータ (Init 前は nullptr)。
     */
    static FAllocator* Get(ESegment s) noexcept;

    /**
     * 現在のセグメント (ScopedMemorySegment が設定した TLS の値) を返す。
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
     * @details Temp の arena をページ保持のまま Reset し、次フレームで再確保なしに再利用できるようにする。
     */
    static void ResetTemp() noexcept;

    /**
     * 全セグメントの統計を out に詰める。
     *
     * @param out 統計を書き込む配列。
     * @param max_count out の容量。
     * @return 実際に書き込んだ要素数。
     */
    static u32 GetStats(SegmentStats* out, u32 max_count) noexcept;
};

/** RAII でカレントセグメントを切り替える (スコープ脱出で元に戻す)。 */
class ScopedMemorySegment {
public:
    /**
     * カレントセグメントを s に切り替える (旧値を退避)。
     *
     * @param s スコープ内で使うセグメント種別。
     */
    explicit ScopedMemorySegment(ESegment s) noexcept;

    /** カレントセグメントを退避していた値に戻す。 */
    ~ScopedMemorySegment() noexcept;

    /** コピー禁止 (スコープガードのため)。 */
    ScopedMemorySegment(const ScopedMemorySegment&) = delete;

    /** コピー代入も禁止。 */
    ScopedMemorySegment& operator=(const ScopedMemorySegment&) = delete;

private:
    /** 構築時のカレントセグメント (デストラクタで復元する)。 */
    ESegment m_Previous;
};

} // namespace acs
