// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — MemorySystem 実装
// -----------------------------------------------------------------------------
// 各セグメントごとに TlsfAllocator または LinearAllocator を保持し、
// Mutex で保護する薄いファサード。
// 「現在のセグメント」は TLS 変数で管理し、ScopedMemorySegment で push/pop。
// =============================================================================
#include "memory/MemorySystem.h"
#include "memory/Tlsf.h"
#include "memory/LinearAllocator.h"
#include "memory/VirtualMemory.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "foundation/Platform.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"

namespace acs {

// 現在のスレッドが選択しているセグメント（ScopedMemorySegment で切替）
ACS_THREAD_LOCAL Segment tls_current_segment = Segment::Default;

namespace {

// 1 セグメント分のアロケータホルダ
struct SegmentSlot {
    Segment         segment        = Segment::Default;
    bool            use_linear     = false;
    bool            initialized    = false;
    Mutex           lock;            // TLSF 単一スレッド前提を保護
    TlsfAllocator   tlsf;            // use_linear=false で使う
    LinearAllocator* linear = nullptr; // use_linear=true で使う
    VmReservation   reservation;
    u64             budget         = 0;
    u64             reserve_size   = 0;
    u64             committed_size = 0;
};

// SegmentSlot を Allocator IF として公開するアダプタ
class SegmentAllocator final : public Allocator {
public:
    SegmentAllocator() noexcept = default;
    void Bind(SegmentSlot* slot) noexcept { _slot = slot; }

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override {
        if (!_slot || !_slot->initialized) return nullptr;
        ScopedLock lk(_slot->lock);
        // 予算超過なら確保失敗
        u64 cur = _slot->use_linear
                  ? _slot->linear->BytesAllocated()
                  : _slot->tlsf.BytesAllocated();
        if (_slot->budget > 0 && cur + size > _slot->budget) return nullptr;
        if (_slot->use_linear) return _slot->linear->Alloc(size, alignment, loc);
        return _slot->tlsf.Alloc(size, alignment, loc);
    }

    void Free(void* ptr) noexcept override {
        if (!_slot || !_slot->initialized || !ptr) return;
        ScopedLock lk(_slot->lock);
        if (_slot->use_linear) _slot->linear->Free(ptr);
        else                   _slot->tlsf.Free(ptr);
    }

    u64 BytesAllocated() const noexcept override {
        if (!_slot || !_slot->initialized) return 0;
        return _slot->use_linear ? _slot->linear->BytesAllocated()
                                 : _slot->tlsf.BytesAllocated();
    }
    u64 PeakBytes() const noexcept override {
        if (!_slot || !_slot->initialized) return 0;
        return _slot->use_linear ? _slot->linear->PeakBytes()
                                 : _slot->tlsf.PeakBytes();
    }
    const char* Name() const noexcept override {
        return _slot ? ToString(_slot->segment) : "Unbound";
    }
private:
    SegmentSlot* _slot = nullptr;
};

// プロセスに 1 つだけのグローバル状態
struct State {
    SegmentSlot      slots[(usize)Segment::_Count];
    SegmentAllocator allocators[(usize)Segment::_Count];
    bool             inited = false;
};
State g_state;

} // namespace

// 既定設定（小規模テスト用、すぐ動かしたいとき向け）
MemorySystemConfig MemorySystem::DefaultConfig() noexcept {
    MemorySystemConfig c {};
    auto setup = [](SegmentConfig& s, Segment seg, u64 reserve, u64 commit, bool lin) {
        s.segment         = seg;
        s.reserve_bytes   = reserve;
        s.commit_initial  = commit;
        s.budget_hard_cap = reserve;
        s.use_linear      = lin;
    };
    setup(c.segments[(usize)Segment::Default],   Segment::Default,   256ull << 20,  16ull << 20, false);
    setup(c.segments[(usize)Segment::Permanent], Segment::Permanent,  64ull << 20,  16ull << 20, false);
    setup(c.segments[(usize)Segment::Temp],      Segment::Temp,       32ull << 20,  32ull << 20, true);
    setup(c.segments[(usize)Segment::Resource],  Segment::Resource,  512ull << 20,  64ull << 20, false);
    setup(c.segments[(usize)Segment::Develop],   Segment::Develop,    64ull << 20,   8ull << 20, false);
    return c;
}

// 全セグメントを設定で初期化
Result<void> MemorySystem::Init(const MemorySystemConfig& cfg) noexcept {
    if (g_state.inited) return ACS_ERR(Memory, 30, "MemorySystem already initialized");

    for (usize i = 0; i < (usize)Segment::_Count; ++i) {
        const SegmentConfig& sc = cfg.segments[i];
        SegmentSlot& slot = g_state.slots[i];
        slot.segment = sc.segment;
        slot.use_linear = sc.use_linear;
        slot.budget = sc.budget_hard_cap;
        slot.reserve_size = sc.reserve_bytes;
        slot.committed_size = sc.commit_initial;

        if (sc.use_linear) {
            // Linear セグメントは VM 予約 + 全領域コミット + LinearAllocator 個別生成
            auto rr = VmReservation::Reserve(sc.reserve_bytes);
            if (rr.IsErr()) return Err<void>(rr.Error());
            slot.reservation = Move(rr.Value());
            auto cr = slot.reservation.Commit(0, sc.commit_initial);
            if (cr.IsErr()) return cr;
            slot.linear = static_cast<LinearAllocator*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(LinearAllocator)));
            if (!slot.linear) return ACS_ERR(Memory, 31, "Linear alloc failed");
            ::new (slot.linear) LinearAllocator(sc.commit_initial);
        } else {
            // TLSF セグメントは VM 予約 + 初期コミットして TLSF プール登録
            auto rr = VmReservation::Reserve(sc.reserve_bytes);
            if (rr.IsErr()) return Err<void>(rr.Error());
            auto ir = slot.tlsf.InitWithReservation(Move(rr.Value()), sc.commit_initial);
            if (ir.IsErr()) return ir;
        }
        slot.initialized = true;
        g_state.allocators[i].Bind(&slot);
    }
    g_state.inited = true;
    return Ok();
}

void MemorySystem::Shutdown() noexcept {
    if (!g_state.inited) return;
    for (usize i = 0; i < (usize)Segment::_Count; ++i) {
        SegmentSlot& slot = g_state.slots[i];
        if (!slot.initialized) continue;
        if (slot.use_linear && slot.linear) {
            slot.linear->~LinearAllocator();
            ::HeapFree(::GetProcessHeap(), 0, slot.linear);
            slot.linear = nullptr;
        }
        slot.reservation.Release();
        slot.initialized = false;
    }
    g_state.inited = false;
}

Allocator* MemorySystem::Get(Segment s) noexcept {
    if (!g_state.inited) return nullptr;
    return &g_state.allocators[(usize)s];
}

Segment MemorySystem::Current() noexcept {
    return tls_current_segment;
}

Allocator* MemorySystem::CurrentAllocator() noexcept {
    return Get(tls_current_segment);
}

// Temp セグメントを巻き戻す（フレーム先頭で 1 回呼ぶ）
void MemorySystem::ResetTemp() noexcept {
    if (!g_state.inited) return;
    SegmentSlot& slot = g_state.slots[(usize)Segment::Temp];
    if (slot.initialized && slot.use_linear && slot.linear) {
        ScopedLock lk(slot.lock);
        slot.linear->Reset();
    }
}

// 全セグメントの統計を out に詰める
u32 MemorySystem::GetStats(SegmentStats* out, u32 max_count) noexcept {
    if (!g_state.inited || !out || max_count == 0) return 0;
    u32 n = 0;
    for (usize i = 0; i < (usize)Segment::_Count && n < max_count; ++i) {
        SegmentSlot& slot = g_state.slots[i];
        if (!slot.initialized) continue;
        SegmentStats& s = out[n++];
        s.segment   = slot.segment;
        s.name      = ToString(slot.segment);
        s.reserve   = slot.reserve_size;
        s.committed = slot.committed_size;
        s.used      = slot.use_linear ? slot.linear->BytesAllocated() : slot.tlsf.BytesAllocated();
        s.peak      = slot.use_linear ? slot.linear->PeakBytes()      : slot.tlsf.PeakBytes();
        s.budget    = slot.budget;
    }
    return n;
}

// =============================================================================
// ScopedMemorySegment — RAII セグメント切替
// =============================================================================
// コンストラクタで TLS の現在セグメントを上書き、デストラクタで元に戻す
ScopedMemorySegment::ScopedMemorySegment(Segment s) noexcept
    : _previous(tls_current_segment) {
    tls_current_segment = s;
}

ScopedMemorySegment::~ScopedMemorySegment() noexcept {
    tls_current_segment = _previous;
}

} // namespace acs
