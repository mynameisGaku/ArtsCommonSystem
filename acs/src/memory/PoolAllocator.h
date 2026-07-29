// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

/** 要素型と容量を固定する型付きプールの前方宣言。 */
template<typename T, usize Capacity>
class TTypedPoolAllocator;

/**
 * 同サイズ小ブロックに特化したスレッドセーフな固定サイズプール。
 *
 * @details
 * 構築時に block_size×block_count の連続ストレージを 1 回確保し、全ブロックを単方向
 * フリーリストに連結する。フリーリストとブロックごとの所有状態を同じ軽量ロックで保護し、
 * 利用者領域へ公開済みのノードを並行して読まない。パーティクル・ノード・コンポーネント等の
 * 大量確保/解放に向く。
 */
class FPoolAllocator final : public FAllocator {
public:
    /**
     * 固定サイズブロックのプールを構築し、ストレージを 1 回確保する。
     *
     * @details
     * block_size は最低 sizeof(FNode) かつ alignment の倍数に切り上げられる。alignment は
     * 最低 sizeof(void*)。block_size×block_count のオーバーフローや確保失敗時は空プール
     * (BlockCount()==0) になる。初期フリーリスト連結はシングルスレッド前提。
     * @param RequestedBlockSize 1 ブロックの要求サイズ (内部で切り上げ)。
     * @param RequestedBlockCount ブロック総数。
     * @param Alignment 各ブロックのアライメント (既定 kDefaultAlignment)。
     * @param BackingAllocator ストレージの確保元 (nullptr なら DefaultAllocator)。
     */
    FPoolAllocator(usize RequestedBlockSize, usize RequestedBlockCount, usize Alignment = kDefaultAlignment, FAllocator* BackingAllocator = nullptr) noexcept;

    /** ストレージを backing に返して破棄する。 */
    ~FPoolAllocator() noexcept override;

    /** コピー禁止 (ストレージを単独所有するため)。 */
    FPoolAllocator(const FPoolAllocator&) = delete;

    /** コピー代入も禁止。 */
    FPoolAllocator& operator=(const FPoolAllocator&) = delete;

    /**
     * フリーリストから 1 ブロックを取り出して返す。
     *
     * @details Size がブロックサイズ超、Alignment がプールの整列超、プール枯渇時は nullptr。返す領域は常に 1 ブロック分。
     * @param Size 要求サイズ (m_BlockSize 以下であること)。
     * @param Alignment 要求アライメント (プールの整列以下であること)。
     * @param Location 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した 1 ブロック (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * 型・サイズ検査済みの呼び出し元向けに 1 ブロックを確保する。
     *
     * @return 確保したブロック。枯渇時は nullptr。
     */
    void* AllocBlock() noexcept;

    /**
     * 1 回のロックで複数ブロックを確保する。
     *
     * @param Output 取得ポインタの出力配列。未取得分は nullptr にする。
     * @param Count 要求ブロック数。
     * @return 実際に取得できたブロック数。
     */
    usize AllocBatch(void** Output, usize Count) noexcept;

    /**
     * ブロックをフリーリストへ返す。
     *
     * @details nullptr、外部ポインタ、ブロック途中のポインタ、二重解放は安全に拒否する。
     * @param Pointer このプールが払い出したブロック (nullptr 可)。
     */
    void Free(void* Pointer) noexcept override;

    /**
     * 1 回のロックで複数ブロックを返却する。
     *
     * nullptr、外部ポインタ、途中ポインタ、重複指定は無視する。
     *
     * @param Pointers 返却対象ポインタの配列。
     * @param Count 配列要素数。
     * @return 実際に返却できたブロック数。
     */
    usize FreeBatch(void* const* Pointers, usize Count) noexcept;

    /**
     * 1 ブロックのサイズを返す (切り上げ後)。
     *
     * @return 切り上げ済みのブロックサイズ (バイト)。
     */
    u64 BlockSize() const noexcept
    {
        return m_BlockSize;
    }

    /**
     * ブロック総数を返す。
     *
     * @return プールが保持するブロック数 (確保失敗時 0)。
     */
    u64 BlockCount() const noexcept
    {
        return m_BlockCount;
    }

    /**
     * 現在の使用バイト数を返す。
     *
     * @return 生存ブロック数 × ブロックサイズ。
     */
    u64 BytesAllocated() const noexcept override
    {
        return m_Live.Load(EMemoryOrder::Acquire) * m_BlockSize;
    }

    /**
     * 現在払い出し中のブロック件数を返す。
     *
     * @return Free されていないプールブロック数。
     */
    u64 AllocationCount() const noexcept override
    {
        return m_Live.Load(EMemoryOrder::Acquire);
    }

    /** バッチ化効果の計測に使う累積ロック取得回数を返す。 */
    u64 LockAcquisitionCount() const noexcept;

    /**
     * 識別名を返す。
     *
     * @return 文字列 "TPool"。
     */
    const char* Name() const noexcept override
    {
        return "TPool";
    }

    /**
     * Pointer がこのプールのブロック先頭かを判定する。
     *
     * @details Heap フォールバック等との区別に使う。払い出し中かどうかは判定しない。
     * @param Pointer 判定対象のポインタ。
     * @return プールのいずれかのブロック先頭なら true。
     */
    bool Contains(const void* Pointer) const noexcept
    {
        if (!m_Storage || !Pointer) return false;
        const uptr StorageAddress = reinterpret_cast<uptr>(m_Storage);
        const uptr PointerAddress = reinterpret_cast<uptr>(Pointer);
        const usize StorageSize = static_cast<usize>(m_BlockSize * m_BlockCount);
        if (PointerAddress < StorageAddress || PointerAddress - StorageAddress >= StorageSize) return false;
        return ((PointerAddress - StorageAddress) % m_BlockSize) == 0u;
    }

private:
    template<typename, usize>
    friend class TTypedPoolAllocator;

    /**
     * typed Destroy の重複実行を防ぐため、払い出し状態を破棄中へ遷移する。
     *
     * @param Pointer このプールが払い出した構築済みオブジェクト。
     * @return 払い出し中から破棄中へ遷移できた場合は true。
     */
    bool TryBeginDestroyBlock(void* Pointer) noexcept;

    /**
     * デストラクタ完了後のブロックをフリーリストへ戻す。
     *
     * @param Pointer TryBeginDestroyBlock に成功したブロック。
     */
    void FinishDestroyBlock(void* Pointer) noexcept;

    /** フリーリストノード (フリーブロックの先頭にオーバーレイ配置)。 */
    struct FNode {
        /** 次のフリーブロックへのリンク。 */
        FNode* next;
    };

    /** ブロック配列の先頭 (backing から 1 回確保、失敗時 nullptr)。 */
    u8* m_Storage = nullptr;

    /** 各ブロックの所有状態。0 は空き、1 は払い出し中、2 は破棄中。 */
    u8* m_AllocationStates = nullptr;

    /** 切り上げ済みの 1 ブロックサイズ。 */
    u64 m_BlockSize = 0;

    /** ブロック総数 (確保失敗時 0)。 */
    u64 m_BlockCount = 0;

    /** 各ブロックのアライメント (切り上げ済み)。 */
    u64 m_Alignment = 0;

    /** ストレージの確保元アロケータ。 */
    FAllocator* m_Backing = nullptr;

    /** 現在使用中のブロック数 (統計用)。 */
    TAtomic<u64> m_Live{0};

    /** Alloc/Free 系がフリーリスト用ロックを取得した累積回数。 */
    u64 m_LockAcquisitions = 0u;

    /** フリーリストの先頭。m_Lock の保護下でのみ読み書きする。 */
    FNode* m_FreeHead = nullptr;

    /** フリーリストと所有状態を一体で保護する。 */
    mutable FMutex m_Lock;
};

} // namespace acs
