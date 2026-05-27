// SPDX-License-Identifier: Apache-2.0
// DiligentMemoryAdapter 実装（Phase 18.7 で本実装）
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

class AcsMemoryAllocator final : public Diligent::IMemoryAllocator {
public:
    explicit AcsMemoryAllocator(FAllocator* backing) noexcept : m_Backing(backing) {}

    void* Allocate(size_t Size, const Diligent::Char* /*dbgInfo*/,
                   const Diligent::Char* /*dbgFile*/,
                   const Diligent::Int32 /*dbgLine*/) override {
        if (!m_Backing || Size == 0) return nullptr;
        return m_Backing->Alloc(Size, alignof(::max_align_t), FSourceLoc::Current());
    }

    void Free(void* Ptr) override {
        if (m_Backing && Ptr) m_Backing->Free(Ptr);
    }

private:
    FAllocator* m_Backing = nullptr;
};

// プロセス寿命のシングルトン（Diligent はアロケータを所有しないので、誰かが持つ必要がある）
AcsMemoryAllocator* g_adapter = nullptr;

} // namespace

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
