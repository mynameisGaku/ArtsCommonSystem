// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySystem 実装
// -----------------------------------------------------------------------------
// 各セグメントごとに FTlsfAllocator または FLinearAllocator を保持し、
// FMutex で保護する薄いファサード。
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
    VmReservation   reservation;
    u64             budget         = 0;
    u64             reserve_size   = 0;
    u64             committed_size = 0;
};

// SegmentSlot を FAllocator IF として公開するアダプタ
class SegmentAllocator final : public FAllocator {
public:
    SegmentAllocator() noexcept = default;
    void Bind(SegmentSlot* slot) noexcept { m_Slot = slot; }

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override {
        if (!m_Slot || !m_Slot->initialized) return nullptr;
        FScopedLock lk(m_Slot->lock);
        // 予算超過なら確保失敗
        u64 cur = m_Slot->use_linear
                  ? m_Slot->linear->BytesAllocated()
                  : m_Slot->tlsf.BytesAllocated();
        if (m_Slot->budget > 0 && cur + size > m_Slot->budget) return nullptr;
        if (m_Slot->use_linear) return m_Slot->linear->Alloc(size, alignment, loc);
        return m_Slot->tlsf.Alloc(size, alignment, loc);
    }

    void Free(void* ptr) noexcept override {
        if (!m_Slot || !m_Slot->initialized || !ptr) return;
        FScopedLock lk(m_Slot->lock);
        if (m_Slot->use_linear) m_Slot->linear->Free(ptr);
        else                   m_Slot->tlsf.Free(ptr);
    }

    u64 BytesAllocated() const noexcept override {
        if (!m_Slot || !m_Slot->initialized) return 0;
        return m_Slot->use_linear ? m_Slot->linear->BytesAllocated()
                                 : m_Slot->tlsf.BytesAllocated();
    }
    u64 PeakBytes() const noexcept override {
        if (!m_Slot || !m_Slot->initialized) return 0;
        return m_Slot->use_linear ? m_Slot->linear->PeakBytes()
                                 : m_Slot->tlsf.PeakBytes();
    }
    const char* Name() const noexcept override {
        return m_Slot ? ToString(m_Slot->segment) : "Unbound";
    }
private:
    SegmentSlot* m_Slot = nullptr;
};

// プロセスに 1 つだけのグローバル状態
struct State {
    SegmentSlot      slots[(usize)ESegment::_Count];
    SegmentAllocator allocators[(usize)ESegment::_Count];
    bool             inited = false;
};
State g_state;

} // namespace

// 既定設定（小規模テスト用、すぐ動かしたいとき向け）
MemorySystemConfig FMemorySystem::DefaultConfig() noexcept {
    MemorySystemConfig c {};
    auto setup = [](SegmentConfig& s, ESegment seg, u64 reserve, u64 commit, bool lin) {
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
TResult<void> FMemorySystem::Init(const MemorySystemConfig& cfg) noexcept {
    if (g_state.inited) return ACS_ERR(Memory, 30, "FMemorySystem already initialized");

    // いずれかのセグメント初期化に失敗した場合、既に確保した VM 予約 / FLinearAllocator が
    // リークしないよう、完全初期化済みスロット [0, done) をロールバックしてから err を返す。
    usize done = 0;
    TResult<void> fail = Ok();

    for (usize i = 0; i < (usize)ESegment::_Count; ++i) {
        const SegmentConfig& sc = cfg.segments[i];
        SegmentSlot& slot = g_state.slots[i];
        slot.segment = sc.segment;
        slot.use_linear = sc.use_linear;
        slot.budget = sc.budget_hard_cap;
        slot.reserve_size = sc.reserve_bytes;
        slot.committed_size = sc.commit_initial;

        if (sc.use_linear) {
            // Linear セグメントは VM 予約 + 全領域コミット + FLinearAllocator 個別生成
            auto rr = VmReservation::Reserve(sc.reserve_bytes);
            if (rr.IsErr()) { fail = Err<void>(rr.Error()); break; }
            slot.reservation = Move(rr.Value());
            auto cr = slot.reservation.Commit(0, sc.commit_initial);
            if (cr.IsErr()) { slot.reservation.Release(); fail = cr; break; }
            slot.linear = static_cast<FLinearAllocator*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(FLinearAllocator)));
            if (!slot.linear) { slot.reservation.Release(); fail = ACS_ERR(Memory, 31, "Linear alloc failed"); break; }
            ::new (slot.linear) FLinearAllocator(sc.commit_initial);
        } else {
            // TLSF セグメントは VM 予約 + 初期コミットして TLSF プール登録
            // (失敗時は InitWithReservation 内のローカル reservation が RAII で解放される)
            auto rr = VmReservation::Reserve(sc.reserve_bytes);
            if (rr.IsErr()) { fail = Err<void>(rr.Error()); break; }
            auto ir = slot.tlsf.InitWithReservation(Move(rr.Value()), sc.commit_initial);
            if (ir.IsErr()) { fail = ir; break; }
        }
        slot.initialized = true;
        g_state.allocators[i].Bind(&slot);
        done = i + 1;
    }

    if (fail.IsErr()) {
        for (usize j = 0; j < done; ++j) {
            SegmentSlot& slot = g_state.slots[j];
            if (slot.use_linear && slot.linear) {
                slot.linear->~FLinearAllocator();
                ::HeapFree(::GetProcessHeap(), 0, slot.linear);
                slot.linear = nullptr;
            }
            slot.reservation.Release();
            slot.initialized = false;
        }
        return fail;
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
u32 FMemorySystem::GetStats(SegmentStats* out, u32 max_count) noexcept {
    if (!g_state.inited || !out || max_count == 0) return 0;
    u32 n = 0;
    for (usize i = 0; i < (usize)ESegment::_Count && n < max_count; ++i) {
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
ScopedMemorySegment::ScopedMemorySegment(ESegment s) noexcept
    : m_Previous(tls_current_segment) {
    tls_current_segment = s;
}

ScopedMemorySegment::~ScopedMemorySegment() noexcept {
    tls_current_segment = m_Previous;
}

} // namespace acs
