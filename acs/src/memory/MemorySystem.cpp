// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySystem 実装
// -----------------------------------------------------------------------------
// 各セグメントごとに FTlsfAllocator または FLinearAllocator を保持し、
// FMutex で保護する薄いファサード。
// 「現在のセグメント」は TLS 変数で管理し、FScopedMemorySegment で push/pop。
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

// 現在のスレッドが選択しているセグメント（FScopedMemorySegment で切替）
ACS_THREAD_LOCAL ESegment tls_current_segment = ESegment::Default;

namespace {

// 1 セグメント分のアロケータホルダ
struct SegmentSlot {
    ESegment         segment        = ESegment::Default;
    bool            use_linear     = false;
    bool            initialized    = false;
    FMutex           lock;            // TLSF 単一スレッド前提を保護
    FTlsfAllocator   tlsf;            // use_linear=false で使う
    FLinearAllocator* linear = nullptr; // use_linear=true で使う
    FVmReservation   reservation;
    u64             budget         = 0;
    u64             reserve_size   = 0;
    u64             committed_size = 0;
};

// SegmentSlot を FAllocator IF として公開するアダプタ
class SegmentAllocator final : public FAllocator {
public:
    SegmentAllocator() noexcept = default;
    void Bind(SegmentSlot* slot) noexcept { _slot = slot; }

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override {
        if (!_slot || !_slot->initialized) return nullptr;
        FScopedLock lk(_slot->lock);
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
        FScopedLock lk(_slot->lock);
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
struct FState {
    SegmentSlot      slots[(usize)ESegment::_Count];
    SegmentAllocator allocators[(usize)ESegment::_Count];
    bool             inited = false;
};
FState g_state;

} // namespace

// 既定設定（小規模テスト用、すぐ動かしたいとき向け）
FMemorySystemConfig FMemorySystem::DefaultConfig() noexcept {
    FMemorySystemConfig c {};
    auto setup = [](FSegmentConfig& s, ESegment seg, u64 reserve, u64 commit, bool lin) {
        s.segment         = seg;
        s.reserve_bytes   = reserve;
        s.commit_initial  = commit;
        s.budget_hard_cap = reserve;
        s.use_linear      = lin;
    };
    setup(c.segments[(usize)ESegment::Default],   ESegment::Default,   256ull << 20,  16ull << 20, false);
    setup(c.segments[(usize)ESegment::Permanent], ESegment::Permanent,  64ull << 20,  16ull << 20, false);
    setup(c.segments[(usize)ESegment::Temp],      ESegment::Temp,       32ull << 20,  32ull << 20, true);
    setup(c.segments[(usize)ESegment::Resource],  ESegment::Resource,  512ull << 20,  64ull << 20, false);
    setup(c.segments[(usize)ESegment::Develop],   ESegment::Develop,    64ull << 20,   8ull << 20, false);
    return c;
}

// 全セグメントを設定で初期化
TResult<void> FMemorySystem::Init(const FMemorySystemConfig& cfg) noexcept {
    if (g_state.inited) return ACS_ERR(Memory, 30, "FMemorySystem already initialized");

    for (usize i = 0; i < (usize)ESegment::_Count; ++i) {
        const FSegmentConfig& sc = cfg.segments[i];
        SegmentSlot& slot = g_state.slots[i];
        slot.segment = sc.segment;
        slot.use_linear = sc.use_linear;
        slot.budget = sc.budget_hard_cap;
        slot.reserve_size = sc.reserve_bytes;
        slot.committed_size = sc.commit_initial;

        if (sc.use_linear) {
            // Linear セグメントは VM 予約 + 全領域コミット + FLinearAllocator 個別生成
            auto rr = FVmReservation::Reserve(sc.reserve_bytes);
            if (rr.IsErr()) return Err<void>(rr.Error());
            slot.reservation = Move(rr.Value());
            auto cr = slot.reservation.Commit(0, sc.commit_initial);
            if (cr.IsErr()) return cr;
            slot.linear = static_cast<FLinearAllocator*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(FLinearAllocator)));
            if (!slot.linear) return ACS_ERR(Memory, 31, "Linear alloc failed");
            ::new (slot.linear) FLinearAllocator(sc.commit_initial);
        } else {
            // TLSF セグメントは VM 予約 + 初期コミットして TLSF プール登録
            auto rr = FVmReservation::Reserve(sc.reserve_bytes);
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

void FMemorySystem::Shutdown() noexcept {
    if (!g_state.inited) return;
    for (usize i = 0; i < (usize)ESegment::_Count; ++i) {
        SegmentSlot& slot = g_state.slots[i];
        if (!slot.initialized) continue;
        if (slot.use_linear && slot.linear) {
            slot.linear->~FLinearAllocator();
            ::HeapFree(::GetProcessHeap(), 0, slot.linear);
            slot.linear = nullptr;
        }
        slot.reservation.Release();
        slot.initialized = false;
    }
    g_state.inited = false;
}

FAllocator* FMemorySystem::Get(ESegment s) noexcept {
    if (!g_state.inited) return nullptr;
    return &g_state.allocators[(usize)s];
}

ESegment FMemorySystem::Current() noexcept {
    return tls_current_segment;
}

FAllocator* FMemorySystem::CurrentAllocator() noexcept {
    return Get(tls_current_segment);
}

// Temp セグメントを巻き戻す（フレーム先頭で 1 回呼ぶ）
void FMemorySystem::ResetTemp() noexcept {
    if (!g_state.inited) return;
    SegmentSlot& slot = g_state.slots[(usize)ESegment::Temp];
    if (slot.initialized && slot.use_linear && slot.linear) {
        FScopedLock lk(slot.lock);
        slot.linear->Reset();
    }
}

// 全セグメントの統計を out に詰める
u32 FMemorySystem::GetStats(FSegmentStats* out, u32 max_count) noexcept {
    if (!g_state.inited || !out || max_count == 0) return 0;
    u32 n = 0;
    for (usize i = 0; i < (usize)ESegment::_Count && n < max_count; ++i) {
        SegmentSlot& slot = g_state.slots[i];
        if (!slot.initialized) continue;
        FSegmentStats& s = out[n++];
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
// FScopedMemorySegment — RAII セグメント切替
// =============================================================================
// コンストラクタで TLS の現在セグメントを上書き、デストラクタで元に戻す
FScopedMemorySegment::FScopedMemorySegment(ESegment s) noexcept
    : _previous(tls_current_segment) {
    tls_current_segment = s;
}

FScopedMemorySegment::~FScopedMemorySegment() noexcept {
    tls_current_segment = _previous;
}

} // namespace acs
