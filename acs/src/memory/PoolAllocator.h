// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — 固定サイズブロックプール
// -----------------------------------------------------------------------------
// 同サイズの小オブジェクトを大量に確保・解放するパターンに特化。
// 例: パーティクル、ノード、コンポーネント、タスクオブジェクト。
//
// アルゴリズム:
//   - 起動時に N 個分の連続バッファを確保
//   - フリーリストを「単方向リンクスタック」として管理
//   - Alloc/Free と所有状態を同一の軽量ロックで保護
//   - 二重解放、外部ポインタ、ブロック途中のポインタを拒否
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

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
     * block_size は最低 sizeof(Node) かつ alignment の倍数に切り上げられる。alignment は
     * 最低 sizeof(void*)。block_size×block_count のオーバーフローや確保失敗時は空プール
     * (BlockCount()==0) になる。初期フリーリスト連結はシングルスレッド前提。
     * @param RequestedBlockSize 1 ブロックの要求サイズ (内部で切り上げ)。
     * @param RequestedBlockCount ブロック総数。
     * @param Alignment 各ブロックのアライメント (既定 kDefaultAlignment)。
     * @param BackingAllocator ストレージの確保元 (nullptr なら DefaultAllocator)。
     */
    FPoolAllocator(usize RequestedBlockSize, usize RequestedBlockCount, usize Alignment = kDefaultAlignment,
                   FAllocator* BackingAllocator = nullptr) noexcept;

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
     * ブロックをフリーリストへ返す。
     *
     * @details nullptr、外部ポインタ、ブロック途中のポインタ、二重解放は安全に拒否する。
     * @param Pointer このプールが払い出したブロック (nullptr 可)。
     */
    void Free(void* Pointer) noexcept override;

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
    /** フリーリストノード (フリーブロックの先頭にオーバーレイ配置)。 */
    struct Node {
        /** 次のフリーブロックへのリンク。 */
        Node* next;
    };

    /** ブロック配列の先頭 (backing から 1 回確保、失敗時 nullptr)。 */
    u8* m_Storage = nullptr;

    /** 各ブロックが払い出し中かを保持する所有状態配列。 */
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

    /** フリーリストの先頭。m_Lock の保護下でのみ読み書きする。 */
    Node* m_FreeHead = nullptr;

    /** フリーリストと所有状態を一体で保護する。 */
    FMutex m_Lock;
};

} // namespace acs
