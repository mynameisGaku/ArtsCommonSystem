// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — animcurve / AnimCurveEditorPanel (Phase 22)
//
// `acs::game::AnimationCurve` (Hermite/Linear/Step + WrapMode) を ImGui で
// **対話的に編集する curve editor panel**。Unity AnimationCurve エディタ /
// Unreal CurveEditor / Godot Curve のキー打ち + タンジェントドラッグの
// 簡易版に相当。Phase 21a で確立された `editor_core::EditorPanel` 基底に
// 載せ、`EditorWorkspace::RegisterPanel(&panel)` の 1 行で workspace に
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
//   ・「キー変更があった」フラグ `_dirty` を内部に保持し、`CurveChangeCallback`
//     経由で外部 (= caller) に通知 (例: 保存ボタンの有効化 / 自動再描画)
//
// 役割分担:
//   ・本 panel は「**curve の編集 UI** だけ」を担当。実際の AnimationCurve
//     データは caller 所有 (= `SetCurve(&curve)` で raw 参照を渡す、寿命は
//     caller 責任)。本 panel が curve を生成 / 破棄しない。
//   ・curve の使い道 (= camera FOV カーブ / fade alpha / 任意 easing 等) は
//     呼出側責任。本 panel は curve に対して `AddKey / RemoveKey / Evaluate`
//     等の API を呼ぶだけで、curve の利用文脈には関与しない。
//
// 使い方 (典型):
//   AnimCurveEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);   // EditorPanel として登録
//
//   AnimationCurve my_curve;
//   my_curve.AddKeyHermite(0.0f, 0.0f, 0.0f, 1.0f);
//   my_curve.AddKeyHermite(0.5f, 1.0f, 0.0f, 0.0f);
//   my_curve.AddKeyHermite(1.0f, 0.0f, -1.0f, 0.0f);
//   panel.SetCurve(&my_curve);
//
//   panel.SetOnChangeCallback([](void* user, AnimationCurve* c) noexcept {
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
// 設計選択 (Phase 22 AnimCurveEditor):
//   ・**EditorPanel 継承**: Phase 21a 共通基盤の dogfood (sample 31 ModelViewer
//     に続く 2 つ目)。Title = "Animation Curve Editor"、DrawUI override。
//   ・**curve は raw pointer の非所有保持**: caller が own する設計
//     (ParticleEditorPanel が ParticleEffectSystem を参照渡しで受けるのと
//     同じ方針)。本 panel は curve の寿命に関与せず、`_curve == nullptr` 時は
//     「(No curve bound)」を表示。
//   ・**canvas は ImGui::InvisibleButton + GetWindowDrawList()**: ImGui の
//     標準パターン (Demo の Canvas example と同形)。ButtonBehavior で hover /
//     held を取り、ChannelsSplit で背景→曲線→key marker の z-order を整える。
//   ・**1024 sample 線描画**: curve は 1 ピクセル粒度なら 1024 で十分滑らか
//     (典型の curve editor 横幅 ~1000px 想定)。それ以上は SIMD でも誤差
//     範囲なので overkill (Phase 23 で sample 数を canvas 横幅から自動算出
//     する case 検討)。
//   ・**選択中 key index は i32 (-1 = 未選択)**: `kNoKeySelected` を sentinel に
//     する (ModelAnimationPanel の `kNoClipSelected` と同形)。
//   ・**Tangent handle の長さは固定 px (= 約 30px)**: curve 形状で接線が
//     画面外に飛ぶ事故を避けるため。実際の tangent 値 (= dy/dx) は curve に
//     書き込むが、handle 描画位置だけは固定スケールで「短い棒」として出す。
//   ・**右クリック menu は ImGui::OpenPopup + BeginPopup**: ImGui 標準。
//     "Add Key Here" は click 位置の (time, value) を decode してそこに key
//     追加。"Delete Selected" は `_selected_key_idx` が有効なら RemoveKey。
//   ・**CurveChangeCallback は raw 関数ポインタ + void* user**: ACS は
//     std::function 禁止。ParticleEditorPanel / AssetBrowser と同形の C-style
//     callback 規約 (ModelAnimationPanel の AnimationFrameCallback と同形)。
//     キー操作の都度発火する設計だが、drag 中は連続発火を避けるため
//     「drag end (= マウス release)」のタイミングで 1 度だけ呼ぶ。
//   ・**Toolbar の WrapMode Combo は Pre / Post 個別**: AnimationCurve API も
//     SetPreWrap / SetPostWrap が分かれているので、UI もそれに合わせる。
//   ・**Eval preview slider [0, Duration]**: 単純な可視化用 read-only 数値表示。
//     curve.Duration() == 0 の場合は slider を disable して 0 表示。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (ParticleEditorPanel /
//     ModelViewerPanel / ModelInspectorPanel) と同形。
//
// 範囲外 (Phase 22 では持たない、将来追加候補):
//   ・複数 curve の同時編集 / レイヤ重ね (= timeline editor の役割)
//   ・curve preset library (= ease-in-out / bounce 等のプリセットボタン)
//   ・undo / redo 統合 (= Phase 21b UndoStack 経由、将来 OnUndo / OnRedo hook)
//   ・curve のシリアライズ (= AnimationCurve 自体に Save/Load を追加する
//     Phase 23 で検討、現状は caller が独自に保存する想定)
//   ・key 複数選択 + 一括 drag (= 現状は単一選択のみ)
//   ・スナップグリッド / 等間隔配置 (= 現状は自由配置のみ)
//   ・curve loop preview の auto-play (= ボタン押下で AnimCurve 再生)
//   ・per-segment curvature 表示 (= Hermite の `s` パラメータ等)
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// 編集対象の AnimationCurve は本ヘッダから forward-decl のみで受ける。
// `<gameframework/AnimationCurve.h>` を include しないことで、本 panel を
// 利用する側がヘッダ依存を最小化できる (= AnimationCurve 自体の変更で
// 不要な再ビルドを避ける)。
class AnimationCurve;
} // namespace acs::game

namespace acs::game::animcurve {

// ---------------------------------------------------------------------------
// AnimCurveEditorPanel — AnimationCurve 編集 UI panel
// ---------------------------------------------------------------------------
class AnimCurveEditorPanel : public acs::game::editor_core::EditorPanel {
public:
    // curve に変更があった時に呼ばれる callback (raw 関数ポインタ + void* user)。
    // ParticleEditorPanel / AssetBrowser と同形の C-style callback 規約
    // (ACS は std::function 禁止)。
    //   user  : SetOnChangeCallback に渡した不透明ポインタ
    //   curve : 変更された AnimationCurve (= 本 panel が編集中の curve)
    // drag 中は連続発火を避け、drag end (= マウス release) で 1 度だけ呼ぶ規約。
    // 「キー追加 / 削除 / interp 変更 / wrap mode 変更」は即時 1 回発火。
    using CurveChangeCallback =
        void (*)(void* user, acs::game::AnimationCurve* curve) noexcept;

    AnimCurveEditorPanel() noexcept = default;
    ~AnimCurveEditorPanel() noexcept override = default;

    // 非コピー / 非ムーブ: 基底 EditorPanel と同規約 + 内部の curve 参照 /
    // selection / callback を曖昧にしない (ACS 規約)。
    AnimCurveEditorPanel(const AnimCurveEditorPanel&)            = delete;
    AnimCurveEditorPanel& operator=(const AnimCurveEditorPanel&) = delete;
    AnimCurveEditorPanel(AnimCurveEditorPanel&&)                 = delete;
    AnimCurveEditorPanel& operator=(AnimCurveEditorPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、curve 参照を nullptr、selection を未選択、
    // dirty=false、callback を nullptr に。多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を全解放 (= curve 参照 / selection / callback を解除)。
    // curve 自体は caller 所有なので本 panel が破棄しない。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- curve バインド --------------------------------------------------

    // 編集対象の AnimationCurve を raw 参照でセット (nullptr で解除)。
    // 寿命は caller 責任 (本 panel は curve を所有しない)。
    // セット直後は selection をリセット (= 別 curve に切替えたら前 curve の
    // selection index は無意味になる)、dirty もリセット。
    void SetCurve(acs::game::AnimationCurve* curve) noexcept;

    // 現在編集対象の AnimationCurve (未バインド時は nullptr)。
    acs::game::AnimationCurve* CurrentCurve() const noexcept;

    // ----- dirty flag (外部に「変更があったよ」通知用) ---------------------

    // 最後に ClearDirty() を呼んでから今までに、curve に何らかの編集が
    // 適用されたか。caller が「Save ボタン enable / 自動保存タイマ起動」等の
    // 判定に使う。
    bool IsDirty() const noexcept;

    // dirty flag を false に戻す。caller が curve を保存した直後に呼ぶ想定。
    void ClearDirty() noexcept;

    // ----- callback --------------------------------------------------------

    // curve 変更時に呼ばれる callback を登録 (nullptr で解除)。
    //   cb   : `void (*)(void* user, AnimationCurve* curve) noexcept`
    //   user : 任意の不透明ポインタ (cb 第 1 引数にそのまま渡る)
    // ライフサイクルは caller 責任 (= panel 破棄前に必ず解除 or 整合性確保)。
    void SetOnChangeCallback(CurveChangeCallback cb, void* user) noexcept;

    // ----- EditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "Animation Curve Editor"; }

    // ImGui::Begin "Animation Curve Editor" + Toolbar + Canvas を描画。
    // curve 未バインドなら "(No curve bound)" を表示してセクションは出さない。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // canvas 上で curve をサンプルする点数 (= 線描画の解像度)。
    // 典型の curve editor 横幅 ~1000px に対して 1024 で十分滑らか。
    static constexpr u32 kCurveSampleCount = 1024u;

    // 「未選択」を表す sentinel (i32 戻り値で使用)。
    static constexpr i32 kNoKeySelected = -1;

    // 接線 handle 描画用の固定 px 長 (curve 形状で handle が画面外に飛ぶのを
    // 防ぐため、UI 上の長さは固定にする)。実際の tangent 値とは無関係。
    static constexpr f32 kTangentHandleLengthPx = 30.0f;

    // key marker の半径 (px)。クリック判定にも使う (= radius + 数 px 余裕)。
    static constexpr f32 kKeyMarkerRadiusPx = 5.0f;

private:
    // 編集対象 AnimationCurve (caller 所有、本 panel は非所有)。
    acs::game::AnimationCurve* _curve = nullptr;

    // 現在選択中の key index (`_curve->KeyCount()` 未満、未選択は kNoKeySelected)。
    i32 _selected_key_idx = kNoKeySelected;

    // 最後の ClearDirty() 以降に編集があったか。
    bool _dirty = false;

    // ドラッグ中 (= マウス左ボタン押下中) フラグ。drag end で callback 1 回発火。
    // 0=なし / 1=key 本体 / 2=in-tangent handle / 3=out-tangent handle。
    u8 _drag_kind = 0u;

    // drag 中の対象 key index (drag_kind != 0 の時のみ有効)。
    i32 _drag_key_idx = -1;

    // 変更通知 callback。
    CurveChangeCallback _on_change_cb   = nullptr;
    void*               _on_change_user = nullptr;

    // dirty を true にし、callback を呼ぶ内部ヘルパ。
    // immediate=true の場合は即時 callback (key 追加 / 削除 / interp 変更等)、
    // false の場合は drag 中の連続変更を表す (= callback は drag end で発火)。
    void NotifyChanged(bool immediate) noexcept;
};

} // namespace acs::game::animcurve
