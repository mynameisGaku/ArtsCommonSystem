// SPDX-License-Identifier: Apache-2.0
// GameFramework — In-game ParticleEditor: Preview canvas + stats
//
// `gameframework/tools/fxedit/` 配下に置く in-game エディタ群の一部。
// 「preview canvas + stats」を切り出した小モジュール。
// ParticleEditor 本体 (curve editor / preset library 等) は別モジュールが担当する。
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
// 範囲外:
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

/**
 * 編集中 emitter の preview canvas + stats を提供する in-game ツール。
 *
 * @details
 * 外部の FParticleEffectSystem 上に preview emitter を 1 個立て、ImGui sub-window で
 * Burst ボタン・active particle 数・pool 使用率・spawn 座標・auto-emit toggle・
 * frame budget (平均 fps) を表示する。編集中の ParticleEmitterDef が変わったら
 * RecreatePreviewEmitter で即時再生成し、UI 上の値変更を preview に反映する。
 * FParticleEffectSystem は所有せず、内部に FEmitterHandle と def snapshot、
 * 60-frame の fps 履歴 ring を保持する non-copy / non-move 型。
 */
class FParticleEditorPreview {
public:
    /** 空状態で構築する (リソースは Init で確保)。 */
    FParticleEditorPreview() noexcept = default;

    /** 破棄する (preview emitter の Destroy は行わない。Shutdown 参照)。 */
    ~FParticleEditorPreview() noexcept = default;

    /** コピー禁止 (FEmitterHandle / 履歴バッファの所有権を曖昧にしないため)。 */
    FParticleEditorPreview(const FParticleEditorPreview&)            = delete;

    /** コピー代入も禁止。 */
    FParticleEditorPreview& operator=(const FParticleEditorPreview&) = delete;

    /** ムーブ禁止 (DestroyEmitter のタイミングを明確に保つため)。 */
    FParticleEditorPreview(FParticleEditorPreview&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FParticleEditorPreview& operator=(FParticleEditorPreview&&)      = delete;

    /**
     * frame budget ring (60 frame) を事前確保し、内部状態を初期化する。
     *
     * @details
     * 再 Init は履歴・stats・handle をクリアする (既存 preview は明示的に Recreate
     * し直す必要がある)。m_SpawnPos / m_AutoEmit は UX 連続性のため保持される。
     */
    void Init() noexcept;

    /**
     * 履歴・handle・stats をクリアして後片付けする。
     *
     * @details
     * 引数で system を取らないため preview emitter の DestroyEmitter は行わず、
     * handle を invalid にするだけ (system が先に死ぬケースに備える)。明示的に
     * preview を止めるには Shutdown 前に StopAll(system) を呼ぶこと。
     */
    void Shutdown() noexcept;

    /**
     * 1 フレーム進めて stats と frame budget を更新する。
     *
     * @details
     * system から active particle 数と pool 容量を取得して保持する。dt <= 0 は
     * frame budget ring に push せず無視する。system 側の Tick は呼ばない
     * (= 呼出し側の責務)。auto-emit は emit_rate ベースの連続放出で system 側が
     * 行うため、ここで Burst は出さない。
     * @param dt 前フレームからの経過秒。
     * @param system stats 取得元のパーティクルシステム。
     */
    void Tick(f32 dt, class FParticleEffectSystem& system) noexcept;

    /**
     * preview window を ImGui で描画する。
     *
     * @details
     * "Particle Preview" タイトルで Begin/End wrap する。def == nullptr の場合は
     * 「未選択」メッセージのみ表示する (defensive)。spawn pos / auto-emit の変更や
     * Burst / Recreate / Stop All ボタンは即座に system に反映する。
     * @param system 操作対象のパーティクルシステム。
     * @param def 表示・再生成に使う編集中の emitter 定義 (nullptr 可)。
     */
    void DrawUI(class FParticleEffectSystem& system, const struct ParticleEmitterDef* def) noexcept;

    /**
     * 編集中 def で preview emitter を即時再生成する。
     *
     * @details
     * 既存 handle が valid なら DestroyEmitter してから、def の copy を内部 snapshot に
     * 保持し、spawn_pos で CreateEmitter する。生成後は m_AutoEmit に応じて active 状態を
     * 反映する。def == nullptr は no-op (handle 維持)。
     * @param system emitter を生成するパーティクルシステム。
     * @param def 再生成元の emitter 定義 (nullptr なら no-op)。
     */
    void RecreatePreviewEmitter(class FParticleEffectSystem& system,
                                const struct ParticleEmitterDef* def) noexcept;

    /**
     * preview emitter に 1 回 Burst を出す。
     *
     * @details handle が invalid のときは no-op (まだ Recreate していないケース)。
     * @param system Burst を発行するパーティクルシステム。
     */
    void TriggerBurst(class FParticleEffectSystem& system) noexcept;

    /**
     * preview emitter を破棄し、system の particle pool を全消去する。
     *
     * @details
     * 編集セッションを綺麗な状態に戻すボタン用。pool 容量は維持され、handle は
     * invalid 化される (次回 Recreate で再生成が必要)。
     * @param system 破棄・全消去の対象パーティクルシステム。
     */
    void StopAll(class FParticleEffectSystem& system) noexcept;

    /**
     * 現在の spawn 座標 (preview canvas 上の出生座標) を返す。
     *
     * @return spawn 座標。
     */
    FVec2 SpawnPos() const noexcept { return m_SpawnPos; }

    /**
     * spawn 座標を設定する。
     *
     * @details
     * 内部値のみ更新する (system 参照を取らない API のため即時反映はしない)。
     * 次の Tick / Recreate 時、または DrawUI 経由の更新で system に反映される。
     * @param pos 新しい spawn 座標。
     */
    void SetSpawnPos(FVec2 pos) noexcept;

    /**
     * 直近 Tick で記録した active particle 数を返す。
     *
     * @return active particle 数。
     */
    u32  ActiveParticleCount() const noexcept { return m_LastActiveCount; }

    /**
     * pool 容量を返す。
     *
     * @return pool 容量 (= FParticleEffectSystem::AllParticles の out_count)。
     */
    u32  MaxParticleCount()    const noexcept { return m_LastCapacity; }

    /**
     * auto-emit (連続放出モード) が有効かを返す。
     *
     * @return 有効なら true (emitter active)、false なら手動 Burst のみ。
     */
    bool IsAutoEmit() const noexcept { return m_AutoEmit; }

    /**
     * auto-emit フラグを設定する。
     *
     * @details
     * 内部値のみ更新する (system 参照を取らない API)。Recreate 時か DrawUI 経由で
     * emitter の active 状態に反映される。
     * @param b true で連続放出、false で手動 Burst のみ。
     */
    void SetAutoEmit(bool b) noexcept;

    /**
     * frame budget 表示用に直近 60 frame の平均 fps を返す。
     *
     * @return 履歴の算術平均 fps (履歴が空なら 0)。
     */
    f32  GraphFps() const noexcept;

private:
    /** frame budget ring buffer の容量 (FDebugOverlay と揃える 60 frame)。 */
    static constexpr u32 kFpsHistoryCap = 60u;

    /** 編集中 emitter (system 上の 1 instance) を指す handle。 */
    FEmitterHandle m_PreviewHandle {};

    /** 編集中 def の snapshot (RecreatePreviewEmitter 時の copy 元)。 */
    ParticleEmitterDef m_LastDef {};

    /** def snapshot を保持済みかどうか。 */
    bool               m_bHasDefSnapshot = false;

    /** spawn 座標 (preview canvas のデフォルト中央)。 */
    FVec2  m_SpawnPos = {320.0f, 240.0f};

    /** auto-emit (連続放出) フラグ (既定 ON)。 */
    bool  m_AutoEmit = true;

    /** 直近 Tick で記録した active particle 数。 */
    u32   m_LastActiveCount = 0u;

    /** 直近 Tick で記録した pool 容量。 */
    u32   m_LastCapacity     = 0u;

    /** 直近 60 frame の fps 履歴 ring (FDebugOverlay と同パターン)。 */
    TArray<f32> m_FpsHistory;

    /** fps 履歴 ring の書き込み位置。 */
    u32        m_FpsIndex  = 0u;

    /** fps 履歴 ring が一巡して満杯になったかどうか。 */
    bool       m_bFpsFilled = false;
};

} // namespace acs::game::fxedit
