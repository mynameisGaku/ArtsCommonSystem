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

// ============================================================================
// 内部ユーティリティ
// ============================================================================
namespace {

// emitter 有効性チェック: handle を保持しているか + system 側で正しく生きているか
// は呼出し側では確認できないため、ここでは handle の IsValid のみで判断する。
// system 側で stale 化していた場合、Set*/Burst/Destroy は no-op として動く設計
// (FParticleEffectSystem の規約) なので安全。
inline bool HasPreview(FEmitterHandle h) noexcept {
    return h.IsValid();
}

} // anonymous namespace

// ============================================================================
// ライフサイクル
// ============================================================================

void FParticleEditorPreview::Init() noexcept {
    // 60 frame 履歴を事前確保 (毎 Tick の reallocation 回避)。
    _fps_history.Clear();
    _fps_history.Reserve(kFpsHistoryCap);
    _fps_index  = 0u;
    _fps_filled = false;

    _preview_handle    = {};
    _has_def_snapshot  = false;
    _last_active_count = 0u;
    _last_capacity     = 0u;
    // _spawn_pos / _auto_emit は呼び出し間で保持しても害がない (UX 連続性)。
}

void FParticleEditorPreview::Shutdown() noexcept {
    // system を持っていないため preview emitter の Destroy は行わない。
    // 明示的に止めたい呼出し側は Shutdown 前に StopAll(system) を呼ぶこと。
    _preview_handle    = {};
    _has_def_snapshot  = false;
    _fps_history.Clear();
    _fps_index         = 0u;
    _fps_filled        = false;
    _last_active_count = 0u;
    _last_capacity     = 0u;
}

// ============================================================================
// Tick — stats + frame budget の更新
// ============================================================================
//   ・dt <= 0 の場合は履歴汚染を避けるため push しない (FDebugOverlay と同規約)。
//   ・FParticleEffectSystem::Tick は呼ばない (= 呼出し側の責務)。
//   ・active particle count は system の getter から、capacity は AllParticles の
//     out_count 経由で取得する (現状の public API ではこちらが唯一の経路)。
// ============================================================================
void FParticleEditorPreview::Tick(f32 dt, FParticleEffectSystem& system) noexcept {
    // stats 更新 (capacity / active 数)。
    u32 cap = 0u;
    (void)system.AllParticles(cap);
    _last_capacity     = cap;
    _last_active_count = system.ActiveParticleCount();

    // frame budget ring: dt <= 0 は無視 (一時停止 / 負値ガード)。
    if (dt <= 0.0f) return;
    const f32 fps = 1.0f / dt;
    if (!_fps_filled) {
        _fps_history.PushBack(fps);
        ++_fps_index;
        if (_fps_index >= kFpsHistoryCap) {
            _fps_index  = 0u;
            _fps_filled = true;
        }
    } else {
        _fps_history[_fps_index] = fps;
        ++_fps_index;
        if (_fps_index >= kFpsHistoryCap) _fps_index = 0u;
    }
}

// ============================================================================
// DrawUI — ImGui sub-window
// ============================================================================
//   ・"Particle Preview" タイトルで Begin/End wrap。
//   ・def == nullptr のときは「未選択」メッセージのみ出す (defensive)。
//   ・Burst ボタン押下時は TriggerBurst (handle invalid なら no-op)。
//   ・spawn pos / auto-emit の変更は即座に system に反映する。
// ============================================================================
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

    // ---- Stats ----
    // active / capacity のフォーマット。snprintf は描画用 transient buffer。
    ImGui::Text("FParticles: %u / %u",
                static_cast<unsigned>(_last_active_count),
                static_cast<unsigned>(_last_capacity));
    {
        // pool 使用率を progress bar として可視化 (0 capacity 時はゼロ進行)。
        const f32 ratio = (_last_capacity > 0u)
            ? (static_cast<f32>(_last_active_count) /
               static_cast<f32>(_last_capacity))
            : 0.0f;
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", ratio * 100.0f);
        ImGui::ProgressBar(ratio, ImVec2(-1.0f, 0.0f), overlay);
    }
    ImGui::Text("Avg FPS (60f): %.1f", GraphFps());

    ImGui::Separator();

    // ---- Spawn position editor ----
    // FVec2 のメモリレイアウトは {f32 x, f32 y} contiguous なので、ImGui::DragFloat2
    // にそのままアドレスを渡せる。値が変わったら即時 system 側にも反映する。
    {
        f32 tmp[2] = { _spawn_pos.x, _spawn_pos.y };
        if (ImGui::DragFloat2("Spawn Pos", tmp, 1.0f)) {
            _spawn_pos = FVec2{ tmp[0], tmp[1] };
            if (HasPreview(_preview_handle)) {
                system.SetEmitterPosition(_preview_handle, _spawn_pos);
            }
        }
    }

    // ---- Auto-emit toggle ----
    {
        bool autoe = _auto_emit;
        if (ImGui::Checkbox("Auto Emit (use emit_rate)", &autoe)) {
            // SetAutoEmit を介して system 側にも反映 (active flag を切り替える)。
            SetAutoEmit(autoe);
            if (HasPreview(_preview_handle)) {
                system.SetEmitterActive(_preview_handle, _auto_emit);
            }
        }
    }

    ImGui::Separator();

    // ---- Action buttons ----
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

    // ---- Live def の主要パラメータ表示 (read-only) ----
    // 編集 UI 本体は別エージェント担当の ParticleEditor 側にあるため、ここでは
    // 「今 preview がどの値で動いているか」を確認できる程度の表示に留める。
    ImGui::TextUnformatted("Current def (read-only):");
    ImGui::BulletText("lifetime   : %.3f sec", def->lifetime_sec);
    ImGui::BulletText("emit_rate  : %.2f /sec", def->emit_rate_per_sec);
    ImGui::BulletText("burst_cnt  : %.0f",      def->burst_count);
    ImGui::BulletText("speed      : %.1f .. %.1f", def->speed_min, def->speed_max);
    ImGui::BulletText("scale      : %.2f -> %.2f", def->scale_start, def->scale_end);
    ImGui::BulletText("gravity    : (%.1f, %.1f)", def->gravity.x, def->gravity.y);

    ImGui::End();
}

// ============================================================================
// RecreatePreviewEmitter — 編集中 def を即時反映
// ============================================================================
//   ・既存 handle が valid なら一旦 Destroy (= 自動放出を止める)。
//     既に出ている particle は寿命まで生かす (= FParticleEffectSystem の規約)。
//   ・def を内部 snapshot にコピーし、新しい emitter を spawn_pos で生成。
//   ・auto_emit が false ならその場で active を off にする。
// ============================================================================
void FParticleEditorPreview::RecreatePreviewEmitter(FParticleEffectSystem& system,
                                                   const ParticleEmitterDef* def) noexcept {
    if (def == nullptr) return;

    if (HasPreview(_preview_handle)) {
        system.DestroyEmitter(_preview_handle);
        _preview_handle = {};
    }

    // def の copy を snapshot として保持 (caller 側 def が消えても再 Tick できる)。
    _last_def         = *def;
    _has_def_snapshot = true;

    _preview_handle = system.CreateEmitter(_last_def, _spawn_pos);
    if (HasPreview(_preview_handle)) {
        // auto_emit に応じて active 状態を反映 (CreateEmitter 直後は active=true)。
        system.SetEmitterActive(_preview_handle, _auto_emit);
    }
}

// ============================================================================
// TriggerBurst — Burst ボタン本体
// ============================================================================
//   ・handle が invalid のときは no-op (= まだ Recreate していないケース)。
//   ・FParticleEffectSystem::Burst は handle 検証 + burst_count に従い floor 切り捨て。
// ============================================================================
void FParticleEditorPreview::TriggerBurst(FParticleEffectSystem& system) noexcept {
    if (!HasPreview(_preview_handle)) return;
    system.Burst(_preview_handle);
}

// ============================================================================
// StopAll — preview emitter 破棄 + system 全消去
// ============================================================================
//   ・編集セッションを綺麗な状態に戻すボタン。pool 容量はそのまま維持される。
//   ・handle は invalid 化 (= 次回 Recreate で再生成必要)。
// ============================================================================
void FParticleEditorPreview::StopAll(FParticleEffectSystem& system) noexcept {
    if (HasPreview(_preview_handle)) {
        system.DestroyEmitter(_preview_handle);
        _preview_handle = {};
    }
    system.ClearAll();
    _last_active_count = 0u;
}

// ============================================================================
// SetSpawnPos / SetAutoEmit
// ============================================================================
// preview 描画中以外からの呼出しを想定したセッター。handle が valid なら
// system 側にも即時反映する (UI からの呼出しと API 呼出しで挙動を揃える)。
// (UI 経由の更新は DrawUI 内で同じことを行うが、二重反映でも no-op で安全。)
// ============================================================================
void FParticleEditorPreview::SetSpawnPos(FVec2 pos) noexcept {
    _spawn_pos = pos;
    // system reference は受け取らない API なので、内部値だけ更新。次の Tick /
    // Recreate 時に反映される。
}

void FParticleEditorPreview::SetAutoEmit(bool b) noexcept {
    _auto_emit = b;
    // system reference は受け取らない API。Recreate 時か DrawUI 経由で反映される。
}

// ============================================================================
// GraphFps — 60 frame moving average
// ============================================================================
//   ・履歴空のとき 0。少数フレームでも有効分だけで平均する (FDebugOverlay と同規約)。
// ============================================================================
f32 FParticleEditorPreview::GraphFps() const noexcept {
    const usize n = _fps_history.Size();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += _fps_history[i];
    return sum / static_cast<f32>(n);
}

} // namespace acs::game::fxedit
