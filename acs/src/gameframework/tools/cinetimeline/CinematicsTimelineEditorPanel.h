// SPDX-License-Identifier: Apache-2.0
#pragma once
// GameFramework の映像 timeline editor — ACinematicsTimelineEditorPanel
//
// `acs::game::CCinematicsDirector` (= タイムライン上の cutscene 駆動器) を
// **対話的に編集する timeline editor panel**。水平タイムライン、複数トラック、
// キーフレームマーカー、再生制御を 1 つの panel にまとめる。
// `editor_core::AEditorPanel` 基底に載せ、`CEditorWorkspace::RegisterPanel(&panel)`
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
//   ・本 panel は「**timeline の編集 UI** だけ」を担当。CCinematicsDirector
//     データは caller 所有 (= `SetCinematicsDirector(&dir)` で raw 参照を渡す、
//     寿命は caller 責任)。本 panel が director を生成 / 破棄しない
//     (AAnimCurveEditorPanel が FAnimationCurve を non-owning で受けるのと同形)。
//   ・panel は **editor 専用の keyframe storage** (= `FEditorKeyframe` の TArray)
//     を内部に持つ。CCinematicsDirector::FTimelineKeyframe は payload が
//     {camera/dialogue/music/event} の 4 種固定 union だが、editor 上では
//     5 種類のオーサリング概念
//     (CameraCut / FadeColor / TimeScale / SpawnEffect / TriggerCallback) を
//     扱えるように **追加 metadata** (FVec3 target / start/end color / scale 値
//     / event_id / 文字列リテラル) を panel 側で保持する。Play 時に director に
//     対応する FTimelineKeyframe を AddKeyframe する設計 (= editor data →
//     runtime data の「ベイク」)。
//   ・実 file save/load は本 panel 範囲外で、呼び出し側が保存を担当する。
//
// 使い方 (典型):
//   ACinematicsTimelineEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);
//
//   CCinematicsDirector director;
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
// 設計選択:
//   ・**AEditorPanel 継承**: editor_core 共通基盤の上に載せる。
//     Title = "Cinematics Timeline"、DrawUI override。
//   ・**5 種類の ETimelineKeyKind**: CameraCut / FadeColor / TimeScale /
//     SpawnEffect / TriggerCallback。CCinematicsDirector::ETimelineTrackKind
//     (= Wait / MoveCamera / ShowDialogue / PlayMusic / FireEvent) とは
//     **意図的に概念を分離**: editor 側はオーサリング向けの "演出意図" を表現する
//     5 種 (cinematic 制作で典型的に並べたい要素) で、director は runtime 向けの
//     5 種 (= 副作用ゼロな状態遷移マーカー)。両者は ToTrackKind() マッピング
//     関数で繋ぐ。例: CameraCut → MoveCamera、FadeColor/TimeScale/SpawnEffect/
//     TriggerCallback → FireEvent (event_id を kind ごとに別 reserve)。
//   ・**TArray<FEditorKeyframe> は panel 所有**: director の internal m_Keyframes
//     を直接編集するには CCinematicsDirector の private を覗く必要があり、また
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
//     AEditorPanel と同パターン。トラック行ごとに marker を描く layout は
//     row_height (~24px) × 5 rows で固定 (= スクロールなしで全 kind 見える)。
//   ・**marker のドラッグ判定 = AABB hit-test**: 縦長矩形 (10px 幅) の AABB に
//     マウス位置が入っているか + LMB クリック。drag 中はマウス x → time へ
//     逆変換して keyframe.time_sec を上書き。
//   ・**選択 keyframe index は i32 (-1 = 未選択)**: `kNoKeySelected` を sentinel。
//     AAnimCurveEditorPanel と同形。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (AAnimCurveEditorPanel /
//     AParticleEditorPanel) と同形。
//
// 範囲外:
//   ・実 file save/load
//   ・複数 director の同時編集
//   ・undo / redo 統合 (= CUndoStack 経由、OnUndo / OnRedo hook)
//   ・keyframe の補間 / curve 編集 (= AnimCurveEditor と統合した時点で検討)
//   ・track の add/remove (= 現状 5 種固定)
//   ・marker 複数選択 + 一括 drag
//   ・スナップグリッド / 等間隔配置 (= 現状は自由配置のみ)
//   ・preview viewport (= scrub したら 3D scene が更新されるリアルタイム連動)
//
// 関連実装: gameframework/CinematicsDirector.h,
//      gameframework/tools/editor_core/EditorPanel.h,
//      gameframework/tools/animcurve/AnimCurveEditorPanel.h
#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "math/Vec.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

// 編集対象の CCinematicsDirector は Forward.h から forward-decl のみで受ける。
// `<gameframework/CinematicsDirector.h>` を include しないことで、本 panel を
// 利用する側がヘッダ依存を最小化できる (= CCinematicsDirector 自体の変更で
// 不要な再ビルドを避ける)。

namespace acs::game::cinetimeline {

/**
 * 編集 UI 上で扱う 5 種の演出概念。
 *
 * @details
 * CCinematicsDirector::ETimelineTrackKind (runtime 側 5 種) とは概念を意図的に
 * 分離し、オーサリング側で典型的に並べたい要素を素直に表現する。Play 時に
 * ToTrackKind() で runtime kind へマッピングしたうえで director に bake する。
 */
enum class ETimelineKeyKind : u8 {
    /** カメラ位置を瞬時に切り替える (target FVec3 を使う)。 */
    CameraCut       = 0,

    /** 画面全体を start_color から end_color へフェードする。 */
    FadeColor       = 1,

    /** ゲーム時間スケールを倍率指定する (1.0 = 等倍、0.5 = スロー)。 */
    TimeScale       = 2,

    /** エフェクトを発火する (effect_id + position を使う)。 */
    SpawnEffect     = 3,

    /** 汎用 callback を発火する (event_id をユーザ定義で解釈)。 */
    TriggerCallback = 4,
};

/**
 * panel 内部で保持する keyframe (リッチ payload 付き)。
 *
 * @details
 * CCinematicsDirector::FTimelineKeyframe より広い payload を持つ。Play 時に
 * director へ AddKeyframe する際は kind に応じた FTimelineKeyframe へ変換する。
 * payload フィールドは active な kind に対応するものだけが意味を持つ
 * (CameraCut=camera_target、FadeColor=fade_start_color/fade_end_color、
 * TimeScale=time_scale、SpawnEffect=event_id+camera_target、
 * TriggerCallback=event_id)。
 */
struct FEditorKeyframe {
    /** タイムライン上の発火時刻 [秒]。 */
    f32              time_sec = 0.0f;

    /** この keyframe の演出種別。 */
    ETimelineKeyKind kind     = ETimelineKeyKind::TriggerCallback;

    /** CameraCut のカメラ移動先 / SpawnEffect の発生位置。 */
    FVec3 camera_target  { 0.0f, 0.0f, 0.0f };

    /** FadeColor のフェード開始色 (r,g,b in [0,1])。 */
    FVec3 fade_start_color { 0.0f, 0.0f, 0.0f };

    /** FadeColor のフェード終了色 (r,g,b in [0,1])。 */
    FVec3 fade_end_color   { 1.0f, 1.0f, 1.0f };

    /** TimeScale の時間スケール倍率。 */
    f32  time_scale       { 1.0f };

    /** SpawnEffect の effect_id / TriggerCallback の event_id。 */
    u32  event_id         { 0u };

    /** 既定値で keyframe を構築する。 */
    FEditorKeyframe() noexcept = default;
};

/**
 * CCinematicsDirector を対話的に編集する timeline editor panel。
 *
 * @details
 * 水平タイムライン + 5 種のトラック + キーフレームマーカー + 再生制御を持つ
 * 最小の cutscene エディタ。AEditorPanel を継承し、editor 専用のリッチな
 * keyframe storage (FEditorKeyframe の TArray) を panel 側で所有する。Play 時に
 * 各 keyframe を runtime 用 FTimelineKeyframe へ bake して director に流し込む。
 * 編集対象の CCinematicsDirector は raw 参照で受け取り、寿命は caller 責任。
 */
class ACinematicsTimelineEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    /** 空状態で構築する (内部 state は Init で初期化)。 */
    ACinematicsTimelineEditorPanel() noexcept = default;

    /** 派生破棄に備えた仮想デストラクタ (リソースは非所有)。 */
    ~ACinematicsTimelineEditorPanel() noexcept override = default;

    /** コピー禁止 (keyframe 配列・selection・director 参照を単独所有するため)。 */
    ACinematicsTimelineEditorPanel(const ACinematicsTimelineEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    ACinematicsTimelineEditorPanel& operator=(const ACinematicsTimelineEditorPanel&) = delete;

    /** ムーブ禁止 (基底 AEditorPanel と同規約)。 */
    ACinematicsTimelineEditorPanel(ACinematicsTimelineEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ACinematicsTimelineEditorPanel& operator=(ACinematicsTimelineEditorPanel&&)      = delete;

    /**
     * 内部 state を既定にリセットする (多重呼び出し可 = 完全リセット)。
     *
     * @details
     * director 参照を nullptr、selection を未選択、duration を default (10s)、
     * keyframes を空に戻す。
     */
    void Init() noexcept;

    /**
     * 内部 state を全解放する (多重呼び出し可)。
     *
     * @details
     * director 参照 / selection / keyframes を解除する。director 自体は caller
     * 所有なので本 panel は破棄しない。
     */
    void Shutdown() noexcept;

    /**
     * 編集対象の CCinematicsDirector を raw 参照でセットする。
     *
     * @details
     * 寿命は caller 責任 (本 panel は director を所有しない)。セット直後に
     * selection をリセット、m_CurrentTime を 0 に戻し、editor の現状を即時 bake する。
     * @param dir 編集対象の director (nullptr で解除)。
     */
    void SetCinematicsDirector(acs::game::CCinematicsDirector* dir) noexcept;

    /**
     * 現在編集対象の CCinematicsDirector を返す。
     *
     * @return バインド中の director (未バインド時は nullptr)。
     */
    acs::game::CCinematicsDirector* CurrentDirector() const noexcept;

    /**
     * 頭から再生を開始する。
     *
     * @details
     * editor の現状を director に bake してから、director を Stop → Play して
     * 先頭から再生する (途中位置開始 API が無いため m_CurrentTime も 0 に戻す)。
     */
    void Play() noexcept;

    /** 再生を一時停止する (m_CurrentTime は保持、director も Pause する)。 */
    void Pause() noexcept;

    /** 再生を完全停止する (m_CurrentTime を 0 に戻し、director も Stop する)。 */
    void Stop() noexcept;

    /**
     * 再生中のみ時間を dt 秒進める。
     *
     * @details
     * m_Playing == true のときだけ動作し、director と同期する加算が有限でない場合は
     * panel と director を変更しない (scrub は slider 直接編集で行う)。
     * director があれば Tick(dt) して m_CurrentTime を director.CurrentTime() と
     * 同期し、無ければ m_CurrentTime に dt を加算する。duration に達したら再生終了。
     * 非有限値と加算結果が非有限になる値は状態を変更しない。
     * @param dt 進める秒 (0 以下なら no-op)。
     */
    void Step(f32 dt) noexcept;

    /**
     * 再生中かを返す。
     *
     * @return 再生中なら true。
     */
    bool IsPlaying() const noexcept;

    /**
     * 現在のタイムカーソル位置を返す。
     *
     * @return 現在時刻 [秒]。
     */
    f32  CurrentTimeSec() const noexcept;

    /**
     * タイムカーソル位置を設定する。
     *
     * @param t 設定する時刻 (有限値は [0, Duration] にクランプ、非有限値は無視)。
     */
    void SetCurrentTimeSec(f32 t) noexcept;

    /**
     * タイムライン全体の長さを返す。
     *
     * @return Duration [秒]。
     */
    f32  DurationSec() const noexcept;

    /**
     * タイムライン全体の長さを設定する。
     *
     * @details
     * kMinDurationSec を下回る値は丸める。Duration が縮んで範囲外に出た既存
     * keyframe と m_CurrentTime は新 Duration にクランプする。
     * 非有限値は状態を変更しない。
     * @param d 設定する長さ [秒]。
     */
    void SetDurationSec(f32 d) noexcept;

    /**
     * 現在選択中の keyframe index を返す。
     *
     * @return 選択中 index (= 内部 TArray の index)。未選択は kNoKeySelected。
     */
    i32  SelectedKeyframeIndex() const noexcept;

    /**
     * selection を変更する。
     *
     * @param i 選択する index (有効範囲外なら kNoKeySelected に丸める)。
     */
    void SelectKeyframe(i32 i) noexcept;

    /**
     * 新規 keyframe を追加する。
     *
     * @details
     * time_sec を [0, Duration] にクランプして time 昇順を保ったまま挿入し、
     * 追加した keyframe を selection にする。挿入後 director に即時 bake する。
     * 非有限時刻は panel と director を変更しない。
     * @param kind 追加する keyframe の演出種別。
     * @param time_sec 発火時刻 [秒] (有限値のみ受け付ける)。
     */
    void AddKeyframe(ETimelineKeyKind kind, f32 time_sec) noexcept;

    /**
     * 選択中の keyframe を削除する (selection が無効なら no-op)。
     *
     * @details 順序保存削除し、selection を解除したうえで director に即時 bake する。
     */
    void RemoveSelectedKeyframe() noexcept;

    /**
     * window タイトルを返す (ImGui::Begin の引数兼 ID)。
     *
     * @return 固定リテラル "Cinematics Timeline"。
     */
    const char* Title() const noexcept override { return "Cinematics Timeline"; }

    /** Toolbar + Timeline canvas + Inspector + Ruler を ImGui で描画する。 */
    void DrawUI() noexcept override;

    /** 「未選択」を表す sentinel (i32 戻り値で使用)。 */
    static constexpr i32 kNoKeySelected = -1;

    /** タイムライン全 track の数 (= ETimelineKeyKind の要素数)。 */
    static constexpr u32 kTrackCount = 5u;

    /** 1 トラックの行高さ (px)。5 トラック分の縦サイズの基準。 */
    static constexpr f32 kTrackRowHeightPx = 28.0f;

    /** marker の幅 (px)。 */
    static constexpr f32 kMarkerWidthPx     = 10.0f;

    /** marker の AABB hit-test に加える余裕 (px)。 */
    static constexpr f32 kMarkerHitSlackPx  = 3.0f;

    /** Duration の最低値 (秒)。0 に近づくとタイム軸が破綻するため。 */
    static constexpr f32 kMinDurationSec = 0.1f;

    /** Duration のデフォルト値 (秒)。 */
    static constexpr f32 kDefaultDurationSec = 10.0f;

private:
    /** 編集対象 CCinematicsDirector (caller 所有、本 panel は非所有)。 */
    acs::game::CCinematicsDirector* m_Director = nullptr;

    /** panel 所有の keyframe 配列 (time 昇順、stable insertion で維持)。 */
    TArray<FEditorKeyframe> m_Keyframes;

    /** 現在選択中の keyframe index。 */
    i32 m_SelectedIdx = kNoKeySelected;

    /** タイムライン上の現在時刻 [秒] (slider / Step / Stop で更新)。 */
    f32 m_CurrentTime = 0.0f;

    /** タイムライン全体の長さ [秒] (default 10s)。 */
    f32 m_Duration = kDefaultDurationSec;

    /** 再生中フラグ (Play で true、Pause / Stop で false)。 */
    bool m_Playing = false;

    /** marker ドラッグ中フラグ (UI 専用 state)。 */
    bool m_bDraggingMarker = false;

    /** ドラッグ対象の keyframe index (未ドラッグ時は -1)。 */
    i32  m_DragIdx        = -1;

    /** Add combo で選択中の kind (+Add 押下時にこの kind で追加)。 */
    ETimelineKeyKind m_AddKind = ETimelineKeyKind::CameraCut;

    /**
     * time 昇順を保ったまま keyframe を挿入する。
     *
     * @param kf 挿入する keyframe。
     * @return 挿入された index (= sort 後の位置)。
     */
    i32 InsertKeyframeSorted(const FEditorKeyframe& kf) noexcept;

    /**
     * editor の全 keyframe を director に bake する (= Clear + AddKeyframe loop)。
     *
     * @details SetCinematicsDirector / Play / 各編集操作で呼ぶ。director が nullptr なら no-op。
     */
    void BakeToDirector() noexcept;
};

using FCinematicsTimelineEditorPanel = ACinematicsTimelineEditorPanel;

} // namespace acs::game::cinetimeline
