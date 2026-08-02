// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — CCameraStack
//
// 複数 `CCamera2D` を **virtual camera スタック**として保持し、最上層 (= top) を
// active として扱う Cinemachine 風スイッチャ。Push/Pop の遷移は **線形補間**で
// 古 top → 新 top の position/zoom/rotation をブレンドし、描画側は Effective*
// アクセサで「いまフレームでカメラがどこを写しているか」を取得する。
//
// 使い方:
//   class AGameplayScene : public AScene {
//       acs::game::CCamera2D  m_FollowCam;     // プレイヤー追従カメラ
//       acs::game::CCamera2D  m_CinematicCam;  // 演出用カメラ (固定 or 別追従)
//       acs::game::CCameraStack m_Stack;
//       void OnEnter() noexcept override {
//           m_Stack.PushCamera(m_FollowCam);                // 初期 top
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_FollowCam.SetTargetPos(player.Position());
//           m_Stack.Tick(dt);  // blend + active camera tick
//           if (BossIntroStarted()) {
//               m_Stack.PushCamera(m_CinematicCam, 1.0f);   // 1 秒で blend
//           } else if (BossIntroEnded()) {
//               m_Stack.PopCamera(0.5f);                    // 戻る
//           }
//       }
//       void OnRender(FRenderContext& rc) noexcept override {
//           rc.SetViewCenter(m_Stack.EffectivePosition());
//           rc.SetViewZoom(m_Stack.EffectiveZoom());
//           rc.SetViewRotation(m_Stack.EffectiveRotation());
//           // ... draw ...
//       }
//   };
//
// 設計選択 (Pillar E):
//   ・**virtual camera スタック**: CCamera2D は user が own。CCameraStack は
//     **non-owning pointer** だけを持つ。寿命管理は呼び出し側責任 (= AScene 内
//     のメンバ変数として持つのが典型)。
//   ・**最大 4 layer**: 想定用途は「平常 / 演出 / カットイン / メニュー」程度。
//     深く積む必要はないので static 上限 4。超えたら警告して無視。
//   ・**Push 中の補間**: 直近 2 層 (旧 top と新 top) を線形補間する。Pop の
//     場合は「下層 (= 残るカメラ)」と「現 top (= これから消えるカメラ)」を
//     補間し、blend 終了時に top を実際に pop する。途中で更に Push/Pop が
//     来た場合は新しい blend に切り替わる (後勝ち)。
//   ・**blend_t は entry 単位で保持**: 各 FCameraEntry が自身の blend 状態
//     (`blend_t` の経過進捗 / `is_in` = フェードイン中か否か / 補間 duration)
//     を持つ。top の `is_in == true && blend_t < 1` の間が「blending」。
//   ・**zoom は対数補間**: 1.0 → 4.0 の blend で線形だと「2.5 → 中点」が
//     不自然 (光学的には 2.0 が中点)。`exp(lerp(log(a), log(b), t))` で
//     人間が「ちょうど中間」と感じる補間にする。zoom <= 0 は 0.001 にクランプ。
//   ・**rotation は最短角補間**: ±π を跨ぐ場合に最短経路で回るよう、差分を
//     [-π, π] に正規化してから lerp。
//   ・**Tick**: 上から順に **active な 2 層**だけ CCamera2D::Tick を呼ぶ
//     (= スタックに積まれているが blend に絡まない下層は止めておく)。これに
//     より「下層カメラがプレイヤーを追ってずれていく」事故を防ぐ。下層を
//     生かしておきたいケースは呼び出し側で個別 Tick すれば良い。
//   ・**非コピー・非ムーブ**: 内部 TArray が non-owning ptr を持つだけだが、
//     CSceneManager と同じく「state holder は move されない」方針で統一。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/Camera2D.h"

namespace acs::game {

/**
 * 複数の CCamera2D を virtual camera スタックとして保持し、最上層を active とする切替器。
 *
 * @details
 * Cinemachine 風スイッチャ。Push/Pop の遷移は直近 2 層を補間 (position/zoom/rotation) し、
 * 描画側は Effective* アクセサで現在のカメラ値を取得する。zoom は対数補間、rotation は
 * 最短角補間。CCamera2D は user が own し、本クラスは non-owning ポインタのみを最大 4 層持つ
 * (非コピー・非ムーブ)。Tick は active な 2 層だけ CCamera2D::Tick を呼ぶ。
 */
class CCameraStack {
public:
    /** 空のスタックを構築する。 */
    CCameraStack() noexcept = default;

    /** 破棄する (カメラ実体は所有しないので解放しない)。 */
    ~CCameraStack() noexcept = default;

    /** コピー禁止 (state holder は move/copy されない方針)。 */
    CCameraStack(const CCameraStack&)            = delete;

    /** コピー代入も禁止。 */
    CCameraStack& operator=(const CCameraStack&) = delete;

    /** ムーブ禁止。 */
    CCameraStack(CCameraStack&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CCameraStack& operator=(CCameraStack&&)      = delete;

    /**
     * カメラを top に push し、旧 top から補間しながら切り替える。
     *
     * @details blend_duration <= 0 で即時切替。スタックが既に kMaxLayers なら警告して無視する
     * (= 同じカメラを 2 回 push しても重複登録される点に注意)。
     * @param cam push する非所有カメラ。
     * @param blend_duration 旧 top からの補間にかける秒数。
     */
    void PushCamera(CCamera2D& cam, f32 blend_duration = 0.5f) noexcept;

    /**
     * top を pop する。
     *
     * @details blend_duration 秒かけて下層へフェードアウトしてから実際に取り除く。
     * スタックが 1 枚以下なら警告して無視する (= 完全に空にはしない)。
     * @param blend_duration 下層へのフェードアウトにかける秒数。
     */
    void PopCamera(f32 blend_duration = 0.5f) noexcept;

    /**
     * 現在 active なカメラ (= 最上層) を返す。
     *
     * @return top のカメラ (空なら nullptr)。
     */
    CCamera2D* Active() const noexcept;

    /**
     * blend 中かどうかを返す。
     *
     * @return top が補間途中 (下層あり + blend_t < 1 + duration > 0) なら true。
     */
    bool      IsBlending() const noexcept;

    /**
     * 現在の blend 進捗を返す。
     *
     * @return blend 進捗 [0, 1] (非 blend 時は 1)。
     */
    f32       BlendProgress() const noexcept;

    /**
     * スタックの深さ (積まれているカメラ数) を返す。
     *
     * @return スタック内のカメラ数。
     */
    u32       Depth() const noexcept { return static_cast<u32>(m_Entries.Size()); }

    /** スタックを空にする (全エントリを破棄)。 */
    void      Clear() noexcept;

    /**
     * 描画に使う実 view center を返す。
     *
     * @details blend 中は下層との線形補間値。それ以外は top の EffectiveViewCenter。
     * @return 補間後の view center (world 座標)。
     */
    FVec2 EffectivePosition() const noexcept;

    /**
     * 描画に使う実 zoom を返す。
     *
     * @details blend 中は下層との対数補間値。それ以外は top の zoom。
     * @return 補間後の zoom 倍率。
     */
    f32  EffectiveZoom()     const noexcept;

    /**
     * 描画に使う実 rotation を返す。
     *
     * @details blend 中は下層との最短角補間値。それ以外は top の rotation。
     * @return 補間後の回転角 (radians)。
     */
    f32  EffectiveRotation() const noexcept;

    /**
     * スタックを 1 フレーム進める。
     *
     * @details blend timer を進め、active な 2 層 (top と blend 中なら下層) の CCamera2D::Tick を
     * 呼ぶ。pop の blend が完了したフレームで実際に top を取り除く。
     * @param dt 経過秒 (負値は 0 にクランプ)。
     */
    void Tick(f32 dt) noexcept;

    /** virtual camera の同時保持上限 (深い積み重ねは想定外なので 4)。 */
    static constexpr u32 kMaxLayers = 4;

private:
    /**
     * スタックに積まれたカメラ 1 層分のエントリ (カメラ参照と blend 状態)。
     */
    struct FCameraEntry {
        /** このエントリが指す非所有カメラ。 */
        CCamera2D* cam            = nullptr;

        /** blend の経過進捗 [0,1]。1 = blend 完了。 */
        f32       blend_t        = 1.0f;

        /** blend にかける秒数 (<= 0 なら即時)。 */
        f32       blend_duration = 0.0f;

        /** true = フェードイン (Push 起源)、false = フェードアウト (Pop 起源)。 */
        bool      is_in          = true;
    };

    /**
     * 2 つの FVec2 を進捗 t で線形補間する。
     *
     * @param a 始点。
     * @param b 終点。
     * @param t 補間進捗 [0,1]。
     * @return 補間後の FVec2。
     */
    static FVec2 LerpVec2(FVec2 a, FVec2 b, f32 t) noexcept;

    /**
     * 2 つの zoom を対数空間で補間する (光学的に自然な中点)。
     *
     * @param a 始点 zoom。
     * @param b 終点 zoom。
     * @param t 補間進捗 [0,1]。
     * @return exp(lerp(log(a), log(b), t))。入力は下限 0.001 にクランプ。
     */
    static f32  LerpZoom(f32 a, f32 b, f32 t) noexcept;

    /**
     * 2 つの角度を最短角経路で補間する。
     *
     * @param a 始点角 (radians)。
     * @param b 終点角 (radians)。
     * @param t 補間進捗 [0,1]。
     * @return 差分を [-π, π] に正規化してから lerp した角度。
     */
    static f32  LerpAngle(f32 a, f32 b, f32 t) noexcept;

    /** カメラエントリの配列 (Back() が top = active)。 */
    TArray<FCameraEntry> m_Entries;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FCameraStack = CCameraStack;

} // namespace acs::game
