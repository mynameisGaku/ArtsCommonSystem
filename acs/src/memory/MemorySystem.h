// =============================================================================
// ACS Memory — MemorySystem（セグメント管理ファサード）
// -----------------------------------------------------------------------------
// 起動時に各 Segment の予算（仮想予約サイズ + 初期コミット）を設定し、
// ランタイム中は ScopedMemorySegment で「現在のセグメント」を切り替えるか、
// MemorySystem::Get(Segment) で明示的にアロケータを取り出して使う。
//
// 内部実装:
//   Default / Permanent / Resource / Develop → TlsfAllocator (VM-backed)
//   Temp                                     → LinearAllocator (毎フレーム Reset)
//
// スレッド安全性:
//   セグメントごとに内部 Mutex で保護。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/Segment.h"

namespace acs {

// 1 セグメントの設定
struct SegmentConfig {
    Segment      segment;
    usize        reserve_bytes;     // 仮想予約サイズ
    usize        commit_initial;    // 初回コミット量
    usize        budget_hard_cap;   // ハード予算（超過時は alloc 失敗）
    bool         use_linear;        // true なら LinearAllocator
};

// MemorySystem 全体の初期化設定
struct MemorySystemConfig {
    SegmentConfig segments[(usize)Segment::_Count];
};

// セグメントの統計
struct SegmentStats {
    Segment     segment;
    const char* name;
    u64         reserve;
    u64         committed;
    u64         used;
    u64         peak;
    u64         budget;
};

// =============================================================================
// MemorySystem — グローバル静的 API
// =============================================================================
class MemorySystem {
public:
    // 全セグメントを設定で初期化
    static Result<void> Init(const MemorySystemConfig& cfg) noexcept;

    // 全セグメントを解放
    static void Shutdown() noexcept;

    // 既定設定（小規模テスト・デフォルト用）
    static MemorySystemConfig DefaultConfig() noexcept;

    // セグメント別アロケータ取得。Init 前に呼ぶと nullptr。
    static Allocator* Get(Segment s) noexcept;

    // 「現在のセグメント」を取得（ScopedMemorySegment が設定したもの）
    static Segment Current() noexcept;

    // 現在のセグメント用アロケータを返す
    static Allocator* CurrentAllocator() noexcept;

    // Temp セグメントを巻き戻す（フレーム先頭で 1 回呼ぶ）
    static void ResetTemp() noexcept;

    // 全セグメントの統計を取得
    static u32 GetStats(SegmentStats* out, u32 max_count) noexcept;
};

// =============================================================================
// ScopedMemorySegment — RAII でセグメントを切り替える
// -----------------------------------------------------------------------------
// スコープ内の MemorySystem::Current() / CurrentAllocator() は指定された
// セグメントを返す。スコープ脱出で元に戻る。
// =============================================================================
class ScopedMemorySegment {
public:
    explicit ScopedMemorySegment(Segment s) noexcept;
    ~ScopedMemorySegment() noexcept;

    ScopedMemorySegment(const ScopedMemorySegment&) = delete;
    ScopedMemorySegment& operator=(const ScopedMemorySegment&) = delete;

private:
    Segment _previous;
};

} // namespace acs
