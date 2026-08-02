// SPDX-License-Identifier: Apache-2.0
// AScene2D - 2D ゲーム向けの実用的な基底 scene。
//
// 次の共通 2D stack を接続する:
//   CSceneServices(Default2D | Camera2D | Physics2D)
//   ルート ANode ツリー
//   world と HUD の描画で共有する 1 つの CSpriteBatch
//
// 利用側は scene ごとに同じ root/update/render 接続を再実装せず、
// OnReady/OnTick/OnFixedTick/OnDrawWorld/OnDrawHud を override する。
#pragma once

#include "gameframework/Forward.h"
#include "gameframework/Scene.h"
#include "gameframework/ANode.h"

namespace acs {

// 描画リソースは CGame が game 寿命で所有するため、シーンヘッダは実体を必要としない
// (docs/SceneUnification.md)。参照と引数にしか使わないので前方宣言で足りる。
class CSpriteBatch;

} // namespace acs

namespace acs::game {

/**
 * 2D ゲーム向けの実用的なシーン基底クラス。
 *
 * @details
 * 共通の 2D スタック (CSceneServices(Default2D | Camera2D | Physics2D)、root ANode ツリー、
 * world/HUD 描画用の共有 CSpriteBatch) を配線する。利用者は root/update/render の定型
 * 処理を毎シーン書き直す代わりに OnReady/OnTick/OnFixedTick/OnDrawWorld/OnDrawHud を
 * override する。平面反射とステンシルマスクをオプションで有効化できる。
 */
class AScene2D : public AScene {
public:
    /** 空の 2D シーンを構築する (root は AScene が確保する)。 */
    AScene2D() noexcept = default;

    /** シーンを破棄する。 */
    ~AScene2D() noexcept override = default;

    /** コピー禁止 (ANode ツリーを単独所有するため)。 */
    AScene2D(const AScene2D&)            = delete;

    /** コピー代入も禁止。 */
    AScene2D& operator=(const AScene2D&) = delete;

    /**
     * このシーンが要求するサービスを返す。
     *
     * @details AScene の既定は ESvc::None なので、2D スタックを使うシーンはこの派生を使うか
     * 同じ値を自分で返す。root と描画は AScene が持つ (docs/SceneUnification.md)。
     * @return Default2D | Camera2D | Physics2D の合成フラグ。
     */
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D;
    }
};

} // namespace acs::game
