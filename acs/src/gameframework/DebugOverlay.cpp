// SPDX-License-Identifier: Apache-2.0
// FPS履歴、scene名、watch値と表示状態を保持・更新する。
// 描画は呼出し側が読み取った状態を使って行う。
#include "gameframework/DebugOverlay.h"

#include <cstring>   // strcmp

namespace acs::game {

/** 60 frame 履歴ぶんの capacity を確保し、カウンタを 0 始まりに戻す。 */
void CDebugOverlay::Init() noexcept {
    // 履歴 (60 frame) を事前確保しておく (毎 Tick の reallocation を回避)。
    // Reserve は capacity のみで size 不変なので、論理 size はそのまま 0 始まり。
    m_FpsHistory.Reset();
    m_FpsHistory.Reserve(kFpsHistoryCap);
    m_FpsIndex    = 0u;
    m_bFpsFilled   = false;
    m_CurrentFps  = 0.0f;
}

/** dt から fps を算出し、循環バッファに書き込む (dt <= 0 は無視)。 */
void CDebugOverlay::Tick(f32 dt) noexcept {
    // dt <= 0 (一時停止 / 負値) は履歴汚染を避けるため無視。
    if (dt <= 0.0f) return;

    const f32 fps = 1.0f / dt;
    m_CurrentFps = fps;

    // 循環バッファ書き込み。
    // まだ 60 個揃っていない間は Add で末尾追加、揃った後は index 位置に上書き。
    if (!m_bFpsFilled) {
        m_FpsHistory.Add(fps);
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

/** 履歴 / watches / scene name をクリアする (m_Visible は保持)。 */
void CDebugOverlay::Reset() noexcept {
    m_FpsHistory.Reset();
    m_FpsIndex    = 0u;
    m_bFpsFilled   = false;
    m_CurrentFps  = 0.0f;
    m_SceneName   = nullptr;
    m_Watches.Reset();
    // m_Visible は意図的に保持 (Reset で誤って非表示にしない)。
}

/** 履歴中の fps の算術平均を返す (履歴空時は 0)。 */
f32 CDebugOverlay::AverageFps() const noexcept {
    const usize n = m_FpsHistory.Num();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += m_FpsHistory[i];
    return sum / static_cast<f32>(n);
}

/** 履歴を線形走査して最小 fps を返す (履歴空時は 0)。 */
f32 CDebugOverlay::MinFps() const noexcept {
    const usize n = m_FpsHistory.Num();
    if (n == 0u) return 0.0f;
    f32 m = m_FpsHistory[0];
    for (usize i = 1; i < n; ++i) {
        const f32 v = m_FpsHistory[i];
        if (v < m) m = v;
    }
    return m;
}

/** 履歴を線形走査して最大 fps を返す (履歴空時は 0)。 */
f32 CDebugOverlay::MaxFps() const noexcept {
    const usize n = m_FpsHistory.Num();
    if (n == 0u) return 0.0f;
    f32 m = m_FpsHistory[0];
    for (usize i = 1; i < n; ++i) {
        const f32 v = m_FpsHistory[i];
        if (v > m) m = v;
    }
    return m;
}

/** 同名 label があれば value を差し替え、無ければ新規 watch を追加する。 */
void CDebugOverlay::AddWatch(const char* label, const char* value) noexcept {
    if (label == nullptr || value == nullptr) return;
    // 同名 label があれば value を差し替え (後勝ち)。
    for (usize i = 0; i < m_Watches.Num(); ++i) {
        const char* existing = m_Watches[i].label;
        if (existing == label || (existing != nullptr && ::strcmp(existing, label) == 0)) {
            m_Watches[i].value = value;
            return;
        }
    }
    FWatch w;
    w.label = label;
    w.value = value;
    m_Watches.Add(w);
}

/** label 一致する watch を swap-remove する (順序非保持)。 */
void CDebugOverlay::RemoveWatch(const char* label) noexcept {
    if (label == nullptr) return;
    for (usize i = 0; i < m_Watches.Num(); ++i) {
        const char* existing = m_Watches[i].label;
        if (existing == label || (existing != nullptr && ::strcmp(existing, label) == 0)) {
            // 順序は描画側責務として swap-remove で十分。
            m_Watches.RemoveAtSwap(i);
            return;
        }
    }
}

/** 全 watch を削除する。 */
void CDebugOverlay::ClearWatches() noexcept {
    m_Watches.Reset();
}

/** 登録済み watch 数を返す。 */
u32 CDebugOverlay::WatchCount() const noexcept {
    return static_cast<u32>(m_Watches.Num());
}

/** watch 配列の先頭ポインタと件数を返す (空なら nullptr)。 */
const CDebugOverlay::FWatch* CDebugOverlay::AllWatches(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Watches.Num());
    if (m_Watches.Num() == 0u) return nullptr;
    return m_Watches.GetData();
}

} // namespace acs::game
