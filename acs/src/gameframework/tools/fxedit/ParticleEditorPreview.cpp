// SPDX-License-Identifier: Apache-2.0
// GameFramework — In-game ParticleEditor: Preview canvas + stats 実装
//
// 仕様は FParticleEditorPreview.h を参照。本ファイルでは:
//   ・preview emitter の作成 / 再作成 / 破棄
//   ・ImGui sub-window 描画 (Burst / stats / spawn pos / auto-emit / frame budget)
//   ・frame budget 60-frame ring
// を実装する。全 noexcept、STL 不使用 (`<cstdio>` の snprintf のみ表示用に許可)。
#include "gameframework/tools/fxedit/ParticleEditorPreview.h"

#include "gameframework/ParticleEffectSystem.h"

#include <imgui.h>

#include <cstdio>   // snprintf (ImGui への label 整形用)

namespace acs::game::fxedit {

namespace {

/**
 * preview emitter handle が有効かを返す。
 *
 * @details
 * system 側で stale 化していても Set 系 / Burst / Destroy は no-op で動く設計のため、
 * ここでは handle の IsValid のみで判断する。
 * @param h 判定する emitter handle。
 * @return handle が有効なら true。
 */
inline bool HasPreview(FEmitterHandle h) noexcept {
    return h.IsValid();
}

} // anonymous namespace

/** 60 frame 履歴を事前確保し、stats と handle をリセットする。 */
void FParticleEditorPreview::Init() noexcept {
    // 60 frame 履歴を事前確保 (毎 Tick の reallocation 回避)。
    m_FpsHistory.Clear();
    m_FpsHistory.Reserve(kFpsHistoryCap);
    m_FpsIndex  = 0u;
    m_bFpsFilled = false;

    m_PreviewHandle    = {};
    m_bHasDefSnapshot  = false;
    m_LastActiveCount = 0u;
    m_LastCapacity     = 0u;
    // m_SpawnPos / m_AutoEmit は呼び出し間で保持しても害がない (UX 連続性)。
}

/** preview handle と stats / 履歴をクリアする (emitter の Destroy は行わない)。 */
void FParticleEditorPreview::Shutdown() noexcept {
    // system を持っていないため preview emitter の Destroy は行わない。
    // 明示的に止めたい呼出し側は Shutdown 前に StopAll(system) を呼ぶこと。
    m_PreviewHandle    = {};
    m_bHasDefSnapshot  = false;
    m_FpsHistory.Clear();
    m_FpsIndex         = 0u;
    m_bFpsFilled        = false;
    m_LastActiveCount = 0u;
    m_LastCapacity     = 0u;
}

/**
 * stats (capacity / active 数) と frame budget ring を更新する。
 *
 * @details
 * dt <= 0 の場合は履歴汚染を避けるため fps を push しない。FParticleEffectSystem::Tick
 * は呼ばない (= 呼出し側の責務)。active count は system の getter から、capacity は
 * AllParticles の out_count 経由で取得する。
 * @param dt 前フレームからの経過秒。
 * @param system stats を取得する対象の particle system。
 */
void FParticleEditorPreview::Tick(f32 dt, FParticleEffectSystem& system) noexcept {
    // stats 更新 (capacity / active 数)。
    u32 cap = 0u;
    (void)system.AllParticles(cap);
    m_LastCapacity     = cap;
    m_LastActiveCount = system.ActiveParticleCount();

    // frame budget ring: dt <= 0 は無視 (一時停止 / 負値ガード)。
    if (dt <= 0.0f) return;
    const f32 fps = 1.0f / dt;
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

/**
 * preview の ImGui sub-window を描画する。
 *
 * @details
 * "Particle Preview" タイトルで Begin/End wrap し、stats・spawn pos・auto-emit・
 * action ボタン (Burst/Recreate/Stop All)・現在 def の read-only 表示を並べる。
 * def == nullptr のときは「未選択」メッセージのみ出す。spawn pos / auto-emit の
 * 変更は即座に system に反映する。
 * @param system 操作対象の particle system。
 * @param def 現在選択中の emitter def (nullptr なら未選択表示)。
 */
void FParticleEditorPreview::DrawUI(FParticleEffectSystem& system,
                                   const ParticleEmitterDef* def) noexcept {
    if (!ImGui::Begin("Particle Preview")) {
        ImGui::End();
        return;
    }

    if (def == nullptr) {
        ImGui::TextUnformatted("No emitter def selected.");
        ImGui::TextUnformatted("Open the ParticleEditor and pick a preset.");
        ImGui::End();
        return;
    }

    // Stats: active / capacity のフォーマット。snprintf は描画用 transient buffer。
    ImGui::Text("FParticles: %u / %u",
                static_cast<unsigned>(m_LastActiveCount),
                static_cast<unsigned>(m_LastCapacity));
    {
        // pool 使用率を progress bar として可視化 (0 capacity 時はゼロ進行)。
        const f32 ratio = (m_LastCapacity > 0u)
            ? (static_cast<f32>(m_LastActiveCount) /
               static_cast<f32>(m_LastCapacity))
            : 0.0f;
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", ratio * 100.0f);
        ImGui::ProgressBar(ratio, ImVec2(-1.0f, 0.0f), overlay);
    }
    ImGui::Text("Avg FPS (60f): %.1f", GraphFps());

    ImGui::Separator();

    // Spawn position editor: FVec2 のメモリレイアウトは {f32 x, f32 y} contiguous なので、
    // ImGui::DragFloat2 にそのままアドレスを渡せる。値が変わったら即時 system 側にも反映する。
    {
        f32 tmp[2] = { m_SpawnPos.x, m_SpawnPos.y };
        if (ImGui::DragFloat2("Spawn Pos", tmp, 1.0f)) {
            m_SpawnPos = FVec2{ tmp[0], tmp[1] };
            if (HasPreview(m_PreviewHandle)) {
                system.SetEmitterPosition(m_PreviewHandle, m_SpawnPos);
            }
        }
    }

    // Auto-emit toggle。
    {
        bool autoe = m_AutoEmit;
        if (ImGui::Checkbox("Auto Emit (use emit_rate)", &autoe)) {
            // SetAutoEmit を介して system 側にも反映 (active flag を切り替える)。
            SetAutoEmit(autoe);
            if (HasPreview(m_PreviewHandle)) {
                system.SetEmitterActive(m_PreviewHandle, m_AutoEmit);
            }
        }
    }

    ImGui::Separator();

    // Action buttons。
    if (ImGui::Button("Burst")) {
        TriggerBurst(system);
    }
    ImGui::SameLine();
    if (ImGui::Button("Recreate")) {
        // 編集中 def で preview emitter を作り直す (UI 値の即時反映用)。
        RecreatePreviewEmitter(system, def);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop All")) {
        StopAll(system);
    }

    ImGui::Separator();

    // Live def の主要パラメータ表示 (read-only)。編集 UI 本体は ParticleEditor 側にあるため、
    // ここでは「今 preview がどの値で動いているか」を確認できる程度の表示に留める。
    ImGui::TextUnformatted("Current def (read-only):");
    ImGui::BulletText("lifetime   : %.3f sec", def->lifetime_sec);
    ImGui::BulletText("emit_rate  : %.2f /sec", def->emit_rate_per_sec);
    ImGui::BulletText("burst_cnt  : %.0f",      def->burst_count);
    ImGui::BulletText("speed      : %.1f .. %.1f", def->speed_min, def->speed_max);
    ImGui::BulletText("scale      : %.2f -> %.2f", def->scale_start, def->scale_end);
    ImGui::BulletText("gravity    : (%.1f, %.1f)", def->gravity.x, def->gravity.y);

    ImGui::End();
}

/**
 * 編集中 def を内部 snapshot にコピーし、preview emitter を作り直す。
 *
 * @details
 * 既存 handle が valid なら一旦 Destroy する (既に出ている particle は寿命まで生かす)。
 * 新しい emitter を spawn pos で生成し、auto_emit が false ならその場で active を off にする。
 * def が nullptr なら no-op。
 * @param system emitter を生成する対象の particle system。
 * @param def 反映する emitter def (nullptr なら no-op)。
 */
void FParticleEditorPreview::RecreatePreviewEmitter(FParticleEffectSystem& system,
                                                   const ParticleEmitterDef* def) noexcept {
    if (def == nullptr) return;

    if (HasPreview(m_PreviewHandle)) {
        system.DestroyEmitter(m_PreviewHandle);
        m_PreviewHandle = {};
    }

    // def の copy を snapshot として保持 (caller 側 def が消えても再 Tick できる)。
    m_LastDef         = *def;
    m_bHasDefSnapshot = true;

    m_PreviewHandle = system.CreateEmitter(m_LastDef, m_SpawnPos);
    if (HasPreview(m_PreviewHandle)) {
        // auto_emit に応じて active 状態を反映 (CreateEmitter 直後は active=true)。
        system.SetEmitterActive(m_PreviewHandle, m_AutoEmit);
    }
}

/**
 * preview emitter に対し 1 回の burst を発火する。
 *
 * @details
 * handle が invalid のとき (= まだ Recreate していない) は no-op。FParticleEffectSystem::Burst
 * が handle 検証と burst_count の floor 切り捨てを行う。
 * @param system burst を発火する対象の particle system。
 */
void FParticleEditorPreview::TriggerBurst(FParticleEffectSystem& system) noexcept {
    if (!HasPreview(m_PreviewHandle)) return;
    system.Burst(m_PreviewHandle);
}

/**
 * preview emitter を破棄し、system の全 particle を消去する。
 *
 * @details
 * 編集セッションを綺麗な状態に戻す。pool 容量はそのまま維持され、handle は invalid 化
 * される (= 次回 Recreate で再生成が必要)。
 * @param system 全消去する対象の particle system。
 */
void FParticleEditorPreview::StopAll(FParticleEffectSystem& system) noexcept {
    if (HasPreview(m_PreviewHandle)) {
        system.DestroyEmitter(m_PreviewHandle);
        m_PreviewHandle = {};
    }
    system.ClearAll();
    m_LastActiveCount = 0u;
}

/**
 * 内部 spawn 位置を設定する。
 *
 * @details system reference を受け取らないため内部値だけ更新し、次の Tick / Recreate 時に反映される。
 * @param pos 新しい spawn 位置。
 */
void FParticleEditorPreview::SetSpawnPos(FVec2 pos) noexcept {
    m_SpawnPos = pos;
    // system reference は受け取らない API なので、内部値だけ更新。次の Tick /
    // Recreate 時に反映される。
}

/**
 * 内部 auto-emit フラグを設定する。
 *
 * @details system reference を受け取らないため内部値だけ更新し、Recreate 時か DrawUI 経由で反映される。
 * @param b auto-emit を有効にするなら true。
 */
void FParticleEditorPreview::SetAutoEmit(bool b) noexcept {
    m_AutoEmit = b;
    // system reference は受け取らない API。Recreate 時か DrawUI 経由で反映される。
}

/**
 * 履歴 ring 内の fps を平均して返す (60 frame moving average)。
 *
 * @details 履歴が空のときは 0。少数フレームでも有効分だけで平均する。
 * @return 平均 fps、履歴が空なら 0.0f。
 */
f32 FParticleEditorPreview::GraphFps() const noexcept {
    const usize n = m_FpsHistory.Size();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += m_FpsHistory[i];
    return sum / static_cast<f32>(n);
}

} // namespace acs::game::fxedit
