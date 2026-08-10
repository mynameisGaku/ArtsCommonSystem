// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/cinematics/ACinematicAsset.h"
#include "gameframework/CinematicsDirector.h"
#include "memory/SharedPtr.h"

namespace acs::game {

/** 所有者単位でアセットを再生し、検証済みviewを保持するプレイヤーです。 */
class CCinematicPlayer final {
public:
    /** 空の再生状態を生成します。 */
    CCinematicPlayer() noexcept = default;

    /** コピーを禁止します。 */
    CCinematicPlayer(const CCinematicPlayer&) = delete;
    CCinematicPlayer& operator=(const CCinematicPlayer&) = delete;
    CCinematicPlayer(CCinematicPlayer&&) = delete;
    CCinematicPlayer& operator=(CCinematicPlayer&&) = delete;

    /** assetを検証して全keyframeを確保し、失敗時は既存assetと再生列を変更しません。 */
    bool TrySetAsset(TSharedPtr<asset::ACinematicAsset> asset) noexcept;

    /** アセットと再生状態を空に戻します。 */
    void Clear() noexcept;

    /** 再生を開始または再開します。 */
    void Play() noexcept
    {
        m_Director.Play();
    }

    /** 再生を一時停止します。 */
    void Pause() noexcept
    {
        m_Director.Pause();
    }

    /** 再生を停止して時刻を初期化します。 */
    void Stop() noexcept
    {
        m_Director.Stop();
    }

    /** 有限の経過時間だけ再生を進め、callback実行中のasset寿命を保持します。 */
    void Tick(f32 dt) noexcept;

    /** 現在時刻を返します。 */
    f32 CurrentTime() const noexcept
    {
        return m_Director.CurrentTime();
    }

    /** 再生中か返します。 */
    bool IsPlaying() const noexcept
    {
        return m_Director.IsPlaying();
    }

    /** 全keyframeを発火し終えたか返します。 */
    bool IsFinished() const noexcept
    {
        return m_Director.IsFinished();
    }

    /** 保持中タイムラインの明示時間を返します。 */
    f32 TotalDuration() const noexcept
    {
        return m_Director.TotalDuration();
    }

    /** 所有中アセットの読み取りviewを返します。 */
    const asset::ACinematicAsset* AssetView() const noexcept
    {
        return m_AssetView;
    }

    /** カメラ発火通知をDirectorへ設定します。 */
    void SetCameraCallback(CameraCallbackFn callback, void* user) noexcept
    {
        m_Director.SetCameraCallback(callback, user);
    }

    /** 会話発火通知をDirectorへ設定します。 */
    void SetDialogueCallback(DialogueCallbackFn callback, void* user) noexcept
    {
        m_Director.SetDialogueCallback(callback, user);
    }

    /** 音楽発火通知をDirectorへ設定します。 */
    void SetMusicCallback(MusicCallbackFn callback, void* user) noexcept
    {
        m_Director.SetMusicCallback(callback, user);
    }

    /** 汎用イベント発火通知をDirectorへ設定します。 */
    void SetEventCallback(EventCallbackFn callback, void* user) noexcept
    {
        m_Director.SetEventCallback(callback, user);
    }

private:
    /** 所有アセットを保持して再生列のpayload寿命を保証します。 */
    TSharedPtr<asset::ACinematicAsset> m_Asset;

    /** 所有アセットの不変読み取りviewです。 */
    const asset::ACinematicAsset* m_AssetView = nullptr;

    /** プレイヤー固有の再生状態です。 */
    CCinematicsDirector m_Director;
};

} // namespace acs::game
