// SPDX-License-Identifier: Apache-2.0
// CFadeTransition — シーン切替フェード演出の state holder
//
// シーン切替時のフェード演出を司る **state holder**。描画自体は行わず、
// 現在の overlay alpha / color と phase だけを提供する。ユーザー側で
// CSpriteBatch を使い fullscreen quad を「色 = OverlayColor()、alpha =
// OverlayAlpha()」で描くだけで、典型的なフェードイン・アウト・ブラック
// フラッシュ・クロスフェードが成立する。
//
// 使い方:
//   class FTitleScene : public AScene {
//       acs::game::CFadeTransition m_Fade;
//       void OnEnter() noexcept override {
//           // 画面が黒から徐々に明けるフェードイン
//           m_Fade.StartFade(EFadeKind::FadeIn, /*out=*/0.0f, /*in=*/0.5f);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Fade.Tick(dt);
//           if (m_Fade.IsMidPause()) {
//               // FadeInOut の暗転中: ここでシーン切替
//               Manager().ChangeScene<FGameplayScene>();
//           }
//       }
//       void OnRender() noexcept override {
//           // ... 通常描画 ...
//           if (m_Fade.IsActive()) {
//               const acs::FVec3 c = m_Fade.OverlayColor();
//               const f32       a = m_Fade.OverlayAlpha();
//               CSpriteBatch().FillFullScreen(c.x, c.y, c.z, a);
//           }
//       }
//   };
//
// 設計選択:
//   ・**state-only**: ACS は描画 API を gameframework から直接叩かない方針
//     (Pillar Q/R が独立しているため)。fade は単に「いま画面上に黒幕が何 %
//     被さっているか」だけ返し、描画はユーザー責任。
//   ・**4 種類の Kind**:
//       - FadeIn   : alpha 1 → 0 (画面が暗黒から明けて見える)
//       - FadeOut  : alpha 0 → 1 (画面が暗黒に閉じる)
//       - FadeInOut: 0 → 1 → (mid pause) → 0 (定番の暗転シーン切替)
//       - CrossFade: 0 → 0.5 → 0 (alpha が 50% まで上がって戻る軽い演出。
//                    本格的なクロスフェードは 2 枚バッファ前提なので
//                    ここでは「軽量代用」と割り切る)
//   ・**phase**: Idle → FadingOut → MidPause → FadingIn → Idle で進行。
//     FadeIn のみ FadingOut/MidPause をスキップ、FadeOut のみ FadingIn を
//     スキップ、CrossFade は FadingOut+FadingIn のみ (mid_pause=0)。
//   ・**IsMidPause()** が true な間にシーン切替を行うのが想定タイミング。
//     mid_pause=0 でも 1 Tick だけ true を返すよう保証 (= 必ず捕まえられる)。
//   ・**duration=0** は「即時 phase 遷移」。0 除算を避けるため alpha 補間時に
//     0 ガードを入れる。
//   ・非コピー・非ムーブ。state holder なので所有権移転を許す意味がない。
//
// 範囲外:
//   ・実描画 (CSpriteBatch 呼び出し)
//   ・easing 曲線指定 (linear 固定。必要なら user 側で OverlayAlpha() を
//      Easing::InOutSine 等に通す)
//   ・複数 overlay の重ね合わせ
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * フェード演出の種類。
 */
enum class EFadeKind : u32 {
    /** フェードなし。 */
    None       = 0,

    /** 暗黒 → 明 (alpha 1→0)。 */
    FadeIn     = 1,

    /** 明 → 暗黒 (alpha 0→1)。 */
    FadeOut    = 2,

    /** 0→1→pause→0。mid pause 中にシーン切替する定番演出。 */
    FadeInOut  = 3,

    /** 0→0.5→0。クロスフェードの軽量代用。 */
    CrossFade  = 4,
};

/**
 * フェードの進行フェーズ。
 *
 * @details Idle → FadingOut → MidPause → FadingIn → Idle の順に進む。
 */
enum class EFadePhase : u32 {
    /** 停止中 (フェードしていない)。 */
    Idle       = 0,

    /** alpha 増加中 (画面が暗くなっていく)。 */
    FadingOut  = 1,

    /** 暗転待機 (シーン切替タイミング)。 */
    MidPause   = 2,

    /** alpha 減少中 (画面が明けていく)。 */
    FadingIn   = 3,
};

/**
 * シーン切替時のフェード演出を司る state holder。
 *
 * @details
 * 描画自体は行わず、現在の overlay alpha / color と phase だけを提供する。
 * ユーザー側で fullscreen quad を「色 = OverlayColor()、alpha = OverlayAlpha()」で
 * 描くことでフェードイン・アウト・クロスフェードが成立する。非コピー・非ムーブ。
 */
class CFadeTransition {
public:
    /** Idle 状態で構築する。 */
    CFadeTransition() noexcept = default;

    /** 破棄する。 */
    ~CFadeTransition() noexcept = default;

    /** コピー禁止 (state holder なので所有権移転に意味がないため)。 */
    CFadeTransition(const CFadeTransition&)            = delete;

    /** コピー代入も禁止。 */
    CFadeTransition& operator=(const CFadeTransition&) = delete;

    /** ムーブ禁止。 */
    CFadeTransition(CFadeTransition&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CFadeTransition& operator=(CFadeTransition&&)      = delete;

    /**
     * フェードを開始する。
     *
     * @details
     * kind に応じて初期 phase と alpha を設定する。FadeIn は FadingIn から、
     * それ以外は FadingOut から始まる。CrossFade では mid_pause は無視される。
     * @param kind 実行するフェードの種類。
     * @param out_duration FadingOut にかける秒数 (FadeIn のときは無視)。
     * @param in_duration FadingIn にかける秒数 (FadeOut のときは無視)。
     * @param mid_pause MidPause を保持する秒数。0 でも 1 Tick は MidPause を通る。
     */
    void StartFade(EFadeKind kind,
                   f32 out_duration = 0.3f,
                   f32 in_duration  = 0.3f,
                   f32 mid_pause    = 0.0f) noexcept;

    /**
     * フェードが進行中かを返す。
     *
     * @return phase が Idle 以外なら true。
     */
    bool      IsActive()      const noexcept { return m_Phase != EFadePhase::Idle; }

    /**
     * 暗転待機中 (MidPause) かを返す。
     *
     * @details この間にシーン切替を行うのが想定タイミング。
     * @return phase が MidPause なら true。
     */
    bool      IsMidPause()    const noexcept { return m_Phase == EFadePhase::MidPause; }

    /**
     * 現在の進行フェーズを返す。
     *
     * @return 現在の EFadePhase。
     */
    EFadePhase CurrentPhase()  const noexcept { return m_Phase; }

    /**
     * 現在実行中のフェード種類を返す。
     *
     * @return 現在の EFadeKind。
     */
    EFadeKind  CurrentKind()   const noexcept { return m_Kind; }

    /**
     * overlay の不透明度を返す。
     *
     * @return [0, 1] の overlay alpha。
     */
    f32  OverlayAlpha() const noexcept { return m_Alpha; }

    /**
     * overlay の色を返す。
     *
     * @return overlay 色 (既定 = black)。
     */
    FVec3 OverlayColor() const noexcept { return m_Color; }

    /**
     * overlay の色を設定する。
     *
     * @param c 新しい overlay 色。
     */
    void SetOverlayColor(FVec3 c) noexcept { m_Color = c; }

    /**
     * フェードを 1 フレーム進める。
     *
     * @details phase に応じて alpha を補間し、必要なら次 phase へ遷移する。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 直ちに Idle に戻す (alpha=0)。途中キャンセル用。
     */
    void Cancel() noexcept;

private:
    /** CrossFade のピーク alpha (0.5 = 画面が半透けまで暗くなって戻る)。 */
    static constexpr f32 kCrossFadePeak = 0.5f;

    /** 現在の進行フェーズ。 */
    EFadePhase m_Phase         = EFadePhase::Idle;

    /** 現在実行中のフェード種類。 */
    EFadeKind  m_Kind          = EFadeKind::None;

    /** 現在の overlay 不透明度 [0, 1]。 */
    f32  m_Alpha              = 0.0f;

    /** overlay 色 (既定 = black)。 */
    FVec3 m_Color              {0.0f, 0.0f, 0.0f};

    /** FadingOut にかける秒数。 */
    f32  m_OutDuration       = 0.0f;

    /** FadingIn にかける秒数。 */
    f32  m_InDuration        = 0.0f;

    /** MidPause を保持する秒数 (負値 = FadeOut の無期限保持)。 */
    f32  m_MidPause          = 0.0f;

    /** 現在 phase 内での経過秒。 */
    f32  m_Elapsed            = 0.0f;

    /** MidPause を 1 Tick 観測済みか (mid_pause=0 でも 1 Tick 保証用)。 */
    bool m_MidPauseConsumed = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FFadeTransition = CFadeTransition;

} // namespace acs::game
