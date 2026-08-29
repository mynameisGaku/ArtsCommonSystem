// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Game.h"
#include "gameframework/EventBus.h"
#include "gameframework/Scene.h"
#include "gameframework/RenderFrameSubmissionSubsystem.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/WorldClockSubsystem.h"

using namespace acs;

namespace {

/** catalog 関数を直接参照せず CGame の link anchor を通す最小ゲーム。 */
class CStaticLinkGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return {}; }
};

} // namespace

ACS_TEST(SubsystemStaticLink, GameConstructorAnchorsBuiltins)
{
    CStaticLinkGame Game;
    AScene OwnerScene;
    CSubsystemCollection World;
    EXPECT_TRUE(World.TryInitialize(ESubsystemScope::World, nullptr,
                                    FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_TRUE(World.Get<AEventBus>() != nullptr);
    EXPECT_TRUE(World.Get<ASpawn2DSubsystem>() != nullptr);
    EXPECT_TRUE(World.Get<AWorldClockSubsystem>() != nullptr);
    EXPECT_TRUE(World.Get<CRenderFrameSubmissionSubsystem>() != nullptr);
}

ACS_TEST(SubsystemStaticLink, RenderFrameSubmissionKeepsOneExactWorldListener)
{
    /** listenerが受け取った回数と最後の結果。 */
    struct FTrace final {
        u32 call_count = 0u;
        FRendererFrameEndResult result{};
    };
    const auto record = [](
        void* listener,
        const FRendererFrameEndResult& result) noexcept {
        FTrace* const trace = static_cast<FTrace*>(listener);
        if (trace == nullptr) return;
        ++trace->call_count;
        trace->result = result;
    };

    CRenderFrameSubmissionSubsystem submission;
    FTrace first{};
    FTrace second{};
    EXPECT_TRUE(submission.TryBind(&first, record));
    EXPECT_TRUE(!submission.TryBind(&second, record));

    submission.Publish(FRendererFrameEndResult{41u, true, false});
    EXPECT_EQ(first.call_count, 1u);
    EXPECT_EQ(first.result.submission_id, 41u);
    EXPECT_TRUE(first.result.submitted);
    EXPECT_FALSE(first.result.presented);
    EXPECT_EQ(second.call_count, 0u);

    submission.Unbind(&second);
    submission.Publish(FRendererFrameEndResult{42u, false, false});
    EXPECT_EQ(first.call_count, 2u);
    EXPECT_EQ(first.result.submission_id, 42u);
    EXPECT_FALSE(first.result.submitted);

    submission.Unbind(&first);
    EXPECT_TRUE(submission.TryBind(&second, record));
    submission.OnDeinitialize();
    submission.Publish(FRendererFrameEndResult{43u, true, true});
    EXPECT_EQ(second.call_count, 0u);
}
