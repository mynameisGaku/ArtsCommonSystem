// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/Forward.h"
#include "math/Vec.h"
#include "subsystem/Subsystem.h"

namespace acs::game {

class ANode;
class ASpawn2DSubsystem;
/** 旧公開名を正規2D生成サブシステム型へ接続する互換別名。 */
using FSpawn2DSubsystem = ASpawn2DSubsystem;

/** 2D シーンのルートへプレハブを生成する World サブシステム。 */
class ASpawn2DSubsystem : public ASubsystem {
public:
    ACS_SUBSYSTEM_KIND(FSpawn2DSubsystem)

    /** 割り当てられた owner が Scene 契約なら利用を許可する。 */
    bool OnOwnerAssigned() noexcept override;

    /** 2D シーンとの非所有接続を解除する。 */
    void OnDeinitialize() noexcept override;

    /** プレハブ文字列を接続済み 2D シーンへ生成し、失敗時は nullptr を返す。 */
    ANode* SpawnPrefabText(const char* Text, FVec2 Position) noexcept;

    /** プレハブファイルを接続済み 2D シーンへ生成し、失敗時は nullptr を返す。 */
    ANode* SpawnPrefabFile(const char* Path, FVec2 Position) noexcept;

private:
    friend class AScene2D;

    /** AScene2D の初期化成功後にだけ生成先ルートを接続する。 */
    void BindTargetRoot(ANode* Root) noexcept { m_TargetRoot = Root; }

    /** 生成先の 2D シーンルート。所有しない。 */
    ANode* m_TargetRoot = nullptr;
};

} // namespace acs::game

namespace acs {

/** 生成したnodeをトップレベルから参照する正規入口。 */
using game::ANode;
/** GameFramework 内の実装型をトップレベルから参照する正規入口。 */
using game::ASpawn2DSubsystem;
/** 旧公開名を正規2D生成サブシステム型へ接続する互換別名。 */
using game::FSpawn2DSubsystem;

} // namespace acs
