// SPDX-License-Identifier: Apache-2.0
// セグメント別メモリ管理ファサード（VM 予約 + アロケータ + 予算）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/Segment.h"

namespace acs {

// 1 セグメントの設定
struct SegmentConfig {
    ESegment      segment;
    usize        reserve_bytes;     // 仮想予約サイズ
    usize        commit_initial;    // 初回コミット量
    usize        budget_hard_cap;   // ハード予算（超過時は alloc 失敗）
    bool         use_linear;        // true なら FLinearAllocator
};

// FMemorySystem 全体の初期化設定
struct MemorySystemConfig {
    SegmentConfig segments[(usize)ESegment::_Count];

    // true なら Init で SetDefaultAllocator(Default セグメント) を呼び、コンテナ等の既定確保を
    // シャード化 VM アロケータ (MT スケール / 安全ガード / 予算 / 可観測性) に通す。Shutdown で
    // 元の既定へ確実に復元する。注意: Default セグメントは reserve 上限を持つ (HeapAlloc のように
    // 無制限には増えない)。有効化する場合は Default セグメントの reserve をゲームのピーク汎用
    // 使用量に合わせて十分に取ること。既定 false (= 従来どおり HeapAlloc)。
    bool make_default = false;
};

// セグメントの統計
struct SegmentStats {
    ESegment     segment;
    const char* name;
    u64         reserve;
    u64         committed;
    u64         used;
    u64         peak;
    u64         budget;
};

class FMemorySystem {
public:
    // 全セグメントを設定で初期化（多重 Init はエラー）
    static TResult<void> Init(const MemorySystemConfig& cfg) noexcept;

    // 全セグメントを解放
    static void Shutdown() noexcept;

    // 既定設定（小規模テスト・デフォルト用）
    static MemorySystemConfig DefaultConfig() noexcept;

    // セグメント別アロケータ取得（Init 前は nullptr）
    static FAllocator* Get(ESegment s) noexcept;

    // 「現在のセグメント」を取得（ScopedMemorySegment が設定したもの）
    static ESegment Current() noexcept;

    // 現在セグメントのアロケータ
    static FAllocator* CurrentAllocator() noexcept;

    // Temp セグメントを巻き戻す（フレーム先頭で 1 回呼ぶ）
    static void ResetTemp() noexcept;

    // 全セグメントの統計を取得
    static u32 GetStats(SegmentStats* out, u32 max_count) noexcept;
};

// RAII でセグメントを切り替える（スコープ脱出で元に戻る）
class ScopedMemorySegment {
public:
    explicit ScopedMemorySegment(ESegment s) noexcept;
    ~ScopedMemorySegment() noexcept;

    ScopedMemorySegment(const ScopedMemorySegment&) = delete;
    ScopedMemorySegment& operator=(const ScopedMemorySegment&) = delete;

private:
    ESegment m_Previous;
};

} // namespace acs
