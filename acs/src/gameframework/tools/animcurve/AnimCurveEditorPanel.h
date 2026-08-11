// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"

namespace acs::game {
// 編集対象の FAnimationCurve は本ヘッダから forward-decl のみで受ける。
// `<gameframework/AnimationCurve.h>` を include しないことで、本 panel を
// 利用する側がヘッダ依存を最小化できる (= FAnimationCurve 自体の変更で
// 不要な再ビルドを避ける)。
class FAnimationCurve;
} // namespace acs::game

namespace acs::game::animcurve {

/**
 * FAnimationCurve を ImGui で対話的に編集する curve editor panel。
 *
 * @details
 * キー追加とタンジェントドラッグを扱う。editor_core::AEditorPanel を継承し、
 * CEditorWorkspace::RegisterPanel(&panel) で workspace に統合できる。
 * canvas 上に curve を kCurveSampleCount sample で線描画し、各 key を丸 marker、
 * Hermite key の in/out tangent を handle として描画して drag 編集できる。
 * 編集対象の FAnimationCurve は caller 所有 (SetCurve で raw 参照を渡す、寿命は
 * caller 責任) で、本 panel は curve を生成・破棄しない。全 noexcept・非コピー・
 * 非ムーブ・STL 不使用で、ImGui 依存は .cpp に閉じる。
 */
class AAnimCurveEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    /**
     * curve に変更があった時に呼ばれる callback 型 (raw 関数ポインタ + void* user)。
     *
     * @details
     * ACS は std::function 禁止のため C-style callback 規約 (AParticleEditorPanel /
     * CAssetBrowser と同形)。第 1 引数 user は SetOnChangeCallback に渡した不透明
     * ポインタ、第 2 引数 curve は編集中の FAnimationCurve。キー追加 / 削除 /
     * interp 変更 / wrap mode 変更は即時 1 回発火し、drag は連続発火を避けて
     * drag end (= マウス release) で 1 度だけ発火する。
     */
    using CurveChangeCallback =
        void (*)(void* user, acs::game::FAnimationCurve* curve) noexcept;

    /** curve 未バインドの空状態で構築する。 */
    AAnimCurveEditorPanel() noexcept = default;

    /** 破棄する (curve は caller 所有なので何も解放しない)。 */
    ~AAnimCurveEditorPanel() noexcept override = default;

    /** コピー禁止 (基底 AEditorPanel と同規約)。 */
    AAnimCurveEditorPanel(const AAnimCurveEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AAnimCurveEditorPanel& operator=(const AAnimCurveEditorPanel&) = delete;

    /** ムーブ禁止。 */
    AAnimCurveEditorPanel(AAnimCurveEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AAnimCurveEditorPanel& operator=(AAnimCurveEditorPanel&&)      = delete;

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
     * @return 固定リテラル "Animation Curve Editor"。
     */
    const char* Title() const noexcept override { return "Animation Curve Editor"; }

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

    /** toolbar で選択中の Easing::EEasingType の安定した u8 値。 */
    u8 m_SelectedEasingPreset = 0u;

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

using FAnimCurveEditorPanel = AAnimCurveEditorPanel;

} // namespace acs::game::animcurve
