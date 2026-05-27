// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugOverlay 実装
//
// state holder のみ。実描画は呼出し側の責務。詳細は FDebugOverlay.h 参照。
#include "gameframework/DebugOverlay.h"

#include <cstring>   // strcmp

namespace acs::game {

// ============================================================================
// ライフサイクル
// ============================================================================

void FDebugOverlay::Init() noexcept {
    // 履歴 (60 frame) を事前確保しておく (毎 Tick の reallocation を回避)。
    // Reserve は capacity のみで size 不変なので、論理 size はそのまま 0 始まり。
    m_FpsHistory.Clear();
    m_FpsHistory.Reserve(kFpsHistoryCap);
    m_FpsIndex    = 0u;
    m_bFpsFilled   = false;
    m_CurrentFps  = 0.0f;
}

void FDebugOverlay::Tick(f32 dt) noexcept {
    // dt <= 0 (一時停止 / 負値) は履歴汚染を避けるため無視。
    if (dt <= 0.0f) return;

    const f32 fps = 1.0f / dt;
    m_CurrentFps = fps;

    // 循環バッファ書き込み。
    // まだ 60 個揃っていない間は PushBack で末尾追加、揃った後は index 位置に上書き。
    if (!m_bFpsFilled) {
        m_FpsHistory.PushBack(fps);
        ++m_FpsIndex;
        if (m_FpsIndex >= kFpsHistoryCap) {
            m_FpsIndex  = 0u;
            m_bFpsFilled = true;
        }
    } else {
        m_FpsHistory[m_FpsIndex] = fps;
        ++m_FpsIndex;
        if (m_FpsIndex >= kFpsHistoryCap) m_FpsIndex = 0u;
    }
}

void FDebugOverlay::Reset() noexcept {
    m_FpsHistory.Clear();
    m_FpsIndex    = 0u;
    m_bFpsFilled   = false;
    m_CurrentFps  = 0.0f;
    m_SceneName   = nullptr;
    m_Watches.Clear();
    // m_Visible は意図的に保持 (Reset で誤って非表示にしない)。
}

// ============================================================================
// フレームレート集計
// ============================================================================

f32 FDebugOverlay::AverageFps() const noexcept {
    const usize n = m_FpsHistory.Size();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += m_FpsHistory[i];
    return sum / static_cast<f32>(n);
}

f32 FDebugOverlay::MinFps() const noexcept {
    const usize n = m_FpsHistory.Size();
    if (n == 0u) return 0.0f;
    f32 m = m_FpsHistory[0];
    for (usize i = 1; i < n; ++i) {
        const f32 v = m_FpsHistory[i];
        if (v < m) m = v;
    }
    return m;
}

f32 FDebugOverlay::MaxFps() const noexcept {
    const usize n = m_FpsHistory.Size();
    if (n == 0u) return 0.0f;
    f32 m = m_FpsHistory[0];
    for (usize i = 1; i < n; ++i) {
        const f32 v = m_FpsHistory[i];
        if (v > m) m = v;
    }
    return m;
}

// ============================================================================
// Watch 管理 (label / value とも caller 所有、本クラスは複製しない)
// ============================================================================

void FDebugOverlay::AddWatch(const char* label, const char* value) noexcept {
    if (label == nullptr || value == nullptr) return;
    // 同名 label があれば value を差し替え (後勝ち)。
    for (usize i = 0; i < m_Watches.Size(); ++i) {
        const char* existing = m_Watches[i].label;
        if (existing == label || (existing != nullptr && ::strcmp(existing, label) == 0)) {
            m_Watches[i].value = value;
            return;
        }
    }
    Watch w;
    w.label = label;
    w.value = value;
    m_Watches.PushBack(w);
}

void FDebugOverlay::RemoveWatch(const char* label) noexcept {
    if (label == nullptr) return;
    for (usize i = 0; i < m_Watches.Size(); ++i) {
        const char* existing = m_Watches[i].label;
        if (existing == label || (existing != nullptr && ::strcmp(existing, label) == 0)) {
            // 順序は描画側責務として swap-remove で十分。
            m_Watches.RemoveAtSwap(i);
            return;
        }
    }
}

void FDebugOverlay::ClearWatches() noexcept {
    m_Watches.Clear();
}

u32 FDebugOverlay::WatchCount() const noexcept {
    return static_cast<u32>(m_Watches.Size());
}

const FDebugOverlay::Watch* FDebugOverlay::AllWatches(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Watches.Size());
    if (m_Watches.Size() == 0u) return nullptr;
    return m_Watches.Data();
}

} // namespace acs::game
