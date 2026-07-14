// SPDX-License-Identifier: Apache-2.0
// 単一連続バッファのバンプアロケータ（Free は no-op、Reset で全体巻き戻し）
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

/**
 * 単一連続バッファ上をアトミック CAS で前進するバンプアロケータ。
 *
 * @details
 * 構築時に backing から capacity バイトを 1 回確保し、Alloc はカーソルを CAS で
 * 進めるだけ (lock-free、O(1))。個別 Free はサポートせず (no-op)、まとめて Reset で
 * 巻き戻す。容量を超える確保は nullptr を返す。並行 Alloc 中の Reset は UB。
 */
class FLinearAllocator final : public FAllocator {
public:
    /**
     * capacity バイトのバッファを backing から 1 回確保して構築する。
     *
     * @param BufferCapacity 確保するバッファの総バイト数。
     * @param BackingAllocator バッキングアロケータ (nullptr なら DefaultAllocator)。
     */
    FLinearAllocator(usize BufferCapacity, FAllocator* BackingAllocator = nullptr) noexcept;

    /** バッキングバッファを backing に返して破棄する。 */
    ~FLinearAllocator() noexcept override;

    /** コピー禁止 (バッファを単独所有するため)。 */
    FLinearAllocator(const FLinearAllocator&) = delete;

    /** コピー代入も禁止。 */
    FLinearAllocator& operator=(const FLinearAllocator&) = delete;

    /**
     * カーソルを alignment 整列して size バイトを切り出す。
     *
     * @details CAS リトライによる lock-free 確保。容量超過・Size==0・未構築時は nullptr。
     * @param Size 確保するバイト数。
     * @param Alignment 要求アライメント (1 未満なら 1 に補正)。
     * @param Location 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 個別解放は非対応 (no-op)。
     *
     * @details リニアアロケータは個別解放できない。全体は Reset() で巻き戻す。
     * @param Pointer 無視される。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * カーソルを 0 に戻して全確保を無効化する。
     *
     * @details バッファは保持したまま再利用する。並行 Alloc 中に呼ぶと UB。
     */
    void Reset() noexcept;

    /**
     * 現在のカーソル位置 (= 使用バイト数) を返す。
     *
     * @return 確保済みバイト数 (アライメント詰め込み込み)。
     */
    u64 BytesAllocated() const noexcept override
    {
        return m_Used.Load(EMemoryOrder::Acquire);
    }

    /**
     * 最後の Reset 以降に払い出した領域の件数を返す。
     *
     * @details 個別 Free は非対応なので、Reset するまで件数は減らない。
     * @return 現在の一括寿命に属する確保件数。
     */
    u64 AllocationCount() const noexcept override
    {
        return m_AllocationCount.Load(EMemoryOrder::Acquire);
    }

    /**
     * 過去ピークのカーソル位置を返す。
     *
     * @return これまでに到達した最大カーソル位置。
     */
    u64 PeakBytes() const noexcept override
    {
        return m_Peak.Load(EMemoryOrder::Acquire);
    }

    /**
     * バッファ総容量を返す。
     *
     * @return 構築時に確保したバイト数。
     */
    u64 Capacity() const noexcept
    {
        return m_Capacity;
    }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "Linear"。
     */
    const char* Name() const noexcept override
    {
        return "Linear";
    }

private:
    /** バッファ先頭 (backing から確保、失敗時 nullptr)。 */
    u8* m_Base = nullptr;

    /** バッファ総容量 (バイト)。 */
    u64 m_Capacity = 0;

    /** バッファの確保元アロケータ。 */
    FAllocator* m_Backing = nullptr;

    /** backing を所有しているか (現状常に false)。 */
    bool m_bOwnsBacking = false;

    /** 現在のカーソル位置 (確保済みバイト数、CAS で前進)。 */
    TAtomic<u64> m_Used{0};

    /** 最後の Reset 以降に成功した確保件数。 */
    TAtomic<u64> m_AllocationCount{0};

    /** 過去ピークのカーソル位置 (CAS で更新)。 */
    mutable TAtomic<u64> m_Peak{0};
};

} // namespace acs
