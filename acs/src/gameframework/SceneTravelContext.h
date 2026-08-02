// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// GameFramework — シーン遷移で «次のシーンへ持っていく» 任意データ
// ----------------------------------------------------------------------------
// ChangeScene / PushScene / PopScene / TransitionTo に travel context を添えると、
// 遷移先のシーンがそれを受け取る。所有権は遷移先のシーンへ移り、シーンが生きて
// いる間は何度でも読める。
//
//   // 送る側
//   class CResultContext final : public CSceneTravelContext {
//   public:
//       ACS_RTTI(CResultContext, CSceneTravelContext)
//       i32 Score = 0;
//   };
//
//   TUniquePtr<CResultContext> ctx = MakeUnique<CResultContext>();
//   ctx->Score = m_Score;
//   Scenes().ChangeScene(MakeUnique<AResultScene>(), Move(ctx));
//
//   // 受け取る側
//   void OnReady() noexcept override {
//       if (const CResultContext* c = TravelContext<CResultContext>()) {
//           m_Score = c->Score;
//       }
//   }
//
// 遷移要求はフレーム境界まで遅延するため、context は要求時から遷移先の OnEnter
// まで CSceneManager が保持し、そこでシーンへ引き渡す。遷移が失敗したり、後から
// 別の要求で上書きされた場合、その context は破棄される。
//
// 型の取り違えは `Cast<T>` (侵入型 RTTI) が防ぐ。派生には必ず
// `ACS_RTTI(自分の型, 親の型)` を書くこと。書かないと TravelContext<T>() は
// 親の型としてしか一致しない。
// ============================================================================
#pragma once

#include "foundation/Cast.h"

namespace acs::game {

/**
 * シーン遷移で引き渡す任意データの基底。
 *
 * @details
 * 利用者はこれを継承し、`ACS_RTTI(Derived, CSceneTravelContext)` を書いて必要な
 * フィールドを足す。エンジンは中身を解釈せず、遷移先のシーンへ所有権ごと渡すだけ。
 */
class CSceneTravelContext {
public:
    ACS_RTTI_ROOT(CSceneTravelContext)

    /** 空の context を構築する。 */
    CSceneTravelContext() noexcept = default;

    /** 派生を正しく破棄するための仮想デストラクタ。 */
    virtual ~CSceneTravelContext() noexcept = default;

    /** コピー禁止 (遷移先へ所有権ごと渡す前提のため)。 */
    CSceneTravelContext(const CSceneTravelContext&)            = delete;

    /** コピー代入も禁止。 */
    CSceneTravelContext& operator=(const CSceneTravelContext&) = delete;
};

} // namespace acs::game

namespace acs {

/** シーン遷移で引き渡す任意データの基底をトップレベルから参照する正規入口。 */
using game::CSceneTravelContext;

} // namespace acs
