// SPDX-License-Identifier: Apache-2.0
#include "gameframework/cinematics/CCinematicPlayer.h"

#include "gameframework/cinematics/FCinematicDirectorBridge.h"

namespace acs::game {

bool CCinematicPlayer::TrySetAsset(TSharedPtr<asset::ACinematicAsset> asset) noexcept
{
    if (!asset) return false;
    // Directorへ移す前に全keyframeを確保し、失敗時の既存状態を保ちます。
    TResult<TArray<FTimelineKeyframe>> staged = FCinematicDirectorBridge::BuildKeyframes(*asset);
    if (staged.IsErr()) return false;
    if (!m_Director.TryReplaceKeyframes(Move(staged.Value()))) return false;
    // 検証済みアセットを強参照で保持し、文字列payloadの寿命を確保します。
    m_Asset = Move(asset);
    m_AssetView = m_Asset.Get();
    return true;
}

void CCinematicPlayer::Clear() noexcept
{
    m_Director.Clear();
    m_Asset.Reset();
    m_AssetView = nullptr;
}

void CCinematicPlayer::Tick(f32 dt) noexcept
{
    // callback中のClear後もassetを保持して同一batchのpayload寿命を守ります。
    TSharedPtr<asset::ACinematicAsset> dispatch_owner = m_Asset;
    (void)dispatch_owner;
    m_Director.Tick(dt);
}

} // namespace acs::game
