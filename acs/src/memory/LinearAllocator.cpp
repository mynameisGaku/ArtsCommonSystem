// =============================================================================
// ACS Memory — LinearAllocator 実装
// -----------------------------------------------------------------------------
// アトミック CAS でカーソル位置を進めるだけ。多スレッド競合下でも
// O(1)（CAS リトライ次第で実測 1-3 倍）。Free は何もしない。
// =============================================================================
#include "memory/LinearAllocator.h"
#include "memory/Memory.h"

namespace acs {

LinearAllocator::LinearAllocator(usize capacity, Allocator* backing) noexcept
    : _capacity(capacity)
    , _backing(backing ? backing : &DefaultAllocator())
    , _owns_backing(false) {
    _base = static_cast<u8*>(_backing->Alloc(capacity, kDefaultAlignment, SourceLoc::Current()));
}

LinearAllocator::~LinearAllocator() noexcept {
    if (_base) _backing->Free(_base);
}

// CAS ループでカーソルを進める：
//   1. 現在カーソル cur を読む
//   2. アライン後の位置を計算
//   3. 必要バイトを足した next を求める
//   4. 容量超過なら nullptr
//   5. CompareExchange で _used を cur → next に交換
//   6. 競合した場合は 1 へ戻ってリトライ
void* LinearAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0 || !_base) return nullptr;
    if (alignment < 1) alignment = 1;
    while (true) {
        u64 cur = _used.Load(MemoryOrder::Relaxed);
        u64 base_addr = reinterpret_cast<u64>(_base);
        u64 aligned   = AlignUp(base_addr + cur, alignment) - base_addr;
        u64 next      = aligned + size;
        if (next > _capacity) return nullptr;  // 予算超過
        u64 expected = cur;
        if (_used.CompareExchange(expected, next)) {
            // ピーク更新
            u64 peak = _peak.Load(MemoryOrder::Relaxed);
            while (next > peak && !_peak.CompareExchange(peak, next)) {}
            return _base + aligned;
        }
        // 他スレッドが先に進めた — 再試行
    }
}

void LinearAllocator::Free(void* /*ptr*/) noexcept {
    // リニアアロケータは個別解放をサポートしない（仕様）。
    // 全体の解放は Reset() で行う。
}

void LinearAllocator::Reset() noexcept {
    _used.Store(0, MemoryOrder::Release);
}

} // namespace acs
