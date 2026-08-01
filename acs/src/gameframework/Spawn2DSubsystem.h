// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "math/Vec.h"
#include "subsystem/Subsystem.h"

namespace acs::game {

class ANode;
class FScene2D;

/** 2D シーンのルートへプレハブを生成する World サブシステム。 */
class FSpawn2DSubsystem : public FSubsystem {
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
    friend class FScene2D;

    /** FScene2D の初期化成功後にだけ生成先ルートを接続する。 */
    void BindTargetRoot(ANode* Root) noexcept { m_TargetRoot = Root; }

    /** 生成先の 2D シーンルート。所有しない。 */
    ANode* m_TargetRoot = nullptr;
};

} // namespace acs::game

namespace acs {

/** 生成したnodeをトップレベルから参照する正規入口。 */
using game::ANode;
/** GameFramework 内の実装型をトップレベルから参照する正規入口。 */
using game::FSpawn2DSubsystem;

} // namespace acs
