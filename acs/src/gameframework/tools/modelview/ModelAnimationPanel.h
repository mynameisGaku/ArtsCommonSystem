// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / AModelAnimationPanel
//
// ModelViewer 内で、ロード済みモデルに含まれる **animation clip の再生制御 +
// timeline 表示** を行う panel。共通基盤の `editor_core::AEditorPanel`
// を継承し、ImGui ベースの control 群 (clip dropdown / Play / Pause / Stop /
// time slider / Loop checkbox / Speed slider / BlendWeight slider) を提供する。
//
// 役割分担 (ModelViewer 全体):
//   ・AModelViewerPanel    : 3D viewport + Lighting / Background / Grid / FBone toggle
//   ・AModelInspectorPanel : mesh 統計 (vertex/index/material/bone count)
//   ・AModelMaterialPanel  : material slot 編集 (basecolor / metallic / roughness)
//   ・AModelAnimationPanel : 本 panel — animation clip の再生制御 + timeline
//
// 役割:
//   ・ロード済みモデルが持つ animation clip 一覧を保持 (`SetClips`)
//   ・clip 切替 (dropdown) + Play / Pause / Stop + Loop toggle + Speed slider
//     + Time slider (現フレームの時刻 [0, duration]) + Blend weight slider
//   ・`Tick(dt)` で Playing 状態の場合 `m_CurrentTime += dt * m_Speed` を進める
//   ・duration 到達時、looping ならラップ、そうでなければ Stopped に遷移
//   ・毎 Tick 終端で `m_OnFrameCb(user, clip_index, time_sec)` を発火し、
//     呼出側 (ModelViewportRenderer 等) が CAnimationPlayer に時刻を反映する
//
// 役割分担 (本 panel と外部):
//   ・本 panel は **時刻の進行 + clip 選択 + UI control** だけを持つ。
//     実際の bone palette 計算 / GPU 反映 / mesh 描画は呼出側 (renderer +
//     CAnimationPlayer) に委譲する。`OnFrameCallback` がその橋渡し点。
//   ・clip メタ情報 (name / duration / looping / clip_index) は外部 (model
//     loader) が確定済みの値を `SetClips` で push する。本 panel は中身を
//     コピー所有 (= FAnimationClipBinding 配列を TArray<>); pointer は持たない。
//
// 使い方 (典型):
//   AModelAnimationPanel anim;
//   anim.Init();
//   workspace.RegisterPanel(&anim);
//
//   // モデル load 時:
//   FAnimationClipBinding clips[] = {
//       { "Idle",  2.0f, true,  0 },
//       { "Walk",  1.2f, true,  1 },
//       { "Jump",  0.8f, false, 2 },
//   };
//   anim.SetClips(clips, 3);
//   anim.SelectClip(0);     // Idle を選択
//   anim.Play();
//
//   // 毎フレーム:
//   anim.Tick(dt);          // m_CurrentTime += dt * speed (Playing 中のみ)
//   // → callback 内で CAnimationPlayer::SetTime(clip_index, time_sec) を呼ぶ
//
// 設計選択 (AModelAnimationPanel):
//   ・**AEditorPanel 継承**: Title / DrawUI を override。
//     OnInit は基底実装 (`Workspace()` ポインタ保持) のみで十分 (= 本 panel は
//     workspace に直接問い合わせる対象が無いため override 不要)。
//   ・**clip リストは値コピーで保持**: `SetClips` 渡しの `FAnimationClipBinding`
//     を内部 `TArray<FAnimationClipBinding>` に push_back。`name` は const char*
//     リテラル想定 (model loader が静的領域 or 永続バッファに保持) — STL
//     `std::string` を使えないため、ポインタ寿命は呼出側責任。
//   ・**Playing 状態は enum** (`EAnimationPlayState`): Stopped / Playing /
//     Paused の三状態。E-prefix 規約 (`enum class E*` + 基底 u8)。
//   ・**Tick (dt) を panel 内に持つ**: CSpriteAnimator と同設計 — DrawUI は
//     ImGui 描画専任、時刻進行は別 hook (= Workspace の OnFrameBegin 経由 or
//     FSample 側が明示的に Tick) に分離してテスト容易性を確保。本 panel の
//     AEditorPanel::OnFrameBegin は override せず、呼出側が Tick(dt) を直接
//     呼ぶ規約 (= Workspace の dt と animation の dt は分離したい場面用)。
//   ・**duration 到達時の挙動**: clip の `is_looping` または `m_LoopOverride`
//     が true なら wrap (= `m_CurrentTime = fmodf(t, dur)`)、そうでなければ
//     `_state = Stopped` + `m_CurrentTime = dur` で末尾に固定。
//     `m_LoopOverride = true` で clip 側 is_looping を強制 true 化 (= UI の
//     "Loop" checkbox に bind)。override は単方向 (= ON
//     にすると loop、OFF だと clip 既定に従う) で運用。
//   ・**Speed slider [0.1, 4.0]**: 仕様通り。逆再生 (= 負値) は範囲外で
//     未対応。0 は実質
//     一時停止と同義だが、UX を明確にするため 0 ではなく 0.1 を下限にする。
//   ・**BlendWeight slider [0, 1]**: 現状は **単一 clip 再生** のため
//     表示のみで再生にはほぼ影響しない (= renderer 側が CAnimationPlayer に
//     渡して final pose に blend する想定)。複数 clip blending を
//     入れる際に「複数 layer の weight」を扱う primary slot として再利用する。
//   ・**AnimationFrameCallback は raw 関数ポインタ + void* user**: ACS は
//     std::function 禁止。AParticleEditorPanel / CAssetBrowser と同形の C-style
//     callback 規約。Tick 終端で 1 度発火、引数は (user, clip_index, time_sec)。
//     未設定 (nullptr) なら no-op。
//   ・**非コピー / 非ムーブ**: 基底 AEditorPanel と同じ規約 + 内部
//     `TArray<FAnimationClipBinding>` の所有を曖昧にしない。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: AParticleEditorPanel / AModelViewerPanel と
//     同形 (= ヘッダから ImGui 依存を漏らさない)。
//
// 範囲外 (別 panel):
//   ・複数 clip の同時 blending (= 全身 vs 上半身レイヤ等)。
//     本 panel は primary slot のみ。
//   ・タイムライン上のキー打ち / イベントマーカー (= AnimCurveEditor の役割)。
//   ・root motion 抽出 / カメラ追従 (= CCinematicsDirector との連携範疇)。
//   ・clip 圧縮設定 / curve 編集 (= model importer の役割)。
//   ・retargeting (= bone mapping エディタの役割)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::modelview {

/**
 * animation clip の再生状態。
 *
 * @details
 * E-prefix 規約 (`enum class E*` + 基底 u8) に従う三状態。Play / Pause / Stop で
 * 遷移し、Tick(dt) は Playing のときだけ時刻を進める。
 */
enum class EAnimationPlayState : u8 {
    /** 未再生 (m_CurrentTime = 0、Play で頭から開始)。 */
    Stopped = 0,

    /** 再生中 (Tick(dt) で m_CurrentTime += dt * speed)。 */
    Playing = 1,

    /** 一時停止 (m_CurrentTime 保持、Play で同位置から再開)。 */
    Paused  = 2,
};

/**
 * model に含まれる 1 個の animation clip のメタ情報。
 *
 * @details
 * SetClips で外部から渡す POD 構造。本 panel は値コピーで内部 TArray に保持する
 * ため、呼出側はビルド済み配列を temp で渡してよい。
 */
struct FAnimationClipBinding {
    /** clip 表示名 (const char* リテラル / 永続バッファ想定。寿命は呼出側責任)。 */
    const char* name         = nullptr;

    /** clip の長さ (秒)。0 以下が来た場合は内部で clamp する。 */
    f32         duration_sec = 0.0f;

    /** clip 既定のループ可否 (UI の Loop checkbox で override 可能)。 */
    bool        is_looping   = false;

    /** 外部 CAnimationPlayer 内のクリップ ID (callback にそのまま渡す。中身は解釈しない)。 */
    u32         clip_index   = 0u;
};

/**
 * animation clip の再生制御 + timeline UI を提供する ModelViewer 用 panel。
 *
 * @details
 * editor_core::AEditorPanel を継承し、clip 選択 dropdown / Play / Pause / Stop /
 * Time slider / Loop checkbox / Speed slider / BlendWeight slider を ImGui で描画する。
 * clip メタ情報は SetClips で値コピー所有し、Tick(dt) で Playing 中の時刻を進めて
 * 終端で OnFrameCallback を発火する。実際の bone palette 計算や GPU 反映は呼出側
 * (renderer + CAnimationPlayer) に委譲し、本 panel は時刻進行と UI control のみを持つ。
 */
class AModelAnimationPanel : public acs::game::editor_core::AEditorPanel {
public:
    /**
     * Tick 終端で 1 度呼ばれる callback 型 (user, clip_index, time_sec)。
     *
     * @details
     * ACS は std::function 禁止のため raw 関数ポインタ + void* user 規約
     * (AParticleEditorPanel / CAssetBrowser と同形)。noexcept 必須。
     */
    using AnimationFrameCallback =
        void (*)(void* user, u32 clip_index, f32 time_sec) noexcept;

    /** 空状態で構築する (clip なし / Stopped / time 0)。 */
    AModelAnimationPanel() noexcept = default;

    /** 破棄する (内部 TArray は ~TArray が解放)。 */
    ~AModelAnimationPanel() noexcept override = default;

    /** コピー禁止 (基底 AEditorPanel と同規約)。 */
    AModelAnimationPanel(const AModelAnimationPanel&)            = delete;

    /** コピー代入も禁止。 */
    AModelAnimationPanel& operator=(const AModelAnimationPanel&) = delete;

    /** ムーブ禁止 (内部 TArray の所有を曖昧にしないため)。 */
    AModelAnimationPanel(AModelAnimationPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AModelAnimationPanel& operator=(AModelAnimationPanel&&)      = delete;

    /**
     * 内部 state を完全にデフォルトへ戻す (多重 Init 可)。
     *
     * @details clip リストを空に、selection / time を 0 に、callback も nullptr に戻す。
     */
    void Init() noexcept;

    /**
     * 内部 state を全解放する (多重 Shutdown 可)。
     *
     * @details TArray の中身は ~TArray で解放されるが、明示 Clear で再 Init の確定状態を作る。
     */
    void Shutdown() noexcept;

    /**
     * 外部 (model loader) からの clip メタ一覧を push する。
     *
     * @details
     * 内部 TArray を count 個に Resize して中身を値コピーする。selection は index 0 に
     * 自動リセット (count==0 なら -1)、時刻 / state も Stopped + 0 にリセットする
     * (モデル切替時の安全側)。
     * @param clips clip メタ配列 (nullptr なら空として扱う)。
     * @param count clip の数 (0 で空)。
     */
    void SetClips(const FAnimationClipBinding* clips, u32 count) noexcept;

    /**
     * clip リストを完全に空にする。
     *
     * @details selection = -1、state = Stopped、time = 0 にする。callback はクリアしない
     * (Init / Shutdown より浅い reset)。
     */
    void ClearClips() noexcept;

    /**
     * 現在登録されている clip の数を返す。
     *
     * @return clip 数。
     */
    u32 ClipCount() const noexcept;

    /**
     * 現在選択中の clip メタを返す (read-only)。
     *
     * @return 選択中 clip へのポインタ (未選択 / 範囲外なら nullptr)。
     */
    const FAnimationClipBinding* CurrentClip() const noexcept;

    /**
     * 現在選択中の clip index を返す。
     *
     * @return ClipCount() 未満の index (未選択は kNoClipSelected = -1)。
     */
    i32 CurrentClipIndex() const noexcept;

    /**
     * clip を選択する。
     *
     * @details
     * selection 切替時は時刻を 0 にリセット + Stopped に戻す (同 clip の再選択も
     * リスタート意図とみなす)。
     * @param clip_index 選択する clip の index (>= ClipCount() は no-op)。
     */
    void SelectClip(u32 clip_index) noexcept;

    /**
     * 現在位置から再生を開始する。
     *
     * @details
     * clip 未選択 (-1) なら no-op。Stopped から Play した場合は m_CurrentTime = 0 から、
     * Paused から Play した場合は m_CurrentTime 維持で再開する。
     */
    void Play() noexcept;

    /** 一時停止する (m_CurrentTime 保持)。Playing 以外からは no-op。 */
    void Pause() noexcept;

    /** 停止する (m_CurrentTime = 0 + state = Stopped)。Stopped でも安全。 */
    void Stop() noexcept;

    /**
     * 現在の再生状態を返す。
     *
     * @return Stopped / Playing / Paused のいずれか。
     */
    EAnimationPlayState PlayState() const noexcept;

    /**
     * 現在の再生位置 (秒) を返す。
     *
     * @return Stopped 時は 0、Playing/Paused 時は最新の Tick 結果または SetCurrentTimeSec の値。
     */
    f32 CurrentTimeSec() const noexcept;

    /**
     * 再生位置を直接設定する (UI の Time slider 操作 or 外部スクラブ用)。
     *
     * @details
     * 0 未満は 0、duration 超過は duration にクランプする (clip 未選択時は 0 のまま no-op)。
     * state は変更しない (Paused / Playing 中の scrub 後も状態を保持)。
     * @param t 設定する再生位置 (秒)。
     */
    void SetCurrentTimeSec(f32 t) noexcept;

    /**
     * 現在の再生速度 (倍率) を返す。
     *
     * @return Tick で m_CurrentTime += dt * speed に乗る倍率。
     */
    f32 PlaybackSpeed() const noexcept;

    /**
     * 再生速度を設定する。
     *
     * @details [kMinPlaybackSpeed, kMaxPlaybackSpeed] = [0.1, 4.0] にクランプ。0 は UX 上
     * 明確にするため下限 0.1 を強制し、負値 (逆再生) は受け付けない。
     * @param speed 設定する再生速度 (倍率)。
     */
    void SetPlaybackSpeed(f32 speed) noexcept;

    /**
     * Loop override が有効かを返す。
     *
     * @return true なら clip 側 is_looping を無視して強制 loop。false なら clip 既定に従う。
     */
    bool IsLoopingOverride() const noexcept;

    /**
     * Loop override を設定する (UI の Loop checkbox に bind)。
     *
     * @param b true で強制 loop、false で clip 既定 (current_clip->is_looping) に従う。
     */
    void SetLoopingOverride(bool b) noexcept;

    /**
     * blend weight を返す。
     *
     * @return blend weight [0, 1] (現状は単一 clip 再生のため表示用 + callback 出力の参考値)。
     */
    f32  BlendWeight() const noexcept;

    /**
     * blend weight を設定する。
     *
     * @details 0 未満は 0、1 超過は 1 にクランプする。
     * @param w 設定する blend weight。
     */
    void SetBlendWeight(f32 w) noexcept;

    /**
     * Playing 中の時刻を進め、duration 到達処理と callback 発火を行う。
     *
     * @details
     * Playing 中は m_CurrentTime += dt * m_Speed を進める。duration 到達時、loop 有効
     * (clip.is_looping || m_LoopOverride) なら余りを wrap、loop 無効なら time = duration +
     * state = Stopped に遷移する。終端で m_OnFrameCb を 1 度発火する (設定時)。clip 未選択 /
     * Stopped / Paused / dt <= 0 はすべて no-op (CSpriteAnimator と同方針、巻き戻し非対応)。
     * @param dt 前フレームからの経過秒 (0 以下は no-op)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * Tick 終端で呼ばれる callback を設定する。
     *
     * @details cb の引数 clip_index は FAnimationClipBinding::clip_index (外部 CAnimationPlayer ID)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user cb の第 1 引数に渡す任意ポインタ。
     */
    void SetOnFrameCallback(AnimationFrameCallback cb, void* user) noexcept;

    /**
     * window タイトル (ImGui::Begin の引数兼 ID) を返す。
     *
     * @return 固定リテラル "Animation"。
     */
    const char* Title() const noexcept override { return "Animation"; }

    /**
     * ImGui window 本体を描画する。
     *
     * @details
     * ImGui::Begin "Animation" + ClipCombo + Time slider + Play/Pause/Stop +
     * Loop checkbox + Speed slider + BlendWeight slider を描画する。IsVisible() が
     * false なら早期 return する (close ボタンで隠せる)。
     */
    void DrawUI() noexcept override;

    /** Speed slider の下限 (SetPlaybackSpeed の clamp にも使う)。 */
    static constexpr f32 kMinPlaybackSpeed = 0.1f;

    /** Speed slider の上限 (SetPlaybackSpeed の clamp にも使う)。 */
    static constexpr f32 kMaxPlaybackSpeed = 4.0f;

    /** 「未選択」を表す sentinel (i32 戻り値で使用)。 */
    static constexpr i32 kNoClipSelected = -1;

private:
    /** 内部 clip 一覧 (値コピー所有。name ポインタの寿命は呼出側責任)。 */
    TArray<FAnimationClipBinding> m_Clips {};

    /** 現在選択中の clip index (m_Clips 内 index、未選択は kNoClipSelected)。 */
    i32 m_CurrentClipIdx = kNoClipSelected;

    /** 再生状態 (Stopped / Playing / Paused)。 */
    EAnimationPlayState _state = EAnimationPlayState::Stopped;

    /** 現在の再生位置 (秒)。clip duration を超えないようクランプ / ラップされる。 */
    f32 m_CurrentTimeSec = 0.0f;

    /** 再生速度 (倍率)。[0.1, 4.0] にクランプ。 */
    f32 m_Speed = 1.0f;

    /** UI の Loop checkbox 状態 (true で clip 既定 is_looping を無視して強制 loop)。 */
    bool m_LoopOverride = false;

    /** blend weight (現状は表示 + callback 出力のみ、再生には影響しない)。 */
    f32 m_BlendWeight = 1.0f;

    /** Tick 終端で呼ばれる callback (外部 CAnimationPlayer への時刻反映点)。 */
    AnimationFrameCallback m_OnFrameCb = nullptr;

    /** m_OnFrameCb の第 1 引数に渡す任意ポインタ。 */
    void*                  m_OnFrameUser = nullptr;
};

using FModelAnimationPanel = AModelAnimationPanel;

} // namespace acs::game::modelview
