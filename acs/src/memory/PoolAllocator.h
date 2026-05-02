// ACS Memory — Fixed-size block pool allocator (Treiber-stack free list).
//
// Thread-safe: free list is a lock-free Treiber stack with an ABA tag stored
// in the upper 16 bits of the pointer (Windows x64 user-mode addresses use
// only 47 bits, so the upper bits are free for tagging).
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class PoolAllocator final : public Allocator {
public:
    // `block_size` is the minimum payload bytes per allocation; we round up
    // to alignof(void*) to embed the free-list pointer.
    PoolAllocator(usize block_size, usize block_count,
                  usize alignment = kDefaultAlignment,
                  Allocator* backing = nullptr) noexcept;
    ~PoolAllocator() noexcept override;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr)                                  noexcept override;

    u64 BlockSize()      const noexcept { return _block_size; }
    u64 BlockCount()     const noexcept { return _block_count; }
    u64 BytesAllocated() const noexcept override {
        return _live.Load(MemoryOrder::Acquire) * _block_size;
    }
    const char* Name()   const noexcept override { return "Pool"; }

private:
    struct Node {
        Node* next;
    };

    // Tagged pointer for ABA-safe Treiber stack.
    struct alignas(16) TaggedPtr {
        Node* ptr;
        u64   tag;
    };

    u8*               _storage    = nullptr;
    u64               _block_size = 0;
    u64               _block_count= 0;
    u64               _alignment  = 0;
    Allocator*        _backing    = nullptr;
    Atomic<u64>       _live {0};
    Atomic<u64>       _head_packed {0}; // packs ptr[47..0] | tag[63..48]
};

} // namespace acs
