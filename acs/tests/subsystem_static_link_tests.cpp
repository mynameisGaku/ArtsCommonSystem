// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Game.h"
#include "gameframework/EventBus.h"
#include "gameframework/Scene.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/WorldClockSubsystem.h"

using namespace acs;

namespace {

/** catalog 関数を直接参照せず CGame の link anchor を通す最小ゲーム。 */
class FStaticLinkGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return {}; }
};

} // namespace

ACS_TEST(SubsystemStaticLink, GameConstructorAnchorsBuiltins)
{
    FStaticLinkGame Game;
    AScene OwnerScene;
    CSubsystemCollection World;
    EXPECT_TRUE(World.TryInitialize(ESubsystemScope::World, nullptr,
                                    FSubsystemOwner{&OwnerScene, ESubsystemOwnerKind::Scene}));
    EXPECT_TRUE(World.Get<AEventBus>() != nullptr);
    EXPECT_TRUE(World.Get<ASpawn2DSubsystem>() != nullptr);
    EXPECT_TRUE(World.Get<AWorldClockSubsystem>() != nullptr);
}
