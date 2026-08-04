// SPDX-License-Identifier: Apache-2.0
// 入力ポーリング API（キーボード / マウス / ゲームパッド）
//
// 使い方:
//   while (!w.ShouldClose()) {
//       CInput::Update();           // フレーム先頭で呼ぶ
//       w.PollEvents();
//
//       if (CInput::IsKeyDown(EKey::Space)) Jump();
//       if (CInput::IsMouseButtonPressed(EMouseButton::Left)) Shoot();
//       FVec2 m = CInput::MousePos();
//   }
//
// ・「Down」 = 現在押されている
// ・「Pressed」 = このフレームで押された (前フレームは Down ではなかった)
// ・「Released」 = このフレームで離された
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "platform/GamepadMotion.h"
#include "platform/InputCodes.h"
#include "platform/Event.h"

namespace acs {

/**
 * キーボード・マウス・ゲームパッドの入力ポーリング API (全メソッド static)。
 *
 * @details
 * 状態はプロセス唯一のグローバルに保持する。フレーム先頭で Update を呼んで現フレームと
 * 前フレームを進め、FWindow からのイベントを OnEvent でこの状態へ反映する。「Down」=現在
 * 押下中、「Pressed」=このフレームで押下開始、「Released」=このフレームで離した、を表す。
 */
class CInput {
public:
    /**
     * フレーム先頭で 1 回呼び、入力状態をフレーム間で進める。
     *
     * @details 現フレームの押下状態を前フレームへ移し、マウス差分・ホイール累積を確定し、
     * テキスト入力をクリアし、XInput を 4 ポート分ポーリングする。
     */
    static void Update() noexcept;

    /**
     * FWindow から受け取ったイベントを入力状態へ反映する。
     *
     * @details FWindow::SetEventCallback でこの関数へブリッジして使う。
     * @param e 反映するイベント。
     */
    static void OnEvent(const FEvent& e) noexcept;

    /**
     * キーが現在押されているかを返す。
     *
     * @param k 対象キー。
     * @return 押下中なら true。
     */
    static bool IsKeyDown(EKey k) noexcept;

    /**
     * キーがこのフレームで押下開始されたかを返す。
     *
     * @param k 対象キー。
     * @return 前フレームは離されていて今フレームで押されたなら true。
     */
    static bool IsKeyPressed(EKey k) noexcept;

    /**
     * キーがこのフレームで離されたかを返す。
     *
     * @param k 対象キー。
     * @return 前フレームは押されていて今フレームで離されたなら true。
     */
    static bool IsKeyReleased(EKey k) noexcept;

    /**
     * マウスボタンが現在押されているかを返す。
     *
     * @param b 対象ボタン。
     * @return 押下中なら true。
     */
    static bool IsMouseButtonDown(EMouseButton b) noexcept;

    /**
     * マウスボタンがこのフレームで押下開始されたかを返す。
     *
     * @param b 対象ボタン。
     * @return 前フレームは離されていて今フレームで押されたなら true。
     */
    static bool IsMouseButtonPressed(EMouseButton b) noexcept;

    /**
     * マウスボタンがこのフレームで離されたかを返す。
     *
     * @param b 対象ボタン。
     * @return 前フレームは押されていて今フレームで離されたなら true。
     */
    static bool IsMouseButtonReleased(EMouseButton b) noexcept;

    /**
     * マウスカーソルの現在位置を返す。
     *
     * @return クライアント座標での位置 (px)。
     */
    static FVec2 MousePos() noexcept;

    /**
     * 前フレームからのマウス移動量を返す。
     *
     * @return クライアント座標での差分 (px)。
     */
    static FVec2 MouseDelta() noexcept;

    /**
     * このフレームのマウスホイール回転量を返す。
     *
     * @return ノッチ数 (正=奥へ回転、負=手前へ回転)。
     */
    static f32  MouseWheel() noexcept;

    /**
     * このフレームに入力されたテキストを返す。
     *
     * @details IME 確定後の UTF-8 文字列。次の Update でクリアされる内部バッファを指す。
     * @return NUL 終端の UTF-8 文字列 (入力が無ければ空文字列)。
     */
    static const char* TextInput() noexcept;

    /**
     * 指定プレイヤーのゲームパッドが接続されているかを返す。
     *
     * @param player_index プレイヤー番号 (0..3)。
     * @return 直近の Update で接続が確認できれば true (範囲外は false)。
     */
    static bool IsGamepadConnected(u32 player_index) noexcept;

    /**
     * 指定プレイヤーのゲームパッドボタンが現在押されているかを返す。
     *
     * @param player_index プレイヤー番号 (0..3)。
     * @param b 対象ボタン。
     * @return 押下中なら true (未接続・範囲外・Guide ボタンは false)。
     */
    static bool IsGamepadButtonDown(u32 player_index, EGamepadButton b) noexcept;

    /**
     * 指定プレイヤーのゲームパッドボタンがこのフレームで押下開始されたかを返す。
     *
     * @param player_index プレイヤー番号 (0..3)。
     * @param b 対象ボタン。
     * @return 前フレームは離されていて今フレームで押されたなら true (未接続・範囲外は false)。
     */
    static bool IsGamepadButtonPressed(u32 player_index, EGamepadButton b) noexcept;

    /**
     * 指定プレイヤーのゲームパッドボタンがこのフレームで解放されたかを返す。
     *
     * @details 押下中にゲームパッドが切断された場合も、一度だけ解放として扱う。
     * @param player_index プレイヤー番号 (0..3)。
     * @param b 対象ボタン。
     * @return 前フレームは押されていて今フレームで離されたなら true。
     */
    static bool IsGamepadButtonReleased(u32 player_index, EGamepadButton b) noexcept;

    /**
     * 指定プレイヤーのゲームパッド軸の値を返す。
     *
     * @details スティックはデッドゾーン処理込みで -1.0〜+1.0、トリガーは 0.0〜1.0 に正規化。
     * @param player_index プレイヤー番号 (0..3)。
     * @param axis 対象の軸。
     * @return 正規化された軸の値 (未接続・範囲外は 0.0)。
     */
    static f32  GamepadAxisValue(u32 player_index, EGamepadAxis axis) noexcept;

    /**
     * 指定プレイヤーのゲームパッドのモーションセンサーの値を返す。
     *
     * @details
     * ジャイロと加速度を積んだパッド (DualShock 4 / DualSense / Switch Pro / Joy-Con) でのみ
     * 取れる。センサーを持たないパッドや未接続では valid が false の既定値を返す。
     * @param player_index プレイヤー番号 (0..3)。
     * @return モーションの値。
     */
    static const FGamepadMotion& GamepadMotion(u32 player_index) noexcept;

    /**
     * 指定プレイヤーのゲームパッドの姿勢をクォータニオンで返す。
     *
     * @details
     * ジャイロを積分し、静止しているときは加速度で傾きを補正した向き。基準は最後に
     * ResetGamepadOrientation を呼んだ時点 (既定は接続時) の姿勢で、絶対方位は分からない。
     * センサーを持たないパッドでは無回転を返す。
     * @param player_index プレイヤー番号 (0..3)。
     * @return 姿勢を表す単位クォータニオン。
     */
    static const FQuat& GamepadRotation(u32 player_index) noexcept;

    /**
     * 指定プレイヤーのゲームパッドの姿勢を回転行列で返す。
     *
     * @details 中身は GamepadRotation と同じ向きで、行列が欲しい場合に使う。
     * @param player_index プレイヤー番号 (0..3)。
     * @return 回転行列 (平行移動なし)。
     */
    static FMat4 GamepadRotationMatrix(u32 player_index) noexcept;

    /**
     * 指定プレイヤーのゲームパッドの姿勢の基準を、いまの向きに取り直す。
     *
     * @param player_index プレイヤー番号 (0..3)。
     */
    static void ResetGamepadOrientation(u32 player_index) noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FInput = CInput;

} // namespace acs
