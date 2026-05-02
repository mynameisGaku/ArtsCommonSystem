// =============================================================================
// ACS Memory — リニア（バンプ）アロケータ
// -----------------------------------------------------------------------------
// 単一の連続メモリブロックを「カーソルを進めるだけ」で確保する超高速
// アロケータ。Free は no-op で、Reset() で全体を巻き戻す。
//
// 用途:
//   - フレームスクラッチ（毎フレーム最初に Reset）
//   - 短命オブジェクトのバッチ確保（パース結果、コマンドバッファ等）
//
// スレッド安全性:
//   - Alloc はアトミック CAS で衝突回避（マルチスレッド OK）
//   - Reset は他スレッドの Alloc 中に呼んではいけない
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class LinearAllocator final : public Allocator {
public:
    // capacity バイトの連続バッファを backing から確保する。
    // backing が null なら DefaultAllocator() を使う。
    LinearAllocator(usize capacity, Allocator* backing = nullptr) noexcept;
    ~LinearAllocator() noexcept override;

    // コピー禁止
    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override;  // no-op（個別解放は不可）

    // カーソルを 0 に戻す（全確保済みメモリが「無効」になる）。
    // 並行 Alloc 中に呼ぶと UB。
    void Reset() noexcept;

    u64 BytesAllocated() const noexcept override { return _used.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    u64 Capacity()       const noexcept          { return _capacity; }
    const char* Name()   const noexcept override { return "Linear"; }

private:
    u8*           _base     = nullptr;        // 確保したバッファ先頭
    u64           _capacity = 0;              // バッファ長
    Allocator*    _backing  = nullptr;        // バッファ確保元
    bool          _owns_backing = false;
    Atomic<u64>   _used {0};                  // 現在のカーソル位置
    mutable Atomic<u64> _peak {0};            // ピーク位置
};

} // namespace acs
