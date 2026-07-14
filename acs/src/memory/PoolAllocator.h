// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — 固定サイズブロックプール（Treiber スタック方式）
// -----------------------------------------------------------------------------
// 同サイズの小オブジェクトを大量に確保・解放するパターンに特化。
// 例: パーティクル、ノード、コンポーネント、タスクオブジェクト。
//
// アルゴリズム:
//   - 起動時に N 個分の連続バッファを確保
//   - フリーリストを「単方向リンクスタック」として管理
//   - Alloc = head を CAS で取り出し、Free = head に CAS で push
//
// ABA 問題対策:
//   Treiber スタックの古典的問題を、ポインタ上位ビットに ABA タグを
//   埋め込む方式で回避（Windows x64 のユーザ空間ポインタは下位 47 ビット
//   しか使わないので、上位 17 ビットがタグ用に空いている）。
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

/**
 * 同サイズ小ブロックに特化した lock-free 固定サイズプール (Treiber スタック方式)。
 *
 * @details
 * 構築時に block_size×block_count の連続ストレージを 1 回確保し、全ブロックを単方向
 * フリーリストに連結する。Alloc=head の pop、Free=head への push を 64bit CAS 1 回で行う。
 * ABA 問題は head ポインタ (x64 ユーザ空間 47bit) の上位 17bit に世代タグを埋め込む方式で
 * 回避する (DCAS 不要)。パーティクル・ノード・コンポーネント等の大量確保/解放に向く。
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
     * フリーリストから 1 ブロックを pop して返す (Treiber スタックの pop)。
     *
     * @details Size がブロックサイズ超、Alignment がプールの整列超、プール枯渇時は nullptr。返す領域は常に 1 ブロック分。
     * @param Size 要求サイズ (m_BlockSize 以下であること)。
     * @param Alignment 要求アライメント (プールの整列以下であること)。
     * @param Location 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した 1 ブロック (失敗時 nullptr)。
     */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override;

    /**
     * ブロックをフリーリストへ push して返す (Treiber スタックの push)。
     *
     * @details nullptr は no-op。このプール由来でないポインタを渡すと UB (検証は Contains で行う)。
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
     * Pointer がこのプールのストレージ範囲内かを判定する。
     *
     * @details Heap フォールバック等との区別に使う。アライメント (= 実ブロック先頭) までは検証しない。
     * @param Pointer 判定対象のポインタ。
     * @return プールのストレージ範囲内なら true。
     */
    bool Contains(const void* Pointer) const noexcept
    {
        if (!m_Storage || !Pointer) return false;
        const u8* const StoragePointer = static_cast<const u8*>(Pointer);
        const u8* const StorageEnd = m_Storage + m_BlockSize * m_BlockCount;
        return StoragePointer >= m_Storage && StoragePointer < StorageEnd;
    }

private:
    /** フリーリストノード (フリーブロックの先頭にオーバーレイ配置)。 */
    struct Node {
        /** 次のフリーブロックへのリンク。 */
        Node* next;
    };

    /** ABA タグ付きポインタ (16B、現状未使用。将来 DCAS へ切り替える際用)。 */
    struct alignas(16) TaggedPtr {
        /** フリーブロックへのポインタ。 */
        Node* ptr;

        /** ABA 検出用の世代タグ。 */
        u64 tag;
    };

    /** ブロック配列の先頭 (backing から 1 回確保、失敗時 nullptr)。 */
    u8* m_Storage = nullptr;

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

    /** フリーリスト head と ABA タグを 1 ワードにパックしたもの (上位 17bit=タグ、下位 47bit=ポインタ)。 */
    TAtomic<u64> m_HeadPacked{0};
};

} // namespace acs
