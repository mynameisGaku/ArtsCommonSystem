// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — animcurve / FAnimCurveEditorPanel
//
// `acs::game::FAnimationCurve` (Hermite/Linear/Step + WrapMode) を ImGui で
// **対話的に編集する curve editor panel**。Unity FAnimationCurve エディタ /
// Unreal CurveEditor / Godot Curve のキー打ち + タンジェントドラッグの
// 簡易版に相当。`editor_core::FEditorPanel` 基底に
// 載せ、`FEditorWorkspace::RegisterPanel(&panel)` の 1 行で workspace に
// 統合できる形にしている。
//
// 役割:
//   ・ImGui canvas 上に curve の波形を 1024 sample で描画 (= 連続線)
//   ・各 key を丸 marker で描画 + drag で time / value を編集
//   ・Hermite key の場合は in / out tangent を小さい handle として描画
//     し、これも drag で編集可能 (= 接線方向の調整)
//   ・右クリックで context menu (Add key here / Delete selected key)
//   ・上部 toolbar:
//       - Interpolation Combo (Step / Linear / Hermite) — 選択中 key に適用
//       - WrapMode Combo (Pre / Post 個別) — curve 全体に適用
//       - Add Key ボタン (= time=0.5 / value=0 の標準キーを 1 個追加)
//       - Clear ボタン (= 全 key 削除)
//       - Eval preview slider (= 現在時刻 [0, Duration] で曲線を sample し、
//         結果値を表示。アニメ確認用)
//   ・「キー変更があった」フラグ `m_Dirty` を内部に保持し、`CurveChangeCallback`
//     経由で外部 (= caller) に通知 (例: 保存ボタンの有効化 / 自動再描画)
//
// 役割分担:
//   ・本 panel は「**curve の編集 UI** だけ」を担当。実際の FAnimationCurve
//     データは caller 所有 (= `SetCurve(&curve)` で raw 参照を渡す、寿命は
//     caller 責任)。本 panel が curve を生成 / 破棄しない。
//   ・curve の使い道 (= camera FOV カーブ / fade alpha / 任意 easing 等) は
//     呼出側責任。本 panel は curve に対して `AddKey / RemoveKey / Evaluate`
//     等の API を呼ぶだけで、curve の利用文脈には関与しない。
//
// 使い方 (典型):
//   FAnimCurveEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);   // FEditorPanel として登録
//
//   FAnimationCurve my_curve;
//   my_curve.AddKeyHermite(0.0f, 0.0f, 0.0f, 1.0f);
//   my_curve.AddKeyHermite(0.5f, 1.0f, 0.0f, 0.0f);
//   my_curve.AddKeyHermite(1.0f, 0.0f, -1.0f, 0.0f);
//   panel.SetCurve(&my_curve);
//
//   panel.SetOnChangeCallback([](void* user, FAnimationCurve* c) noexcept {
//       // ファイルを autosave、再生中なら再 evaluate、等
//       (void)user; (void)c;
//   }, nullptr);
//
//   // 毎フレーム TickAllPanels(dt) の中で OnFrameBegin + DrawUI が呼ばれる。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel);
//   panel.Shutdown();
//
// 設計選択:
//   ・**FEditorPanel 継承**: 共通基盤を利用する。Title = "FAnimation Curve Editor"、DrawUI override。
//   ・**curve は raw pointer の非所有保持**: caller が own する設計
//     (FParticleEditorPanel が FParticleEffectSystem を参照渡しで受けるのと
//     同じ方針)。本 panel は curve の寿命に関与せず、`m_Curve == nullptr` 時は
//     「(No curve bound)」を表示。
//   ・**canvas は ImGui::InvisibleButton + GetWindowDrawList()**: ImGui の
//     標準パターン (Demo の Canvas example と同形)。ButtonBehavior で hover /
//     held を取り、ChannelsSplit で背景→曲線→key marker の z-order を整える。
//   ・**1024 sample 線描画**: curve は 1 ピクセル粒度なら 1024 で十分滑らか
//     (典型の curve editor 横幅 ~1000px 想定)。それ以上は SIMD でも誤差
//     範囲なので overkill。
//   ・**選択中 key index は i32 (-1 = 未選択)**: `kNoKeySelected` を sentinel に
//     する (FModelAnimationPanel の `kNoClipSelected` と同形)。
//   ・**Tangent handle の長さは固定 px (= 約 30px)**: curve 形状で接線が
//     画面外に飛ぶ事故を避けるため。実際の tangent 値 (= dy/dx) は curve に
//     書き込むが、handle 描画位置だけは固定スケールで「短い棒」として出す。
//   ・**右クリック menu は ImGui::OpenPopup + BeginPopup**: ImGui 標準。
//     "Add Key Here" は click 位置の (time, value) を decode してそこに key
//     追加。"Delete Selected" は `m_SelectedKeyIdx` が有効なら RemoveKey。
//   ・**CurveChangeCallback は raw 関数ポインタ + void* user**: ACS は
//     std::function 禁止。FParticleEditorPanel / FAssetBrowser と同形の C-style
//     callback 規約 (FModelAnimationPanel の AnimationFrameCallback と同形)。
//     キー操作の都度発火する設計だが、drag 中は連続発火を避けるため
//     「drag end (= マウス release)」のタイミングで 1 度だけ呼ぶ。
//   ・**Toolbar の WrapMode Combo は Pre / Post 個別**: FAnimationCurve API も
//     SetPreWrap / SetPostWrap が分かれているので、UI もそれに合わせる。
//   ・**Eval preview slider [0, Duration]**: 単純な可視化用 read-only 数値表示。
//     curve.Duration() == 0 の場合は slider を disable して 0 表示。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (FParticleEditorPanel /
//     FModelViewerPanel / FModelInspectorPanel) と同形。
//
// 範囲外:
//   ・複数 curve の同時編集 / レイヤ重ね (= timeline editor の役割)
//   ・curve preset library (= ease-in-out / bounce 等のプリセットボタン)
//   ・undo / redo 統合
//   ・curve のシリアライズ (= 現状は caller が独自に保存する想定)
//   ・key 複数選択 + 一括 drag (= 現状は単一選択のみ)
//   ・スナップグリッド / 等間隔配置 (= 現状は自由配置のみ)
//   ・curve loop preview の auto-play (= ボタン押下で AnimCurve 再生)
//   ・per-segment curvature 表示 (= Hermite の `s` パラメータ等)
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// 編集対象の FAnimationCurve は本ヘッダから forward-decl のみで受ける。
// `<gameframework/FAnimationCurve.h>` を include しないことで、本 panel を
// 利用する側がヘッダ依存を最小化できる (= FAnimationCurve 自体の変更で
// 不要な再ビルドを避ける)。
class FAnimationCurve;
} // namespace acs::game

namespace acs::game::animcurve {

/**
 * FAnimationCurve を ImGui で対話的に編集する curve editor panel。
 *
 * @details
 * Unity FAnimationCurve エディタ / Unreal CurveEditor 相当のキー打ち +
 * タンジェントドラッグの簡易版。editor_core::FEditorPanel を継承し、
 * FEditorWorkspace::RegisterPanel(&panel) で workspace に統合できる。
 * canvas 上に curve を kCurveSampleCount sample で線描画し、各 key を丸 marker、
 * Hermite key の in/out tangent を handle として描画して drag 編集できる。
 * 編集対象の FAnimationCurve は caller 所有 (SetCurve で raw 参照を渡す、寿命は
 * caller 責任) で、本 panel は curve を生成・破棄しない。全 noexcept・非コピー・
 * 非ムーブ・STL 不使用で、ImGui 依存は .cpp に閉じる。
 */
class FAnimCurveEditorPanel : public acs::game::editor_core::FEditorPanel {
public:
    /**
     * curve に変更があった時に呼ばれる callback 型 (raw 関数ポインタ + void* user)。
     *
     * @details
     * ACS は std::function 禁止のため C-style callback 規約 (FParticleEditorPanel /
     * FAssetBrowser と同形)。第 1 引数 user は SetOnChangeCallback に渡した不透明
     * ポインタ、第 2 引数 curve は編集中の FAnimationCurve。キー追加 / 削除 /
     * interp 変更 / wrap mode 変更は即時 1 回発火し、drag は連続発火を避けて
     * drag end (= マウス release) で 1 度だけ発火する。
     */
    using CurveChangeCallback =
        void (*)(void* user, acs::game::FAnimationCurve* curve) noexcept;

    /** curve 未バインドの空状態で構築する。 */
    FAnimCurveEditorPanel() noexcept = default;

    /** 破棄する (curve は caller 所有なので何も解放しない)。 */
    ~FAnimCurveEditorPanel() noexcept override = default;

    /** コピー禁止 (基底 FEditorPanel と同規約)。 */
    FAnimCurveEditorPanel(const FAnimCurveEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    FAnimCurveEditorPanel& operator=(const FAnimCurveEditorPanel&) = delete;

    /** ムーブ禁止。 */
    FAnimCurveEditorPanel(FAnimCurveEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAnimCurveEditorPanel& operator=(FAnimCurveEditorPanel&&)      = delete;

    /**
     * 内部 state を初期値に戻す (curve 参照 / selection / dirty / callback を全クリア)。
     *
     * @details curve 参照を nullptr、selection を未選択、dirty を false、callback を
     * nullptr にする。多重 Init 可 (= 完全リセット)。
     */
    void Init() noexcept;

    /**
     * 内部 state を全解放する (curve 参照 / selection / callback を解除)。
     *
     * @details curve 自体は caller 所有なので本 panel は破棄しない。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * 編集対象の FAnimationCurve を raw 参照でセットする (nullptr で解除)。
     *
     * @details 寿命は caller 責任 (本 panel は curve を所有しない)。セット直後は
     * selection と dirty をリセットする (別 curve に切替えると前 curve の
     * selection index は無意味になるため)。
     * @param curve バインドする FAnimationCurve (nullptr で解除)。
     */
    void SetCurve(acs::game::FAnimationCurve* curve) noexcept;

    /**
     * 現在編集対象の FAnimationCurve を返す。
     *
     * @return バインド中の curve (未バインド時は nullptr)。
     */
    acs::game::FAnimationCurve* CurrentCurve() const noexcept;

    /**
     * 最後の ClearDirty() 以降に curve への編集が適用されたかを返す。
     *
     * @details caller が「Save ボタン enable / 自動保存タイマ起動」等の判定に使う。
     * @return 編集があれば true。
     */
    bool IsDirty() const noexcept;

    /**
     * dirty flag を false に戻す。
     *
     * @details caller が curve を保存した直後に呼ぶ想定。
     */
    void ClearDirty() noexcept;

    /**
     * curve 変更時に呼ばれる callback を登録する (nullptr で解除)。
     *
     * @details ライフサイクルは caller 責任 (panel 破棄前に必ず解除 or 整合性確保)。
     * @param cb 変更通知 callback (nullptr で解除)。
     * @param user cb の第 1 引数にそのまま渡る不透明ポインタ。
     */
    void SetOnChangeCallback(CurveChangeCallback cb, void* user) noexcept;

    /**
     * window タイトルを返す (ImGui::Begin の引数兼 ID)。
     *
     * @return 固定リテラル "FAnimation Curve Editor"。
     */
    const char* Title() const noexcept override { return "FAnimation Curve Editor"; }

    /**
     * Toolbar + Canvas を ImGui で描画する。
     *
     * @details curve 未バインドなら "(No curve bound)" を表示してセクションは出さない。
     */
    void DrawUI() noexcept override;

    /** canvas 上で curve をサンプルする点数 (= 線描画の解像度)。 */
    static constexpr u32 kCurveSampleCount = 1024u;

    /** 「未選択」を表す sentinel (i32 の key index で使用)。 */
    static constexpr i32 kNoKeySelected = -1;

    /** 接線 handle 描画用の固定 px 長 (handle が画面外に飛ぶのを防ぐ。tangent 値とは無関係)。 */
    static constexpr f32 kTangentHandleLengthPx = 30.0f;

    /** key marker の半径 (px)。クリック判定にも使う (= radius + 数 px 余裕)。 */
    static constexpr f32 kKeyMarkerRadiusPx = 5.0f;

private:
    /** 編集対象 FAnimationCurve (caller 所有、本 panel は非所有)。 */
    acs::game::FAnimationCurve* m_Curve = nullptr;

    /** 現在選択中の key index (m_Curve->KeyCount() 未満、未選択は kNoKeySelected)。 */
    i32 m_SelectedKeyIdx = kNoKeySelected;

    /** 最後の ClearDirty() 以降に編集があったか。 */
    bool m_Dirty = false;

    /** ドラッグ種別 (0=なし / 1=key 本体 / 2=in-tangent handle / 3=out-tangent handle)。 */
    u8 m_DragKind = 0u;

    /** drag 中の対象 key index (m_DragKind != 0 の時のみ有効)。 */
    i32 m_DragKeyIdx = -1;

    /** 変更通知 callback (未登録なら nullptr)。 */
    CurveChangeCallback m_OnChangeCb   = nullptr;

    /** callback の第 1 引数に渡す不透明ユーザポインタ。 */
    void*               m_OnChangeUser = nullptr;

    /**
     * dirty を立て、必要なら callback を呼ぶ内部ヘルパ。
     *
     * @param immediate true なら即時に callback 発火 (key 追加 / 削除 / interp 変更等)、
     *                  false なら dirty のみ立てる (drag 中の連続変更。callback は
     *                  drag end で別途発火する)。
     */
    void NotifyChanged(bool immediate) noexcept;
};

} // namespace acs::game::animcurve
