// SPDX-License-Identifier: Apache-2.0
// GameFramework — In-game ParticleEditor: Preview canvas + stats (Phase 19b)
//
// `gameframework/tools/fxedit/` 配下に置く in-game エディタ群の一部。Phase 19b の
// 並列エージェント分担で「preview canvas + stats」を切り出した小モジュール。
// ParticleEditor 本体 (curve editor / preset library 等) は別エージェントが担当する。
//
// 役割:
//   ・編集中の `ParticleEmitterDef` を 1 個だけ instance 化して、リアルタイムに
//     visual feedback を返すための **preview canvas**。
//   ・ImGui を使った sub-window で:
//       - Burst ボタン
//       - active particle count
//       - pool stats (active / capacity)
//       - spawn position editor
//       - auto-emit toggle
//       - frame budget (graph fps)
//     を表示する。
//   ・edit 中の def が変わったら `RecreatePreviewEmitter` で即時再生成する
//     (= 古い emitter を Destroy → 新 def で Create) ので、UI 上の値変更が
//     即座に preview に反映される。
//
// 設計選択 (FDebugOverlay と同 Pillar の延長線上):
//   ・**FParticleEffectSystem を所有しない**: preview はあくまで「外部の
//     FParticleEffectSystem 上に 1 個 emitter を立てる」スタイル。テスト時には
//     fake system を渡せるし、in-game ツール時には本番 system を共有できる。
//   ・**def の copy を内部保持**: 編集中 def を caller のポインタ経由でも、
//     内部 snapshot 経由でも参照できる。`RecreatePreviewEmitter(system, def)`
//     に渡された `def` を copy し、`CreateEmitter(copy, spawn_pos)` で新規 instance 化。
//   ・**frame budget 60-frame ring**: FDebugOverlay と同じ循環バッファ方式
//     (容量固定 / push 不可時は上書き)。`GraphFps()` は履歴の算術平均を返す。
//   ・**非コピー・非ムーブ**: 内部に `FEmitterHandle` (= system 内 slot を指す
//     handle) を保持するため、ムーブで複製されると DestroyEmitter のタイミングが
//     不明瞭になる。明示的に削除。
//   ・**全 noexcept**: ACS 規約。失敗は no-op で表現。
//   ・**STL 不使用**: `acs::TArray<f32>` で frame ring を保持。`<string>` 等は禁止。
//   ・**ImGui include 可**: tools/ 配下なので tooling 層に位置し、ImGui 依存は許可。
//
// 範囲外 (Phase 19c+ で):
//   ・curve editor (color/size の時間カーブ編集)
//   ・preset library (json/tdat 保存・読み込み)
//   ・GPU sprite preview (実際の FSpriteBatch 統合)
//   ・複数 emitter の同時 preview (現状 1 個固定)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/ParticleEffectSystem.h"   // EmitterHandle 型解決のため必要
#include "math/Vec.h"

namespace acs::game::fxedit {

// ---------------------------------------------------------------------------
// FParticleEditorPreview — 編集中 emitter の preview canvas + stats
// ---------------------------------------------------------------------------
class FParticleEditorPreview {
public:
    FParticleEditorPreview() noexcept = default;
    ~FParticleEditorPreview() noexcept = default;

    // 非コピー・非ムーブ: 内部 FEmitterHandle / 履歴バッファの所有権を曖昧にしない。
    FParticleEditorPreview(const FParticleEditorPreview&)            = delete;
    FParticleEditorPreview& operator=(const FParticleEditorPreview&) = delete;
    FParticleEditorPreview(FParticleEditorPreview&&)                 = delete;
    FParticleEditorPreview& operator=(FParticleEditorPreview&&)      = delete;

    // ----- ライフサイクル -----
    // frame budget ring を事前確保 (60 frame)。再 Init は履歴をクリア。
    // 内部 FEmitterHandle は無効化 (= 既存 preview は明示的に Recreate する必要あり)。
    void Init() noexcept;

    // 後片付け。`StopAll` 相当 + 履歴 / handle のクリアを行うが、引数で system を
    // 取らないため preview emitter の DestroyEmitter は **行わない**。
    // (system が先に死ぬ場合があるので、handle は invalid にするだけ。)
    // 明示的に preview を止めるには Shutdown 前に `StopAll(system)` を呼ぶこと。
    void Shutdown() noexcept;

    // ----- Tick / Draw -----
    // 1 フレーム進める。dt <= 0 は frame budget ring に push せず無視。
    // system 側の Tick は **呼び出さない** (= 呼出し側の責務、複数 system 構成を
    // 想定して preview は system のドライバではない)。
    // ここでは「auto-emit が true なら現状 def に従って system に Burst を出す
    // ことはしない」: auto-emit は emit_rate_per_sec ベースの連続放出で、これは
    // emitter active フラグの ON/OFF で system 側が勝手にやってくれる。
    void Tick(f32 dt, class FParticleEffectSystem& system) noexcept;

    // preview window を ImGui で描画。"Particle Preview" タイトルで Begin/End wrap。
    // def == nullptr の場合は "no def selected" 旨だけ表示する (defensive)。
    void DrawUI(class FParticleEffectSystem& system, const struct ParticleEmitterDef* def) noexcept;

    // ----- 編集中 def の同期 -----
    // 編集中 emitter を即時再生成: 既存 handle が valid なら DestroyEmitter 後、
    // 新 def の copy で CreateEmitter する。def == nullptr は no-op (handle 維持)。
    void RecreatePreviewEmitter(class FParticleEffectSystem& system,
                                const struct ParticleEmitterDef* def) noexcept;

    // burst_count に従い 1 回 Burst (handle が valid のときのみ)。
    void TriggerBurst(class FParticleEffectSystem& system) noexcept;

    // 現在の preview emitter を破棄 + system の particle pool 全消去。
    // (= 編集セッションを綺麗な状態に戻すボタン用)
    void StopAll(class FParticleEffectSystem& system) noexcept;

    // ----- spawn position (preview canvas 上の出生座標) -----
    FVec2 SpawnPos() const noexcept { return m_SpawnPos; }
    void SetSpawnPos(FVec2 pos) noexcept;

    // ----- stats アクセサ -----
    // 直近 Tick で記録した system の active particle 数。
    u32  ActiveParticleCount() const noexcept { return m_LastActiveCount; }
    // pool 容量 (= FParticleEffectSystem::AllParticles の out_count)。
    u32  MaxParticleCount()    const noexcept { return m_LastCapacity; }
    // 連続放出モード (true なら emitter は active 状態、false なら手動 Burst のみ)。
    bool IsAutoEmit() const noexcept { return m_AutoEmit; }
    void SetAutoEmit(bool b) noexcept;
    // frame budget 表示用: 直近 60 frame の平均 fps。履歴空なら 0。
    f32  GraphFps() const noexcept;

private:
    // frame budget ring buffer 容量。FDebugOverlay と揃える (60 frame)。
    static constexpr u32 kFpsHistoryCap = 60u;

    // 編集中 emitter (system 上の 1 instance を指す handle)。Init / Shutdown /
    // RecreatePreviewEmitter / StopAll で更新される。
    FEmitterHandle m_PreviewHandle {};

    // 現在 caller が編集中の def の snapshot (RecreatePreviewEmitter 時の copy 元)。
    // ヘッダ依存を最小化するためフル struct を持つが、ParticleEmitterDef は
    // POD 同等 (FVec2/FVec3/f32 のみ) なのでサイズ的にも問題なし。
    ParticleEmitterDef m_LastDef {};
    bool               m_bHasDefSnapshot = false;

    FVec2  m_SpawnPos = {320.0f, 240.0f};   // preview canvas のデフォルト中央
    bool  m_AutoEmit = true;               // 連続放出 ON
    u32   m_LastActiveCount = 0u;         // Tick で更新
    u32   m_LastCapacity     = 0u;         // Tick で更新 (= pool 容量)

    // 60 frame ring (FDebugOverlay と同パターン)
    TArray<f32> m_FpsHistory;
    u32        m_FpsIndex  = 0u;
    bool       m_bFpsFilled = false;
};

} // namespace acs::game::fxedit
