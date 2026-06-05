// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FLinearAllocator 実装
// -----------------------------------------------------------------------------
// アトミック CAS でカーソル位置を進めるだけ。多スレッド競合下でも
// O(1)（CAS リトライ次第で実測 1-3 倍）。Free は何もしない。
// =============================================================================
#include "memory/LinearAllocator.h"
#include "memory/Memory.h"

namespace acs {

FLinearAllocator::FLinearAllocator(usize capacity, FAllocator* backing) noexcept
    : m_Capacity(capacity)
    , m_Backing(backing ? backing : &DefaultAllocator())
    , m_bOwnsBacking(false) {
    m_Base = static_cast<u8*>(m_Backing->Alloc(capacity, kDefaultAlignment, FSourceLoc::Current()));
}

FLinearAllocator::~FLinearAllocator() noexcept {
    if (m_Base) m_Backing->Free(m_Base);
}

// CAS ループでカーソルを進める：
//   1. 現在カーソル cur を読む
//   2. アライン後の位置を計算
//   3. 必要バイトを足した next を求める
//   4. 容量超過なら nullptr
//   5. CompareExchange で m_Used を cur → next に交換
//   6. 競合した場合は 1 へ戻ってリトライ
void* FLinearAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    if (size == 0 || !m_Base) return nullptr;
    if (alignment < 1) alignment = 1;
    while (true) {
        const u64 cur = m_Used.Load(EMemoryOrder::Relaxed);
        const u64 base_addr = reinterpret_cast<u64>(m_Base);
        const u64 aligned   = AlignUp(base_addr + cur, alignment) - base_addr;
        const u64 next      = aligned + size;
        if (next > m_Capacity) return nullptr;  // 予算超過
        u64 expected = cur;
        if (m_Used.CompareExchange(expected, next)) {
            // ピーク更新
            u64 peak = m_Peak.Load(EMemoryOrder::Relaxed);
            while (next > peak && !m_Peak.CompareExchange(peak, next)) {}
            return m_Base + aligned;
        }
        // 他スレッドが先に進めた — 再試行
    }
}

void FLinearAllocator::Free(void* /*ptr*/) noexcept {
    // リニアアロケータは個別解放をサポートしない（仕様）。
    // 全体の解放は Reset() で行う。
}

void FLinearAllocator::Reset() noexcept {
    m_Used.Store(0, EMemoryOrder::Release);
}

} // namespace acs
