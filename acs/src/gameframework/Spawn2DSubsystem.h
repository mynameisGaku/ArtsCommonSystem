// SPDX-License-Identifier: Apache-2.0
// GameFramework — FSpawn2DSubsystem (どこからでもプレハブをワールドへ生成する)
//
// World サブシステム。Owner()(= 所属 FScene2D)経由でシーンの root に手が届くので、
// «ノードを持たない側»(他のサブシステム・ゲームロジック・スクリプト)からでも
// プレハブをスポーンできる。敵・弾・拾い物の動的生成のハブ。
//
//   auto* spawner = GetSubsystem<acs::game::FSpawn2DSubsystem>();
//   spawner->SpawnPrefabFile("Assets/Enemy.acsprefab", FVec2{ 100, 0 });
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/Subsystem.h"
#include "gameframework/ANode.h"
#include "gameframework/Scene2D.h"          // FScene2D (Owner) / Root()
#include "gameframework/SceneTextLoader.h"  // SpawnPrefabText / SpawnPrefabFile

namespace acs::game {

/**
 * プレハブをワールド(所属 FScene2D の root)へ生成する World サブシステム。
 *
 * @details Owner() が FScene2D を指す(FScene が World サブシステムへ this を渡す)。
 * 2D 以外のシーンに対しては Owner が FScene2D でないため Spawn* は nullptr を返す。
 */
class FSpawn2DSubsystem : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FSpawn2DSubsystem)

    /**
     * プレハブテキスト(.acsprefab 直列化)をシーンへ生成し pos に配置する。
     *
     * @param text プレハブテキスト。
     * @param pos  生成位置(生成ルートの local position)。
     * @return 生成サブツリーのルート(失敗で nullptr)。
     */
    ANode* SpawnPrefabText(const char* text, FVec2 pos) noexcept {
        FScene2D* scene = OwnerAs<FScene2D>();
        if (scene == nullptr || text == nullptr) return nullptr;
        ANode* n = ::acs::game::SpawnPrefabText(text, scene->Root());
        if (n != nullptr) n->SetPosition2D(pos);
        return n;
    }

    /**
     * プレハブファイル(.acsprefab)をシーンへ生成し pos に配置する。
     *
     * @param path .acsprefab パス。
     * @param pos  生成位置。
     * @return 生成サブツリーのルート(失敗で nullptr)。
     */
    ANode* SpawnPrefabFile(const char* path, FVec2 pos) noexcept {
        FScene2D* scene = OwnerAs<FScene2D>();
        if (scene == nullptr || path == nullptr) return nullptr;
        ANode* n = ::acs::game::SpawnPrefabFile(path, scene->Root());
        if (n != nullptr) n->SetPosition2D(pos);
        return n;
    }
};

} // namespace acs::game
