// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySystem 実装
// -----------------------------------------------------------------------------
// 各セグメントごとに FTlsfAllocator または FLinearAllocator を保持し、
// FMutex で保護する薄いファサード。
// 「現在のセグメント」は TLS 変数で管理し、ScopedMemorySegment で push/pop。
// =============================================================================
#include "memory/MemorySystem.h"
#include "memory/ShardedTlsf.h"
#include "memory/ArenaAllocator.h"
#include "memory/SystemAllocator.h"
#include "memory/VirtualMemory.h"
#include "foundation/Platform.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"

namespace acs {

// 現在のスレッドが選択しているセグメント（ScopedMemorySegment で切替）
ACS_THREAD_LOCAL ESegment tls_current_segment = ESegment::Default;

namespace {

// frame(arena) アロケータのページ確保元。既定アロケータ差し替え (SetDefaultAllocator) の
// 影響を受けない独立した system allocator を使う (再帰を避ける)。
FSystemAllocator g_frame_backing;

// 1 セグメント分のアロケータホルダ。
//   ・use_frame=false: 汎用 = FShardedTlsfAllocator (per-thread シャードで MT スケール、self-lock)
//   ・use_frame=true : フレーム/スクラッチ = FArenaAllocator (lock-free bump、毎フレーム Reset)
struct SegmentSlot {
    ESegment              segment        = ESegment::Default;
    bool                  use_frame      = false;
    bool                  initialized    = false;
    FShardedTlsfAllocator sharded;            // use_frame=false
    FArenaAllocator*      arena = nullptr;    // use_frame=true (placement-new)
    u64                   budget         = 0;
    u64                   reserve_size   = 0;
    u64                   committed_size = 0;
};

// SegmentSlot を FAllocator IF として公開するアダプタ。
// sharded / arena はどちらも内部でスレッド安全 (前者=per-shard ロック、後者=lock-free CAS) なので、
// ここでロックは取らない。旧実装の「セグメントごと単一 SRWLOCK funnel」を撤廃し、MT 確保の
// ロック競合を解消する。予算チェックはロック無し (近似でよい)。
class SegmentAllocator final : public FAllocator {
public:
    SegmentAllocator() noexcept = default;
    void Bind(SegmentSlot* slot) noexcept { m_Slot = slot; }

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override {
        if (!m_Slot || !m_Slot->initialized) return nullptr;
        FAllocator* a = Backing();
        if (m_Slot->budget > 0 && a->BytesAllocated() + size > m_Slot->budget) return nullptr;
        return a->Alloc(size, alignment, loc);
    }
    void Free(void* ptr) noexcept override {
        if (!m_Slot || !m_Slot->initialized || !ptr) return;
        Backing()->Free(ptr);
    }
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc) noexcept override {
        if (!m_Slot || !m_Slot->initialized) return nullptr;
        return Backing()->Realloc(ptr, old_size, new_size, alignment, loc);  // sharded は in-place
    }
    u64 BytesAllocated() const noexcept override {
        return (m_Slot && m_Slot->initialized) ? Backing()->BytesAllocated() : 0;
    }
    u64 PeakBytes() const noexcept override {
        return (m_Slot && m_Slot->initialized) ? Backing()->PeakBytes() : 0;
    }
    const char* Name() const noexcept override {
        return m_Slot ? ToString(m_Slot->segment) : "Unbound";
    }
private:
    FAllocator* Backing() const noexcept {
        return m_Slot->use_frame ? static_cast<FAllocator*>(m_Slot->arena)
                                  : static_cast<FAllocator*>(&m_Slot->sharded);
    }
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

    // いずれかのセグメント初期化に失敗した場合、確保済みリソース (シャード VM 予約 / arena) が
    // リークしないよう、完全初期化済みスロット [0, done) をロールバックしてから err を返す。
    usize done = 0;
    TResult<void> fail = Ok();

    for (usize i = 0; i < (usize)ESegment::_Count; ++i) {
        const SegmentConfig& sc = cfg.segments[i];
        SegmentSlot& slot = g_state.slots[i];
        slot.segment        = sc.segment;
        slot.use_frame      = sc.use_linear;   // 設定の use_linear = フレーム/スクラッチ用途
        slot.budget         = sc.budget_hard_cap;
        slot.reserve_size   = sc.reserve_bytes;
        slot.committed_size = sc.commit_initial;

        if (sc.use_linear) {
            // フレーム/スクラッチ: lock-free bump (FArenaAllocator)。ページは独立 system
            // allocator から確保し (既定差し替えの影響を受けない)、ページサイズ 1MiB。
            slot.arena = static_cast<FArenaAllocator*>(
                ::HeapAlloc(::GetProcessHeap(), 0, sizeof(FArenaAllocator)));
            if (!slot.arena) { fail = ACS_ERR(Memory, 31, "arena alloc failed"); break; }
            ::new (slot.arena) FArenaAllocator(1u * 1024u * 1024u, &g_frame_backing);
        } else {
            // 汎用: per-thread シャード化 TLSF (VM 予約 + auto-grow + 安全ガード + in-place realloc)。
            auto ir = slot.sharded.Init(sc.reserve_bytes, sc.commit_initial, 0u);
            if (ir.IsErr()) { fail = ir; break; }
        }
        slot.initialized = true;
        g_state.allocators[i].Bind(&slot);
        done = i + 1;
    }

    if (fail.IsErr()) {
        for (usize j = 0; j < done; ++j) {
            SegmentSlot& slot = g_state.slots[j];
            if (slot.use_frame) {
                if (slot.arena) {
                    slot.arena->~FArenaAllocator();
                    ::HeapFree(::GetProcessHeap(), 0, slot.arena);
                    slot.arena = nullptr;
                }
            } else {
                slot.sharded.Shutdown();
            }
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
        if (slot.use_frame) {
            if (slot.arena) {
                slot.arena->~FArenaAllocator();
                ::HeapFree(::GetProcessHeap(), 0, slot.arena);
                slot.arena = nullptr;
            }
        } else {
            slot.sharded.Shutdown();   // 各シャードの VM 予約を解放
        }
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
    if (slot.initialized && slot.use_frame && slot.arena) {
        slot.arena->Reset(false);   // ページは保持して次フレームで再利用 (再確保なし)
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
        FAllocator* a = slot.use_frame ? static_cast<FAllocator*>(slot.arena)
                                       : static_cast<FAllocator*>(&slot.sharded);
        s.used      = a->BytesAllocated();
        s.peak      = a->PeakBytes();
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
