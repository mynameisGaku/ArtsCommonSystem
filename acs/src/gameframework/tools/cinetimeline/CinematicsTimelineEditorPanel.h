// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — cinetimeline / FCinematicsTimelineEditorPanel (Phase 23)
//
// `acs::game::FCinematicsDirector` (= タイムライン上の cutscene 駆動器) を
// **対話的に編集する timeline editor panel**。Unity Timeline / Unreal Sequencer /
// Godot FAnimationPlayer の「水平タイムライン + 複数トラック + キーフレーム
// マーカー + 再生制御」の最小版に相当。Phase 21a で確立された
// `editor_core::FEditorPanel` 基底に載せ、`FEditorWorkspace::RegisterPanel(&panel)`
// の 1 行で workspace に統合できる形にしている。
//
// 役割:
//   ・上部 toolbar: 再生制御 (Play / Pause / Stop / Step) + Time slider
//     (= [0, Duration] でタイムカーソルを drag scrub) + Add Keyframe combo
//     (= 5 種から選んで現在カーソル位置に新規キー追加)
//   ・中央 viewport: 水平タイムライン view (= 5 種の kind ごとに横一列のトラック、
//     各キーフレームを「縦長の四角 marker」で描画)。マウス drag で marker の
//     time を編集、選択中の marker は色変えで強調。
//   ・右側 inspector: 選択中の keyframe の kind / time / kind 別パラメータを
//     ドラッグ float 等で編集可能 (例 CameraCut なら target FVec3、FadeColor なら
//     start/end color)
//   ・下部 ruler: 0s / 1s / 2s ... の時間メモリ + 現在カーソル位置の縦線
//
// 役割分担:
//   ・本 panel は「**timeline の編集 UI** だけ」を担当。FCinematicsDirector
//     データは caller 所有 (= `SetCinematicsDirector(&dir)` で raw 参照を渡す、
//     寿命は caller 責任)。本 panel が director を生成 / 破棄しない
//     (FAnimCurveEditorPanel が FAnimationCurve を non-owning で受けるのと同形)。
//   ・panel は **editor 専用の keyframe storage** (= `EditorKeyframe` の TArray)
//     を内部に持つ。FCinematicsDirector::FTimelineKeyframe は payload が
//     {camera/dialogue/music/event} の 4 種固定 union だが、editor 上では
//     UE Sequencer / Unity Timeline のように "5 種類のオーサリング概念"
//     (CameraCut / FadeColor / TimeScale / SpawnEffect / TriggerCallback) を
//     扱えるように **追加 metadata** (FVec3 target / start/end color / scale 値
//     / event_id / 文字列リテラル) を panel 側で保持する。Play 時に director に
//     対応する FTimelineKeyframe を AddKeyframe する設計 (= editor data →
//     runtime data の「ベイク」)。
//   ・実 file save/load は本 panel 範囲外 (= sample 37 menu で stub menu、本物は
//     別 phase で .acscinetimeline serializer を追加した時点で配線)。
//
// 使い方 (典型):
//   FCinematicsTimelineEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);
//
//   FCinematicsDirector director;
//   panel.SetCinematicsDirector(&director);
//
//   // 初期キーを 3 個追加
//   panel.AddKeyframe(ETimelineKeyKind::CameraCut,       0.0f);
//   panel.AddKeyframe(ETimelineKeyKind::FadeColor,       2.0f);
//   panel.AddKeyframe(ETimelineKeyKind::TriggerCallback, 5.0f);
//
//   // 毎フレーム TickAllPanels(dt) の中で OnFrameBegin + DrawUI が呼ばれる。
//   // Play 中なら panel.Step(dt) で時間進行 + director.Tick (= keyframe 発火)。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel);
//   panel.Shutdown();
//
// 設計選択 (Phase 23 CinematicsTimelineEditor):
//   ・**FEditorPanel 継承**: Phase 21a 共通基盤の dogfood (sample 31 ModelViewer /
//     sample 32 AnimCurveEditor / sample 34 LevelEditor 等に続く)。
//     Title = "Cinematics Timeline"、DrawUI override。
//   ・**5 種類の ETimelineKeyKind**: CameraCut / FadeColor / TimeScale /
//     SpawnEffect / TriggerCallback。FCinematicsDirector::ETimelineTrackKind
//     (= Wait / MoveCamera / ShowDialogue / PlayMusic / FireEvent) とは
//     **意図的に概念を分離**: editor 側はオーサリング向けの "演出意図" を表現する
//     5 種 (cinematic 制作で典型的に並べたい要素) で、director は runtime 向けの
//     5 種 (= 副作用ゼロな状態遷移マーカー)。両者は ToTrackKind() マッピング
//     関数で繋ぐ。例: CameraCut → MoveCamera、FadeColor/TimeScale/SpawnEffect/
//     TriggerCallback → FireEvent (event_id を kind ごとに別 reserve)。
//   ・**TArray<EditorKeyframe> は panel 所有**: director の internal m_Keyframes
//     を直接編集するには FCinematicsDirector の private を覗く必要があり、また
//     payload に色や FVec3 を持たせるには既存 union を拡張する必要がある。
//     editor は「リッチな payload を持つ自前 storage」を持ち、Play 時に
//     director に baked FTimelineKeyframe を渡す方が依存が浅い。
//   ・**panel-local clock (`m_CurrentTime`) と director.CurrentTime() の二重保持**:
//     editor で scrub したいだけのときは director を進めずに `m_CurrentTime` の
//     カーソルだけ動かす。Play 時は両方同期 (panel が Step(dt) → director.Tick(dt)
//     → panel.m_CurrentTime = director.CurrentTime())。Stop 時は両方 0 に reset。
//   ・**Duration は panel 側設定 (default 10s)**: director の TotalDuration() は
//     keyframe の末尾 time から自動算出されるが、編集中は「将来追加される
//     keyframe のために予め長めに取りたい」需要がある。editor 側で
//     `SetDurationSec()` で明示できる方が UX が良い。
//   ・**ImGui canvas は ImGui::InvisibleButton + GetWindowDrawList()**: AnimCurve
//     FEditorPanel と同パターン。トラック行ごとに marker を描く layout は
//     row_height (~24px) × 5 rows で固定 (= スクロールなしで全 kind 見える)。
//   ・**marker のドラッグ判定 = AABB hit-test**: 縦長矩形 (10px 幅) の AABB に
//     マウス位置が入っているか + LMB クリック。drag 中はマウス x → time へ
//     逆変換して keyframe.time_sec を上書き。
//   ・**選択 keyframe index は i32 (-1 = 未選択)**: `kNoKeySelected` を sentinel。
//     FAnimCurveEditorPanel と同形。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (FAnimCurveEditorPanel /
//     FParticleEditorPanel) と同形。
//
// 範囲外 (Phase 23 では持たない、将来追加候補):
//   ・実 file save/load (= sample 37 menu で stub、別 phase で
//     .acscinetimeline serializer を作って配線)
//   ・複数 director の同時編集
//   ・undo / redo 統合 (= Phase 21b FUndoStack 経由、将来 OnUndo / OnRedo hook)
//   ・keyframe の補間 / curve 編集 (= AnimCurveEditor と統合した時点で検討)
//   ・track の add/remove (= 現状 5 種固定)
//   ・marker 複数選択 + 一括 drag
//   ・スナップグリッド / 等間隔配置 (= 現状は自由配置のみ)
//   ・preview viewport (= scrub したら 3D scene が更新されるリアルタイム連動)
//
// 参考: gameframework/FCinematicsDirector.h,
//      gameframework/tools/editor_core/FEditorPanel.h,
//      gameframework/tools/animcurve/FAnimCurveEditorPanel.h
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// 編集対象の FCinematicsDirector は本ヘッダから forward-decl のみで受ける。
// `<gameframework/FCinematicsDirector.h>` を include しないことで、本 panel を
// 利用する側がヘッダ依存を最小化できる (= FCinematicsDirector 自体の変更で
// 不要な再ビルドを避ける)。
class FCinematicsDirector;
} // namespace acs::game

namespace acs::game::cinetimeline {

// ---------------------------------------------------------------------------
// ETimelineKeyKind — 編集 UI 上で扱う 5 種の演出概念
// ---------------------------------------------------------------------------
// FCinematicsDirector::ETimelineTrackKind (runtime 側 5 種) とは概念を意図的に
// 分離し、オーサリング側で典型的に並べたい要素を素直に表現する。
//   CameraCut       : カメラ位置を瞬時に切り替え (target FVec3)
//   FadeColor       : 画面全体を start_color → end_color にフェード
//   TimeScale       : ゲーム時間スケールを倍率指定 (1.0 = 等倍、0.5 = スロー)
//   SpawnEffect     : エフェクト発火 (effect_id + position)
//   TriggerCallback : 汎用 callback 発火 (event_id をユーザ定義で解釈)
enum class ETimelineKeyKind : u8 {
    CameraCut       = 0,
    FadeColor       = 1,
    TimeScale       = 2,
    SpawnEffect     = 3,
    TriggerCallback = 4,
};

// ---------------------------------------------------------------------------
// EditorKeyframe — panel 内部で保持する keyframe (リッチ payload 付き)
// ---------------------------------------------------------------------------
// FCinematicsDirector::FTimelineKeyframe より広い payload を持つ。Play 時に
// director に AddKeyframe する際は「kind に応じた FTimelineKeyframe」へ変換する。
struct EditorKeyframe {
    f32              time_sec = 0.0f;                            // タイムライン上の発火時刻 [秒]
    ETimelineKeyKind kind     = ETimelineKeyKind::TriggerCallback;

    // ---- kind 別 payload (active な kind に対応するフィールドのみ意味を持つ) ----
    // CameraCut       : camera_target を使う
    // FadeColor       : fade_start_color / fade_end_color を使う
    // TimeScale       : time_scale を使う
    // SpawnEffect     : effect_id (event_id), spawn_position (camera_target を兼用) を使う
    // TriggerCallback : event_id を使う
    FVec3 camera_target  { 0.0f, 0.0f, 0.0f };   // CameraCut / SpawnEffect で使用
    FVec3 fade_start_color { 0.0f, 0.0f, 0.0f }; // FadeColor で使用 (r,g,b in [0,1])
    FVec3 fade_end_color   { 1.0f, 1.0f, 1.0f }; // FadeColor で使用 (r,g,b in [0,1])
    f32  time_scale       { 1.0f };             // TimeScale で使用
    u32  event_id         { 0u };               // SpawnEffect / TriggerCallback で使用

    EditorKeyframe() noexcept = default;
};

// ---------------------------------------------------------------------------
// FCinematicsTimelineEditorPanel — timeline 編集 UI panel
// ---------------------------------------------------------------------------
class FCinematicsTimelineEditorPanel : public acs::game::editor_core::FEditorPanel {
public:
    FCinematicsTimelineEditorPanel() noexcept = default;
    ~FCinematicsTimelineEditorPanel() noexcept override = default;

    // 非コピー / 非ムーブ: 基底 FEditorPanel と同規約 + 内部の keyframe 配列 /
    // selection / director 参照を曖昧にしない (ACS 規約)。
    FCinematicsTimelineEditorPanel(const FCinematicsTimelineEditorPanel&)            = delete;
    FCinematicsTimelineEditorPanel& operator=(const FCinematicsTimelineEditorPanel&) = delete;
    FCinematicsTimelineEditorPanel(FCinematicsTimelineEditorPanel&&)                 = delete;
    FCinematicsTimelineEditorPanel& operator=(FCinematicsTimelineEditorPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、director 参照を nullptr、selection を未選択、
    // duration を default (10s)、keyframes を空に。多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を全解放 (= director 参照 / selection / keyframes を解除)。
    // director 自体は caller 所有なので本 panel が破棄しない。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- director バインド ----------------------------------------------

    // 編集対象の FCinematicsDirector を raw 参照でセット (nullptr で解除)。
    // 寿命は caller 責任 (本 panel は director を所有しない)。
    // セット直後は selection をリセット、m_CurrentTime も 0 に戻す。
    void SetCinematicsDirector(acs::game::FCinematicsDirector* dir) noexcept;

    // 現在編集対象の FCinematicsDirector (未バインド時は nullptr)。
    acs::game::FCinematicsDirector* CurrentDirector() const noexcept;

    // ----- 再生制御 --------------------------------------------------------

    // タイムカーソルを現在位置から再生開始。director がバインドされていれば
    // director の全 keyframe を一旦 Clear → editor の現状で再 bake → Play する
    // (= editor で編集した内容を runtime に反映してから再生開始)。
    void Play() noexcept;

    // 再生一時停止 (`m_Playing = false`、`m_CurrentTime` 保持)。
    void Pause() noexcept;

    // 完全停止 (`m_Playing = false`、`m_CurrentTime = 0`、director.Stop も呼ぶ)。
    void Stop() noexcept;

    // dt 秒進める。`m_Playing == true` なら director.Tick(dt) も呼んで keyframe を
    // 発火させ、`m_CurrentTime` を director.CurrentTime() と同期する。
    // `m_Playing == false` (= scrub 中) なら panel-local clock だけ進める用途には
    // 使わない (= scrub は slider 直接編集で行う、Step は再生中専用)。
    void Step(f32 dt) noexcept;

    bool IsPlaying() const noexcept;

    // ----- 時間軸アクセサ -------------------------------------------------

    f32  CurrentTimeSec() const noexcept;
    void SetCurrentTimeSec(f32 t) noexcept;   // [0, Duration] にクランプ

    f32  DurationSec() const noexcept;
    void SetDurationSec(f32 d) noexcept;      // 最低 0.1s にクランプ

    // ----- keyframe 操作 --------------------------------------------------

    // 現在選択中の keyframe index (= 内部 TArray の index)、未選択は kNoKeySelected。
    i32  SelectedKeyframeIndex() const noexcept;

    // selection を変更 (`-1 .. KeyframeCount-1` 範囲外なら kNoKeySelected に丸める)。
    void SelectKeyframe(i32 i) noexcept;

    // 新規 keyframe を kind + time_sec で追加。time_sec は [0, Duration] にクランプ。
    // 追加後は新 keyframe を selection にする。
    // 内部 keyframe 配列は time 昇順を維持する (= insertion 時に sort)。
    void AddKeyframe(ETimelineKeyKind kind, f32 time_sec) noexcept;

    // 選択中の keyframe を削除 (selection が無効なら no-op)。削除後は selection を
    // 解除する。
    void RemoveSelectedKeyframe() noexcept;

    // ----- FEditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "Cinematics Timeline"; }

    // ImGui::Begin "Cinematics Timeline" + Toolbar + Timeline canvas + Inspector
    // + Ruler を描画。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // 「未選択」を表す sentinel (i32 戻り値で使用)。FAnimCurveEditorPanel と同形。
    static constexpr i32 kNoKeySelected = -1;

    // タイムライン全 track の数 = ETimelineKeyKind の要素数。
    static constexpr u32 kTrackCount = 5u;

    // 1 トラックの行高さ (px)。5 トラック分の縦サイズの基準。
    static constexpr f32 kTrackRowHeightPx = 28.0f;

    // marker の幅 (px) と AABB hit-test 余裕。
    static constexpr f32 kMarkerWidthPx     = 10.0f;
    static constexpr f32 kMarkerHitSlackPx  = 3.0f;

    // Duration の最低値 (秒)。0 に近づくとタイム軸が破綻するため。
    static constexpr f32 kMinDurationSec = 0.1f;

    // Duration のデフォルト値 (秒)。
    static constexpr f32 kDefaultDurationSec = 10.0f;

private:
    // 編集対象 FCinematicsDirector (caller 所有、本 panel は非所有)。
    acs::game::FCinematicsDirector* m_Director = nullptr;

    // panel 所有の keyframe 配列 (time 昇順、stable insertion で維持)。
    TArray<EditorKeyframe> m_Keyframes;

    // 現在選択中の keyframe index。
    i32 m_SelectedIdx = kNoKeySelected;

    // タイムライン上の現在時刻 [秒]。slider / Step / Stop で更新。
    f32 m_CurrentTime = 0.0f;

    // タイムライン全体の長さ [秒]。default 10s。
    f32 m_Duration = kDefaultDurationSec;

    // 再生中フラグ。Play で true、Pause / Stop で false。
    bool m_Playing = false;

    // marker ドラッグ中フラグ + 対象 index。Step / 他処理に影響しない UI 専用 state。
    bool m_bDraggingMarker = false;
    i32  m_DragIdx        = -1;

    // 追加 combo のために現在選択している kind (Add ボタン押下時にこの kind で
    // 新規 keyframe を m_CurrentTime に追加)。
    ETimelineKeyKind m_AddKind = ETimelineKeyKind::CameraCut;

    // ヘルパ: keyframes を time 昇順を保ったまま `kf` を挿入する。
    // 戻り値: 挿入された index (= sort 後の位置)。
    i32 InsertKeyframeSorted(const EditorKeyframe& kf) noexcept;

    // ヘルパ: editor の全 keyframe を director に bake (= Clear + AddKeyframe loop)。
    // SetCinematicsDirector / Play のタイミングで呼ぶ。director が nullptr なら no-op。
    void BakeToDirector() noexcept;
};

} // namespace acs::game::cinetimeline
