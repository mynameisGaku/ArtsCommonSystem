#include "memory/PoolAllocator.h"
#include "memory/Memory.h"
#include "foundation/Assert.h"

namespace acs {

namespace {
// 47-bit user-mode pointer + 17-bit ABA tag packed in 64 bits.
constexpr u64 kPtrMask = (1ull << 47) - 1ull;
constexpr u64 kTagMask = ~kPtrMask;
constexpr u32 kTagShift = 47;

ACS_FORCEINLINE u64 Pack(PoolAllocator* /*self*/, void* p, u64 tag) noexcept {
    return (reinterpret_cast<u64>(p) & kPtrMask) | ((tag & ((1ull << 17) - 1)) << kTagShift);
}
ACS_FORCEINLINE void* UnpackPtr(u64 v) noexcept {
    return reinterpret_cast<void*>(v & kPtrMask);
}
ACS_FORCEINLINE u64 UnpackTag(u64 v) noexcept {
    return (v & kTagMask) >> kTagShift;
}
} // namespace

PoolAllocator::PoolAllocator(usize block_size, usize block_count,
                             usize alignment, Allocator* backing) noexcept
    : _block_size(static_cast<u64>(block_size))
    , _block_count(static_cast<u64>(block_count))
    , _alignment(static_cast<u64>(alignment))
    , _backing(backing ? backing : &DefaultAllocator()) {

    if (_alignment < sizeof(void*)) _alignment = sizeof(void*);
    if (_block_size < sizeof(Node)) _block_size = sizeof(Node);
    _block_size = AlignUp(_block_size, _alignment);

    usize total = static_cast<usize>(_block_size * _block_count);
    _storage = static_cast<u8*>(_backing->Alloc(total, _alignment, SourceLoc::Current()));
    if (!_storage) {
        _block_count = 0;
        return;
    }

    // Initialize free list — chain all blocks together (single threaded init).
    Node* prev = nullptr;
    for (u64 i = 0; i < _block_count; ++i) {
        Node* n = reinterpret_cast<Node*>(_storage + i * _block_size);
        n->next = prev;
        prev = n;
    }
    u64 packed = Pack(this, prev, 0);
    _head_packed.Store(packed, MemoryOrder::Release);
}

PoolAllocator::~PoolAllocator() noexcept {
    if (_storage) _backing->Free(_storage);
}

void* PoolAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    if (size > _block_size) return nullptr;
    if (alignment > _alignment) return nullptr;

    while (true) {
        u64 head = _head_packed.Load(MemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        if (!top) return nullptr; // exhausted
        u64 tag = UnpackTag(head);
        // Read top->next BEFORE attempting CAS — but top might be freed by
        // another thread mid-read. ABA tag ensures CAS detects the change.
        Node* next = top->next;
        u64 desired = Pack(this, next, tag + 1);
        u64 expected = head;
        if (_head_packed.CompareExchange(expected, desired)) {
            _live.FetchAdd(1);
            return top;
        }
    }
}

void PoolAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    Node* n = static_cast<Node*>(ptr);
    while (true) {
        u64 head = _head_packed.Load(MemoryOrder::Acquire);
        Node* top = static_cast<Node*>(UnpackPtr(head));
        u64 tag = UnpackTag(head);
        n->next = top;
        u64 desired = Pack(this, n, tag + 1);
        u64 expected = head;
        if (_head_packed.CompareExchange(expected, desired)) {
            _live.FetchSub(1);
            return;
        }
    }
}

} // namespace acs
