// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — FCameraStack (Phase 2)
//
// 複数 `FCamera2D` を **virtual camera スタック**として保持し、最上層 (= top) を
// active として扱う Cinemachine 風スイッチャ。Push/Pop の遷移は **線形補間**で
// 古 top → 新 top の position/zoom/rotation をブレンドし、描画側は Effective*
// アクセサで「いまフレームでカメラがどこを写しているか」を取得する。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::FCamera2D  m_FollowCam;     // プレイヤー追従カメラ
//       acs::game::FCamera2D  m_CinematicCam;  // 演出用カメラ (固定 or 別追従)
//       acs::game::FCameraStack m_Stack;
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
//       void OnRender(RenderContext& rc) noexcept override {
//           rc.SetViewCenter(m_Stack.EffectivePosition());
//           rc.SetViewZoom(m_Stack.EffectiveZoom());
//           rc.SetViewRotation(m_Stack.EffectiveRotation());
//           // ... draw ...
//       }
//   };
//
// 設計選択 (Phase 2 = Pillar E Phase 2):
//   ・**virtual camera スタック**: FCamera2D は user が own。FCameraStack は
//     **non-owning pointer** だけを持つ。寿命管理は呼び出し側責任 (= Scene 内
//     のメンバ変数として持つのが典型)。
//   ・**最大 4 layer**: 想定用途は「平常 / 演出 / カットイン / メニュー」程度。
//     深く積む必要はないので static 上限 4。超えたら警告して無視。
//   ・**Push 中の補間**: 直近 2 層 (旧 top と新 top) を線形補間する。Pop の
//     場合は「下層 (= 残るカメラ)」と「現 top (= これから消えるカメラ)」を
//     補間し、blend 終了時に top を実際に pop する。途中で更に Push/Pop が
//     来た場合は新しい blend に切り替わる (後勝ち)。
//   ・**blend_t は entry 単位で保持**: 各 CameraEntry が自身の blend 状態
//     (`blend_t` の経過進捗 / `is_in` = フェードイン中か否か / 補間 duration)
//     を持つ。top の `is_in == true && blend_t < 1` の間が「blending」。
//   ・**zoom は対数補間**: 1.0 → 4.0 の blend で線形だと「2.5 → 中点」が
//     不自然 (光学的には 2.0 が中点)。`exp(lerp(log(a), log(b), t))` で
//     人間が「ちょうど中間」と感じる補間にする。zoom <= 0 は 0.001 にクランプ。
//   ・**rotation は最短角補間**: ±π を跨ぐ場合に最短経路で回るよう、差分を
//     [-π, π] に正規化してから lerp。
//   ・**Tick**: 上から順に **active な 2 層**だけ FCamera2D::Tick を呼ぶ
//     (= スタックに積まれているが blend に絡まない下層は止めておく)。これに
//     より「下層カメラがプレイヤーを追ってずれていく」事故を防ぐ。下層を
//     生かしておきたいケースは呼び出し側で個別 Tick すれば良い。
//   ・**非コピー・非ムーブ**: 内部 TArray が non-owning ptr を持つだけだが、
//     FSceneManager と同じく「state holder は move されない」方針で統一。
//
// 範囲外 (Phase 3+ で):
//   ・blend カーブ指定 (現状 linear 固定。easing は user が dt 前に通せば良い)
//   ・複数 camera の同時 blend (3 層以上を重みづけ合成)
//   ・Cinemachine 級の priority queue / blend graph
//   ・3D camera (Pillar E Phase 3 で Camera3D を別途定義予定)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/Camera2D.h"

namespace acs::game {

class FCameraStack {
public:
    FCameraStack() noexcept = default;
    ~FCameraStack() noexcept = default;

    FCameraStack(const FCameraStack&)            = delete;
    FCameraStack& operator=(const FCameraStack&) = delete;
    FCameraStack(FCameraStack&&)                 = delete;
    FCameraStack& operator=(FCameraStack&&)      = delete;

    // ----- 遷移 -----
    // `cam` を top に push し、`blend_duration` 秒かけて旧 top から線形補間する。
    // `blend_duration <= 0` で即時切替。スタックが既に kMaxLayers なら警告し
    // 無視する (= 同じカメラを 2 回 push しても重複登録される点に注意)。
    void PushCamera(FCamera2D& cam, f32 blend_duration = 0.5f) noexcept;

    // top を pop。`blend_duration` 秒かけて下層へフェードアウトしてから実際に
    // 取り除く。スタックが 1 枚以下なら警告して無視 (= 完全に空にはしない)。
    void PopCamera(f32 blend_duration = 0.5f) noexcept;

    // ----- 状態取得 -----
    FCamera2D* Active() const noexcept;          // 現在の top (= 最上層)。空なら nullptr。
    bool      IsBlending() const noexcept;
    f32       BlendProgress() const noexcept;   // [0, 1]、非 blend 時は 1。
    u32       Depth() const noexcept { return static_cast<u32>(m_Entries.Size()); }
    void      Clear() noexcept;

    // ----- 描画側が読む補間結果 -----
    // active (= top) のカメラ値を、blend 中は「下層 (= 一つ下) との線形補間」
    // した値で返す。zoom は対数補間、rotation は最短角補間 (wrap-around 補正)。
    FVec2 EffectivePosition() const noexcept;
    f32  EffectiveZoom()     const noexcept;
    f32  EffectiveRotation() const noexcept;

    // ----- driver -----
    // blend timer 進行 + active な 2 層の FCamera2D::Tick 呼び出し。pop の
    // blend が完了したフレームで実際に top を取り除く。
    void Tick(f32 dt) noexcept;

    // virtual camera の同時保持上限。深い積み重ねは想定外 (= 4 で十分)。
    static constexpr u32 kMaxLayers = 4;

private:
    struct CameraEntry {
        FCamera2D* cam            = nullptr;
        f32       blend_t        = 1.0f;   // 経過進捗 [0,1]。1 = blend 完了。
        f32       blend_duration = 0.0f;   // <= 0 なら即時。
        bool      is_in          = true;   // true = フェードイン (Push 起源)、
                                            // false = フェードアウト (Pop 起源)。
    };

    // [0,1] の進捗で `a` と `b` を線形補間 (FVec2)。
    static FVec2 LerpVec2(FVec2 a, FVec2 b, f32 t) noexcept;
    // zoom は exp(lerp(log(a), log(b), t)) (光学的に自然な中点)。
    static f32  LerpZoom(f32 a, f32 b, f32 t) noexcept;
    // rotation は差分を [-π, π] に正規化してから lerp (最短角)。
    static f32  LerpAngle(f32 a, f32 b, f32 t) noexcept;

    TArray<CameraEntry> m_Entries;   // m_Entries.Back() が top (= active)
};

} // namespace acs::game
