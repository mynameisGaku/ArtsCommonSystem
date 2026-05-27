// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / FModelAnimationPanel (Phase 21b)
//
// ModelViewer 内で、ロード済みモデルに含まれる **animation clip の再生制御 +
// timeline 表示** を行う panel。Phase 21a 共通基盤の `editor_core::FEditorPanel`
// を継承し、ImGui ベースの control 群 (clip dropdown / Play / Pause / Stop /
// time slider / Loop checkbox / Speed slider / BlendWeight slider) を提供する。
//
// 役割分担 (Phase 21b ModelViewer 全体):
//   ・FModelViewerPanel    : 3D viewport + Lighting / Background / Grid / FBone toggle
//   ・FModelInspectorPanel : mesh 統計 (vertex/index/material/bone count)
//   ・FModelMaterialPanel  : material slot 編集 (basecolor / metallic / roughness)
//   ・FModelAnimationPanel : 本 panel — animation clip の再生制御 + timeline
//   ・FSample 31 demo      : 上記 4 panel を Workspace に組み立て
//
// 役割:
//   ・ロード済みモデルが持つ animation clip 一覧を保持 (`SetClips`)
//   ・clip 切替 (dropdown) + Play / Pause / Stop + Loop toggle + Speed slider
//     + Time slider (現フレームの時刻 [0, duration]) + Blend weight slider
//   ・`Tick(dt)` で Playing 状態の場合 `m_CurrentTime += dt * m_Speed` を進める
//   ・duration 到達時、looping ならラップ、そうでなければ Stopped に遷移
//   ・毎 Tick 終端で `m_OnFrameCb(user, clip_index, time_sec)` を発火し、
//     呼出側 (ModelViewportRenderer 等) が FAnimationPlayer に時刻を反映する
//
// 役割分担 (本 panel と外部):
//   ・本 panel は **時刻の進行 + clip 選択 + UI control** だけを持つ。
//     実際の bone palette 計算 / GPU 反映 / mesh 描画は呼出側 (renderer +
//     FAnimationPlayer) に委譲する。`OnFrameCallback` がその橋渡し点。
//   ・clip メタ情報 (name / duration / looping / clip_index) は外部 (model
//     loader) が確定済みの値を `SetClips` で push する。本 panel は中身を
//     コピー所有 (= FAnimationClipBinding 配列を TArray<>); pointer は持たない。
//
// 使い方 (典型):
//   FModelAnimationPanel anim;
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
//   // → callback 内で FAnimationPlayer::SetTime(clip_index, time_sec) を呼ぶ
//
// 設計選択 (Phase 21b FModelAnimationPanel):
//   ・**FEditorPanel 継承**: Phase 21a 基盤に準拠。Title / DrawUI を override。
//     OnInit は基底実装 (`Workspace()` ポインタ保持) のみで十分 (= 本 panel は
//     workspace に直接問い合わせる対象が無いため override 不要)。
//   ・**clip リストは値コピーで保持**: `SetClips` 渡しの `FAnimationClipBinding`
//     を内部 `TArray<FAnimationClipBinding>` に push_back。`name` は const char*
//     リテラル想定 (model loader が静的領域 or 永続バッファに保持) — STL
//     `std::string` を使えないため、ポインタ寿命は呼出側責任。
//   ・**Playing 状態は enum** (`EAnimationPlayState`): Stopped / Playing /
//     Paused の三状態。Phase 19a E-prefix 規約 (`enum class E*` + 基底 u8)。
//   ・**Tick (dt) を panel 内に持つ**: FSpriteAnimator と同設計 — DrawUI は
//     ImGui 描画専任、時刻進行は別 hook (= Workspace の OnFrameBegin 経由 or
//     FSample 側が明示的に Tick) に分離してテスト容易性を確保。本 panel の
//     FEditorPanel::OnFrameBegin は override せず、呼出側が Tick(dt) を直接
//     呼ぶ規約 (= Workspace の dt と FAnimation の dt は分離したい場面用)。
//   ・**duration 到達時の挙動**: clip の `is_looping` または `m_LoopOverride`
//     が true なら wrap (= `m_CurrentTime = fmodf(t, dur)`)、そうでなければ
//     `_state = Stopped` + `m_CurrentTime = dur` で末尾に固定。
//     `m_LoopOverride = true` で clip 側 is_looping を強制 true 化 (= UI の
//     "Loop" checkbox に bind)。Phase 21b 時点では override は単方向 (= ON
//     にすると loop、OFF だと clip 既定に従う) で運用。
//   ・**Speed slider [0.1, 4.0]**: 仕様通り。逆再生 (= 負値) は範囲外で
//     Phase 21b では未対応 (将来必要なら slider 下限を負に拡張)。0 は実質
//     一時停止と同義だが、UX を明確にするため 0 ではなく 0.1 を下限にする。
//   ・**BlendWeight slider [0, 1]**: Phase 21b は **単一 clip 再生** のため
//     表示のみで再生にはほぼ影響しない (= renderer 側が FAnimationPlayer に
//     渡して final pose に blend する想定)。Phase 22+ で複数 clip blending を
//     入れる際に「複数 layer の weight」を扱う primary slot として再利用。
//   ・**AnimationFrameCallback は raw 関数ポインタ + void* user**: ACS は
//     std::function 禁止。FParticleEditorPanel / FAssetBrowser と同形の C-style
//     callback 規約。Tick 終端で 1 度発火、引数は (user, clip_index, time_sec)。
//     未設定 (nullptr) なら no-op。
//   ・**非コピー / 非ムーブ**: 基底 FEditorPanel と同じ規約 + 内部
//     `TArray<FAnimationClipBinding>` の所有を曖昧にしない。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: FParticleEditorPanel / FModelViewerPanel と
//     同形 (= ヘッダから ImGui 依存を漏らさない)。
//
// 範囲外 (将来 / 別 panel):
//   ・複数 clip の同時 blending (= 全身 vs 上半身レイヤ等) は Phase 22+。
//     本 panel は primary slot のみ。
//   ・タイムライン上のキー打ち / イベントマーカー (= AnimCurveEditor の役割)。
//   ・root motion 抽出 / カメラ追従 (= FCinematicsDirector との連携範疇)。
//   ・clip 圧縮設定 / curve 編集 (= model importer の役割)。
//   ・retargeting (= bone mapping エディタの将来役割)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game::modelview {

// ---------------------------------------------------------------------------
// EAnimationPlayState — 再生状態
// ---------------------------------------------------------------------------
// Phase 19a E-prefix 規約 (= `enum class E*` + 基底 u8)。
//   Stopped : 未再生 (= `m_CurrentTime = 0`、Play で頭から開始)
//   Playing : 再生中 (= Tick(dt) で `m_CurrentTime += dt * speed`)
//   Paused  : 一時停止 (= `m_CurrentTime` 保持、Play で同位置から再開)
// ---------------------------------------------------------------------------
enum class EAnimationPlayState : u8 {
    Stopped = 0,
    Playing = 1,
    Paused  = 2,
};

// ---------------------------------------------------------------------------
// FAnimationClipBinding — model に含まれる 1 個の animation clip メタ情報
// ---------------------------------------------------------------------------
// `SetClips` で外部から渡す POD 構造。本 panel は値コピーで内部 TArray に
// 保持する (= 呼出側はビルド済み配列を temp で渡してよい)。
//   name       : clip 表示名。const char* リテラル / 永続バッファ想定
//                (= 本 panel はコピー所有しない、ポインタ寿命は呼出側責任)。
//   duration_sec: clip の長さ (秒)。0 以下が来た場合は内部で clamp する。
//   is_looping : clip 既定のループ可否。UI の Loop checkbox で override 可能。
//   clip_index : 外部 FAnimationPlayer 内のクリップ ID (= 呼出側 callback に
//                そのまま渡される値)。本 panel は中身を解釈しない。
// ---------------------------------------------------------------------------
struct FAnimationClipBinding {
    const char* name         = nullptr;
    f32         duration_sec = 0.0f;
    bool        is_looping   = false;
    u32         clip_index   = 0u;
};

// ---------------------------------------------------------------------------
// FModelAnimationPanel — animation clip 再生制御 + timeline UI
// ---------------------------------------------------------------------------
class FModelAnimationPanel : public acs::game::editor_core::FEditorPanel {
public:
    // Tick 終端で呼ばれる callback. (user, clip_index, time_sec)。
    // ACS は std::function 禁止のため raw 関数ポインタ + void* user 規約
    // (FParticleEditorPanel / FAssetBrowser と同形)。`noexcept` 必須。
    using AnimationFrameCallback =
        void (*)(void* user, u32 clip_index, f32 time_sec) noexcept;

    FModelAnimationPanel() noexcept = default;
    ~FModelAnimationPanel() noexcept override = default;

    // 非コピー・非ムーブ: 基底 FEditorPanel と同規約 + 内部 TArray の所有を
    // 曖昧にしない (ACS 規約)。
    FModelAnimationPanel(const FModelAnimationPanel&)            = delete;
    FModelAnimationPanel& operator=(const FModelAnimationPanel&) = delete;
    FModelAnimationPanel(FModelAnimationPanel&&)                 = delete;
    FModelAnimationPanel& operator=(FModelAnimationPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、clip リストを空に、selection / time を 0 に。
    // callback も nullptr に戻す (= 完全リセット)。多重 Init 可。
    void Init() noexcept;

    // 内部 state を全解放 (TArray の中身は ~TArray で解放されるが、明示 Clear
    // で再 Init の確定状態を作る)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- clip リスト管理 -------------------------------------------------

    // 外部 (model loader) からの clip メタ一覧 push。`count == 0` で空。
    // 内部 TArray を `count` 個に Resize して中身を値コピーする。
    // 既存の selection は reset (= 自動的に index 0 を選択、count==0 なら -1)。
    // 時刻 / state も Stopped + 0 にリセット (= モデル切替時の安全側)。
    void SetClips(const FAnimationClipBinding* clips, u32 count) noexcept;

    // clip リストを完全に空に。selection = -1、state = Stopped、time = 0 にする。
    // callback はクリアしない (= Init / Shutdown と区別)。
    void ClearClips() noexcept;

    // 現在登録されている clip の数。
    u32 ClipCount() const noexcept;

    // 現在選択中の clip メタ (read-only)。未選択 / 範囲外は nullptr。
    const FAnimationClipBinding* CurrentClip() const noexcept;

    // 現在選択中の clip index (`ClipCount()` 未満の値、未選択は -1)。
    i32 CurrentClipIndex() const noexcept;

    // clip を選択。範囲外 (>= ClipCount()) は no-op。
    // selection 切替時は時刻を 0 にリセット + Stopped に戻す
    // (= 別 clip を選択したのに前 clip の time が残ると混乱するため)。
    void SelectClip(u32 clip_index) noexcept;

    // ----- 再生制御 --------------------------------------------------------

    // 現在位置から再生開始。clip が未選択 (= -1) なら no-op。
    // Stopped から Play した場合は `m_CurrentTime = 0` から、Paused から
    // Play した場合は `m_CurrentTime` 維持で再開。
    void Play() noexcept;

    // 一時停止 (`m_CurrentTime` 保持)。`Playing` 以外からは no-op。
    void Pause() noexcept;

    // 停止 (`m_CurrentTime = 0` + state = Stopped)。`Stopped` でも安全。
    void Stop() noexcept;

    // 現在の再生状態。
    EAnimationPlayState PlayState() const noexcept;

    // ----- 時刻 / 速度 -----------------------------------------------------

    // 現在の再生位置 (秒)。Stopped 時は 0、Playing/Paused 時は最新の Tick
    // 結果 or `SetCurrentTimeSec` で書かれた値。
    f32 CurrentTimeSec() const noexcept;

    // 再生位置を直接設定 (= UI の Time slider 操作 or 外部スクラブ用)。
    // 0 未満は 0、duration 超過は duration にクランプ (clip 未選択時は
    // `m_CurrentTime = 0` のまま no-op)。state は変更しない (= Paused 中の
    // scrub 後も Paused のまま、Playing 中の scrub 後も Playing のまま)。
    void SetCurrentTimeSec(f32 t) noexcept;

    // 再生速度 (倍率)。Tick で `m_CurrentTime += dt * speed` に乗る。
    f32 PlaybackSpeed() const noexcept;

    // 再生速度を設定。仕様: [0.1, 4.0] にクランプ。
    // 0 は実質一時停止だが UX 上明確にするため下限 0.1 を強制。
    // 負値は受け付けない (= 逆再生は Phase 21b 範囲外)。
    void SetPlaybackSpeed(f32 speed) noexcept;

    // ----- Loop override + BlendWeight ------------------------------------

    // UI の Loop checkbox 状態。true なら clip 側 is_looping を無視して
    // 強制 loop。false なら clip 既定 (= `current_clip->is_looping`) に従う。
    bool IsLoopingOverride() const noexcept;
    void SetLoopingOverride(bool b) noexcept;

    // blend weight [0, 1]。Phase 21b は単一 clip 再生のため表示用 + callback
    // 経由で外部 renderer が FAnimationPlayer に渡す参考値。0 未満は 0、
    // 1 超過は 1 にクランプ。
    f32  BlendWeight() const noexcept;
    void SetBlendWeight(f32 w) noexcept;

    // ----- Tick + callback -------------------------------------------------

    // Playing 中は `m_CurrentTime += dt * m_Speed`。duration 到達時は
    //   ・loop 有効 → 余りを wrap (= `m_CurrentTime -= duration` を繰り返す)
    //   ・loop 無効 → `m_CurrentTime = duration` + state = Stopped に遷移
    // 終端で callback (`m_OnFrameCb`) を発火 (= 設定されていれば)。
    // clip 未選択 / Stopped / Paused 時は no-op (= state を進めず callback も
    // 発火しない)。dt <= 0 も no-op (= 巻き戻し非対応、FSpriteAnimator と同方針)。
    void Tick(f32 dt) noexcept;

    // Tick 終端で 1 度呼ばれる callback を設定。nullptr 渡しで解除可能。
    // cb の引数: (user, clip_index, time_sec) — clip_index は
    // `FAnimationClipBinding::clip_index` (= 外部 FAnimationPlayer ID)。
    void SetOnFrameCallback(AnimationFrameCallback cb, void* user) noexcept;

    // ----- FEditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "FAnimation"; }

    // ImGui::Begin "FAnimation" + ClipCombo + Time slider + Play/Pause/Stop +
    // Loop checkbox + Speed slider + BlendWeight slider 描画。
    // `IsVisible()` が false なら早期 return (= close ボタンで隠せる)。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // Speed slider の範囲。SetPlaybackSpeed の clamp にも使う。
    static constexpr f32 kMinPlaybackSpeed = 0.1f;
    static constexpr f32 kMaxPlaybackSpeed = 4.0f;

    // 「未選択」を表す sentinel (i32 戻り値で使用)。
    static constexpr i32 kNoClipSelected = -1;

private:
    // 内部 clip 一覧 (値コピー所有)。name ポインタの寿命は呼出側責任。
    TArray<FAnimationClipBinding> m_Clips {};

    // 現在選択中の clip index (`m_Clips` 内 index、未選択は kNoClipSelected)。
    i32 m_CurrentClipIdx = kNoClipSelected;

    // 再生状態。Stopped / Playing / Paused。
    EAnimationPlayState _state = EAnimationPlayState::Stopped;

    // 現在の再生位置 (秒)。clip duration を超えないようにクランプ / ラップされる。
    f32 m_CurrentTimeSec = 0.0f;

    // 再生速度 (倍率)。[0.1, 4.0] にクランプ。
    f32 m_Speed = 1.0f;

    // UI の Loop checkbox 状態。true なら clip 既定 is_looping を無視して
    // 強制 loop。false なら clip 既定に従う。
    bool m_LoopOverride = false;

    // blend weight (Phase 21b は表示 + callback 出力のみ、再生には影響しない)。
    f32 m_BlendWeight = 1.0f;

    // Tick 終端で呼ばれる callback (= 外部 FAnimationPlayer への時刻反映点)。
    AnimationFrameCallback m_OnFrameCb = nullptr;
    void*                  m_OnFrameUser = nullptr;
};

} // namespace acs::game::modelview
