// SPDX-License-Identifier: Apache-2.0
// DiligentMemoryAdapter 実装
//
// Diligent の IMemoryAllocator は以下の仮想関数を持つ:
//   void* Allocate(size_t Size, const Char* dbgInfo, const Char* dbgFile, const Int32 dbgLine);
//   void  Free(void* Ptr);
//   void* AllocateAligned(size_t Size, size_t Alignment, ...);
//   void  FreeAligned(void* Ptr);
// IObject は継承しない（軽量インターフェイス）。
#include "render/Diligent/DiligentMemoryAdapter.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "MemoryAllocator.h"  // Diligent/Common
#include "memory/Memory.h"
#include "memory/MemorySystem.h"
#include "memory/Allocator.h"
#include "foundation/SourceLoc.h"

#include <cstddef>  // max_align_t

namespace acs {

namespace {

/**
 * ACS の FAllocator へ委譲する Diligent::IMemoryAllocator 実装。
 *
 * @details
 * Diligent の確保・解放要求を backing FAllocator に流すだけの薄いラッパ。アライメントは
 * max_align_t に固定し、確保元 (ESegment 等) は backing 側の設定に従う。
 */
class AcsMemoryAllocator final : public Diligent::IMemoryAllocator {
public:
    /**
     * 委譲先アロケータを束ねてアダプタを構築する。
     *
     * @param backing 実際の確保・解放を委譲する ACS アロケータ。
     */
    explicit AcsMemoryAllocator(FAllocator* backing) noexcept : m_Backing(backing) {}

    /**
     * backing 経由でメモリを確保する。
     *
     * @details backing が null または Size が 0 のときは nullptr を返す。デバッグ引数は使わない。
     * @param Size 確保するバイト数。
     * @param dbgInfo Diligent のデバッグ用説明文字列 (未使用)。
     * @param dbgFile Diligent のデバッグ用ソースファイル名 (未使用)。
     * @param dbgLine Diligent のデバッグ用行番号 (未使用)。
     * @return 確保したメモリ (失敗時は nullptr)。
     */
    void* Allocate(size_t Size, const Diligent::Char* /*dbgInfo*/,
                   const Diligent::Char* /*dbgFile*/,
                   const Diligent::Int32 /*dbgLine*/) override {
        if (!m_Backing || Size == 0) return nullptr;
        return m_Backing->Alloc(Size, alignof(::max_align_t), FSourceLoc::Current());
    }

    /**
     * backing 経由でメモリを解放する。
     *
     * @param Ptr 解放するメモリ (null なら何もしない)。
     */
    void Free(void* Ptr) override {
        if (m_Backing && Ptr) m_Backing->Free(Ptr);
    }

private:
    /** 確保・解放を委譲する ACS アロケータ。 */
    FAllocator* m_Backing = nullptr;
};

/** プロセス寿命のアダプタシングルトン (Diligent はアロケータを所有しないので保持が必要)。 */
AcsMemoryAllocator* g_adapter = nullptr;

} // namespace

/** backing を委譲先にするアダプタをプロセス寿命で生成する。 */
void* DiligentMemoryAdapter::Create(FAllocator* backing) noexcept {
    if (!backing) return nullptr;
    if (!g_adapter) {
        // 自身も backing から確保（自己参照だが Diligent に渡る前なら安全）
        void* mem = backing->Alloc(sizeof(AcsMemoryAllocator),
                                   alignof(AcsMemoryAllocator),
                                   FSourceLoc::Current());
        if (!mem) return nullptr;
        g_adapter = ::new (mem) AcsMemoryAllocator(backing);
    }
    return static_cast<void*>(g_adapter);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
