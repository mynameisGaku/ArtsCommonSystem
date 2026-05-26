// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FDamageFeedback (Polish & GameFeel)
//
// プレイヤーがダメージを受けたときの視覚フィードバックを集中管理する
// state holder。FPS / アクション系で必須の 3 要素を一箇所で扱う:
//   1) 画面エッジが赤くなる被弾オーバーレイ (vignette red)
//   2) 攻撃源を指す方向矢印 (off-screen damage indicator)
//   3) death cam (致命傷 → killer をズームアウトで映す演出)
//
// 設計選択 (Phase 2 = Pillar R Phase 2):
//   ・**FDamageFeedback 自身は描画しない / カメラを動かさない**:
//      FEffectSystem と同じ「副作用ゼロ / 純粋 state machine」方針。
//      ・赤エッジ → 描画パイプ末尾の overlay が ScreenEdgeRedIntensity() を
//                  vignette mask に掛けて加算 blend。
//      ・方向矢印 → UI 層が HasDirectionalIndicator() / DirectionalIndicator()
//                   / DirectionalIndicatorAlpha() を pull して描く。
//      ・death cam → カメラ制御コードが IsDeathCamActive() / DeathCamTarget()
//                    / DeathCamProgress() を見て自前で zoom + LookAt する。
//      これにより GameFramework から FRenderer / FCamera への依存を切る。
//   ・**赤エッジは累積 → 指数 decay**: TakeDamage で `intensity += amount * 0.1`
//      を加算 [0,1] clamp、Tick で `-= 2.0 * dt` の線形 decay (~0.5 秒で消える)。
//      累積式にすることで「連続被弾で画面が真っ赤に染まる」自然な挙動が出る。
//   ・**方向矢印は最後の被弾源を上書き**: source_direction が指定されたとき
//      のみ更新。指定なし (= FVec2{}) なら矢印は触らない (例: 環境ダメージ /
//      毒ダメージなど方向の無いダメージ)。alpha も累積でなく 1.0 リセットで、
//      decay は赤エッジと同様 2.0/s。
//   ・**source_direction の向き規約**: caller は「ダメージを与えた相手 →
//      プレイヤー」ベクトルを渡す (= 矢印が指す向き、画面外から自分を狙う方向)。
//      正規化は内部で行うので caller は正規化不要。
//   ・**death cam は明示的 trigger / exit**: HP <= 0 を検知したゲームコードが
//      `TriggerDeathCam(killer_pos)` を呼ぶ。`Progress()` は Tick で 0 → 1 へ
//      2 秒で進む dramatic zoom 用パラメータ。完全に進んでも自動で抜けない
//      (リスポーン UI を出すまで止めておきたいので)。
//   ・**Reset() でゲーム再開時に全状態を初期化**: シーン切替や respawn で
//      赤フェード残骸 / 矢印 / death cam を一括クリア。
//
// 非コピー・非ムーブ:
//   FGame / FScene のメンバとして 1 インスタンスだけ存在する想定。state が
//   重複すると death cam の trigger 整合性が壊れるので機械的に禁止。
//
// 使い方:
//   class GameplayScene : public FScene {
//       FDamageFeedback _dmg;
//       void OnPlayerHit(f32 amount, FVec2 attacker_pos) noexcept {
//           const FVec2 to_player = player.Position() - attacker_pos;
//           _dmg.TakeDamage(amount, to_player);
//           if (player.HP() <= 0.0f) {
//               _dmg.TriggerDeathCam(FVec3{attacker_pos.x, 0, attacker_pos.y});
//           }
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _dmg.Tick(dt);
//       }
//   };
//
// 範囲外 (Phase 3+):
//   ・血しぶき・画面割れ等の SFX 連携
//   ・low HP 時の心拍音 / 視野狭窄 (FAudioDirector / 別 overlay と連携)
//   ・death cam の cinematic curve (今は線形)
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"
#include "math/Vec.h"

namespace acs::game {

class FDamageFeedback {
public:
    FDamageFeedback() noexcept = default;
    ~FDamageFeedback() noexcept = default;

    FDamageFeedback(const FDamageFeedback&)            = delete;
    FDamageFeedback& operator=(const FDamageFeedback&) = delete;
    FDamageFeedback(FDamageFeedback&&)                 = delete;
    FDamageFeedback& operator=(FDamageFeedback&&)      = delete;

    // ----- ダメージ受領 -----
    // amount           : 受けたダメージ量 (任意スケール、0.1 倍で赤エッジに加算)
    // source_direction : 攻撃源 → プレイヤー方向ベクトル。零ベクトルなら矢印を
    //                    更新しない (= 環境ダメージ等の無方向ヒット)。
    //                    正規化は内部で実行 (caller は生 delta を渡せる)。
    void TakeDamage(f32 amount, FVec2 source_direction = FVec2{0.0f, 0.0f}) noexcept;

    // ----- driver -----
    // real-time dt で呼ばれる前提。各エフェクトの decay を進める。
    // dt が負なら 0 扱いで state を破壊しない。
    void Tick(f32 dt) noexcept;

    // ----- 赤エッジ accessors (描画 overlay が pull) -----
    // 0 = エフェクトなし、1 = 完全赤被せ。線形 [0,1]。
    f32 ScreenEdgeRedIntensity() const noexcept { return _red_intensity; }

    // ----- 方向矢印 accessors (UI 層が pull) -----
    // 矢印を描画すべきか (alpha が ε 以上か)
    bool HasDirectionalIndicator() const noexcept { return _dir_intensity > 0.0f; }
    // 矢印が指す方向 (正規化済み)。HasDirectionalIndicator() == false のとき
    // の値は未定義扱い (caller は HasDirectionalIndicator で守ること)。
    FVec2 DirectionalIndicator() const noexcept { return _dir_vec; }
    // 矢印の不透明度 [0,1]
    f32 DirectionalIndicatorAlpha() const noexcept { return _dir_intensity; }

    // ----- death cam -----
    // killer_pos: カメラを向ける対象の world 座標 (= 致命傷を与えた相手の位置)
    void TriggerDeathCam(FVec3 killer_pos) noexcept;

    // 現在 death cam 演出中か
    bool IsDeathCamActive() const noexcept { return _death_cam_active; }
    // カメラが LookAt すべき world 座標 (trigger 時の killer 位置)
    FVec3 DeathCamTarget() const noexcept { return _killer_pos; }
    // [0,1] dramatic zoom progress。Tick で 0 → 1 へ 2 秒で進み、その後は 1.0 で
    // 維持。caller (カメラ制御) はこれを zoom / DoF のイージングに使う。
    f32 DeathCamProgress() const noexcept { return _death_cam_t; }
    // 明示的に death cam を解除 (例: リスポーン処理開始時)
    void ExitDeathCam() noexcept;

    // ----- 一括リセット (シーン切替 / respawn 用) -----
    // 全 state を初期値に戻す。
    void Reset() noexcept;

private:
    // ---- 赤エッジ state ----
    // [0,1] 画面端を赤く染める強度。TakeDamage で加算、Tick で 2.0/s decay。
    f32 _red_intensity = 0.0f;

    // ---- 方向矢印 state ----
    // [0,1] 矢印の不透明度。TakeDamage(dir != 0) で 1.0、Tick で 2.0/s decay。
    f32  _dir_intensity = 0.0f;
    // 正規化済みの矢印方向ベクトル。最後に向きが指定された TakeDamage で更新。
    FVec2 _dir_vec       = FVec2{0.0f, 0.0f};

    // ---- death cam state ----
    bool _death_cam_active = false;
    // [0,1] dramatic zoom progress。Trigger 時 0、Tick で 1/2.0 * dt 加算。
    f32  _death_cam_t      = 0.0f;
    // カメラが見るべき killer 位置 (trigger 時にスナップ、以後 caller が読む)
    FVec3 _killer_pos       = FVec3{0.0f, 0.0f, 0.0f};

    // ---- デバッグ用 / 将来拡張 (low HP 演出フック等) ----
    // 累積ダメージ総量。Reset で 0 に戻る。FAmbientDirector などが
    // 「最近のダメージ密度」を読みたい時のフック。
    f32 _recent_damage_total = 0.0f;
};

} // namespace acs::game
