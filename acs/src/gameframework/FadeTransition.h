// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FFadeTransition (Phase A polish)
//
// シーン切替時のフェード演出を司る **state holder**。描画自体は行わず、
// 現在の overlay alpha / color と phase だけを提供する。ユーザー側で
// FSpriteBatch を使い fullscreen quad を「色 = OverlayColor()、alpha =
// OverlayAlpha()」で描くだけで、典型的なフェードイン・アウト・ブラック
// フラッシュ・クロスフェードが成立する。
//
// 使い方:
//   class TitleScene : public Scene {
//       acs::game::FFadeTransition m_Fade;
//       void OnEnter() noexcept override {
//           // 画面が黒から徐々に明けるフェードイン
//           m_Fade.StartFade(EFadeKind::FadeIn, /*out=*/0.0f, /*in=*/0.5f);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Fade.Tick(dt);
//           if (m_Fade.IsMidPause()) {
//               // FadeInOut の暗転中: ここでシーン切替
//               Manager().ChangeScene<GameplayScene>();
//           }
//       }
//       void OnRender() noexcept override {
//           // ... 通常描画 ...
//           if (m_Fade.IsActive()) {
//               const acs::FVec3 c = m_Fade.OverlayColor();
//               const f32       a = m_Fade.OverlayAlpha();
//               FSpriteBatch().FillFullScreen(c.x, c.y, c.z, a);
//           }
//       }
//   };
//
// 設計選択 (Phase A polish):
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
//   ・実描画 (FSpriteBatch 呼び出し)
//   ・easing 曲線指定 (linear 固定。必要なら user 側で OverlayAlpha() を
//      Easing::InOutSine 等に通す)
//   ・複数 overlay の重ね合わせ
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

enum class EFadeKind : u32 {
    None       = 0,
    FadeIn     = 1,  // 暗黒 → 明 (alpha 1→0)
    FadeOut    = 2,  // 明 → 暗黒 (alpha 0→1)
    FadeInOut  = 3,  // 0→1→pause→0、mid で切替
    CrossFade  = 4,  // 0→0.5→0 軽量代用
};

enum class EFadePhase : u32 {
    Idle       = 0,
    FadingOut  = 1,  // alpha 増加中
    MidPause   = 2,  // 暗転待機 (シーン切替タイミング)
    FadingIn   = 3,  // alpha 減少中
};

class FFadeTransition {
public:
    FFadeTransition() noexcept = default;
    ~FFadeTransition() noexcept = default;

    FFadeTransition(const FFadeTransition&)            = delete;
    FFadeTransition& operator=(const FFadeTransition&) = delete;
    FFadeTransition(FFadeTransition&&)                 = delete;
    FFadeTransition& operator=(FFadeTransition&&)      = delete;

    // ----- 開始 -----
    // out_duration: FadingOut にかける秒数 (FadeIn の時は無視)
    // in_duration : FadingIn にかける秒数 (FadeOut の時は無視)
    // mid_pause   : MidPause を保持する秒数。0 でも 1 Tick は MidPause を通る。
    void StartFade(EFadeKind kind,
                   f32 out_duration = 0.3f,
                   f32 in_duration  = 0.3f,
                   f32 mid_pause    = 0.0f) noexcept;

    // ----- 状態 -----
    bool      IsActive()      const noexcept { return m_Phase != EFadePhase::Idle; }
    bool      IsMidPause()    const noexcept { return m_Phase == EFadePhase::MidPause; }
    EFadePhase CurrentPhase()  const noexcept { return m_Phase; }
    EFadeKind  CurrentKind()   const noexcept { return m_Kind; }

    // [0, 1]、描画側 overlay の不透明度
    f32  OverlayAlpha() const noexcept { return m_Alpha; }

    // Overlay 色 (既定 = black)
    FVec3 OverlayColor() const noexcept { return m_Color; }
    void SetOverlayColor(FVec3 c) noexcept { m_Color = c; }

    // ----- driver -----
    void Tick(f32 dt) noexcept;

    // 直ちに Idle に戻す (alpha=0)。途中キャンセル用。
    void Cancel() noexcept;

private:
    // CrossFade のピーク alpha (= 0.5 が「画面が半透けまで暗くなって戻る」相当)
    static constexpr f32 kCrossFadePeak = 0.5f;

    EFadePhase m_Phase         = EFadePhase::Idle;
    EFadeKind  m_Kind          = EFadeKind::None;

    f32  m_Alpha              = 0.0f;
    FVec3 m_Color              {0.0f, 0.0f, 0.0f};  // 既定: black

    f32  m_OutDuration       = 0.0f;
    f32  m_InDuration        = 0.0f;
    f32  m_MidPause          = 0.0f;
    f32  m_Elapsed            = 0.0f;   // 現在 phase 内での経過秒
    bool m_MidPauseConsumed = false;  // mid_pause=0 のとき 1 Tick 保証
};

} // namespace acs::game
