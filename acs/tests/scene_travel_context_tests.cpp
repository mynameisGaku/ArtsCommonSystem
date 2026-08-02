// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// シーン遷移で任意データを次のシーンへ持っていく (travel context) の検証:
//   ・Change/Push で遷移先が OnEnter の時点から読めること
//   ・Pop でモーダルの «結果» を戻り先へ渡せること
//   ・型違いは nullptr になること (Cast<T> による安全判定)
//   ・context 無しの遷移、pop できない pop、要求の上書きが安全であること
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Game.h"
#include "gameframework/Scene.h"
#include "gameframework/SceneManager.h"
#include "gameframework/SceneTravelContext.h"
#include "gameframework/Subsystem.h"
#include "memory/UniquePtr.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 遷移先へ渡す整数 1 個のテスト用 context。 */
class CScoreTravelContext final : public CSceneTravelContext {
public:
    ACS_RTTI(CScoreTravelContext, CSceneTravelContext)

    explicit CScoreTravelContext(i32 value) noexcept : Score(value) {}

    /** 引き渡す値。 */
    i32 Score = 0;
};

/** 型違い検査に使う別の context。 */
class COtherTravelContext final : public CSceneTravelContext {
public:
    ACS_RTTI(COtherTravelContext, CSceneTravelContext)
};

/** OnEnter / OnResume の時点で見えた context を記録するシーン。 */
class AProbeScene final : public AScene {
public:
    /** OnEnter 時点で読めた Score (読めなければ -1)。 */
    i32 entered_score = -1;

    /** OnResume 時点で読めた Score (読めなければ -1)。 */
    i32 resumed_score = -1;

    /** OnEnter 時点で COtherTravelContext として読めたか。 */
    bool entered_wrong_type_matched = false;

    void OnEnter() noexcept override {
        const CScoreTravelContext* const Context = TravelContext<CScoreTravelContext>();
        entered_score = Context != nullptr ? Context->Score : -1;
        entered_wrong_type_matched = TravelContext<COtherTravelContext>() != nullptr;
    }

    void OnResume() noexcept override {
        const CScoreTravelContext* const Context = TravelContext<CScoreTravelContext>();
        resumed_score = Context != nullptr ? Context->Score : -1;
    }
};

/** シーンを持たない最小の headless game。 */
class CTravelProbeGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return {}; }
};

/** Engine / GameInstance サブシステムを初期化した headless game を用意する。 */
bool InitializeProbeGame(CTravelProbeGame& Game) noexcept {
    if (!Game.EngineSubsystems().TryInitialize(
            ESubsystemScope::Engine, nullptr,
            FSubsystemOwner{&Game, ESubsystemOwnerKind::Application})) {
        return false;
    }
    return Game.GameInstanceSubsystems().TryInitialize(
        ESubsystemScope::GameInstance, &Game.EngineSubsystems(),
        FSubsystemOwner{&Game, ESubsystemOwnerKind::Game});
}

} // namespace

// Change 遷移に添えた context を、遷移先が OnEnter の時点で読める。
ACS_TEST(SceneTravelContext, ChangeSceneDeliversContextBeforeOnEnter) {
    CTravelProbeGame Game;
    EXPECT_TRUE(InitializeProbeGame(Game));

    Game.Scenes().ChangeScene(MakeUnique<AProbeScene>(),
                              MakeUnique<CScoreTravelContext>(1200));
    Game.Scenes()._ApplyPending(Game);

    AProbeScene* const Scene = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Scene != nullptr);
    if (Scene == nullptr) return;
    EXPECT_EQ(Scene->entered_score, 1200);
    // 型が違えば一致しない (Cast<T> が守る)。
    EXPECT_TRUE(!Scene->entered_wrong_type_matched);
    // 所有権はシーン側にあるので OnEnter 後も読める。
    EXPECT_TRUE(Scene->HasTravelContext());
    const CScoreTravelContext* const Held = Scene->TravelContext<CScoreTravelContext>();
    EXPECT_TRUE(Held != nullptr);
    if (Held != nullptr) EXPECT_EQ(Held->Score, 1200);
}

// context を添えない遷移では context 無しになる (既存の呼び出しは無変更で動く)。
ACS_TEST(SceneTravelContext, ChangeSceneWithoutContextLeavesSceneEmpty) {
    CTravelProbeGame Game;
    EXPECT_TRUE(InitializeProbeGame(Game));

    Game.Scenes().ChangeScene(MakeUnique<AProbeScene>());
    Game.Scenes()._ApplyPending(Game);

    AProbeScene* const Scene = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Scene != nullptr);
    if (Scene == nullptr) return;
    EXPECT_EQ(Scene->entered_score, -1);
    EXPECT_TRUE(!Scene->HasTravelContext());
    EXPECT_TRUE(Scene->TravelContextBase() == nullptr);
}

// Push した先も OnEnter で読め、Pop に添えた context は戻り先が OnResume で読む。
ACS_TEST(SceneTravelContext, PushAndPopCarryContextBothWays) {
    CTravelProbeGame Game;
    EXPECT_TRUE(InitializeProbeGame(Game));

    Game.Scenes().PushScene(MakeUnique<AProbeScene>());
    Game.Scenes()._ApplyPending(Game);
    AProbeScene* const Base = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Base != nullptr);
    if (Base == nullptr) return;

    Game.Scenes().PushScene(MakeUnique<AProbeScene>(),
                            MakeUnique<CScoreTravelContext>(7));
    Game.Scenes()._ApplyPending(Game);
    AProbeScene* const Modal = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Modal != nullptr);
    if (Modal == nullptr) return;
    EXPECT_EQ(Modal->entered_score, 7);
    EXPECT_EQ(Game.Scenes().Depth(), 2u);

    // モーダルの結果を戻り先へ返す。
    Game.Scenes().PopScene(MakeUnique<CScoreTravelContext>(42));
    Game.Scenes()._ApplyPending(Game);
    EXPECT_EQ(Game.Scenes().Depth(), 1u);
    EXPECT_EQ(Base->resumed_score, 42);
}

// 適用前に上書きされた要求の context は捨てられ、後勝ちの context だけが届く。
ACS_TEST(SceneTravelContext, LastRequestWinsAndDropsEarlierContext) {
    CTravelProbeGame Game;
    EXPECT_TRUE(InitializeProbeGame(Game));

    Game.Scenes().ChangeScene(MakeUnique<AProbeScene>(),
                              MakeUnique<CScoreTravelContext>(1));
    Game.Scenes().ChangeScene(MakeUnique<AProbeScene>(),
                              MakeUnique<CScoreTravelContext>(2));
    Game.Scenes()._ApplyPending(Game);

    AProbeScene* const Scene = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Scene != nullptr);
    if (Scene == nullptr) return;
    EXPECT_EQ(Scene->entered_score, 2);
    EXPECT_EQ(Game.Scenes().Depth(), 1u);
}

// pop できないスタックでの PopScene(context) は何も起こさず、context は破棄される。
ACS_TEST(SceneTravelContext, PopOnSingleSceneStackKeepsTopUntouched) {
    CTravelProbeGame Game;
    EXPECT_TRUE(InitializeProbeGame(Game));

    Game.Scenes().PushScene(MakeUnique<AProbeScene>());
    Game.Scenes()._ApplyPending(Game);
    AProbeScene* const Scene = static_cast<AProbeScene*>(Game.Scenes().Top());
    EXPECT_TRUE(Scene != nullptr);
    if (Scene == nullptr) return;

    Game.Scenes().PopScene(MakeUnique<CScoreTravelContext>(99));
    Game.Scenes()._ApplyPending(Game);

    EXPECT_EQ(Game.Scenes().Depth(), 1u);
    EXPECT_TRUE(Game.Scenes().Top() == Scene);
    EXPECT_EQ(Scene->resumed_score, -1);
    EXPECT_TRUE(!Scene->HasTravelContext());
}
