// SPDX-License-Identifier: Apache-2.0
// HelloECS — FApplication 派生クラス。
//
// ACS の ECS / FEvent / Timer / 並列イテレーションを 1 つの app で紹介する小品。
//
// やってること:
//   ・FWorld に 200 個の Entity を生成、各々 Position + Velocity + FColor
//   ・OnUpdate 毎フレーム: FWorld.Query<Position, Velocity>().EachParallel(...) で
//     FThreadPool 経由の並列移動更新（壁で反射）
//   ・FSpriteBatch で円型テクスチャを使って粒子のように描画
//   ・FTimerManager で 5 秒おきに Entity を 30 個追加
//   ・FMessageBroker で「Entity が追加された」イベントを pub/sub 通知
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"
#include "foundation/Types.h"

namespace hello04 {

class HelloECSApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

    // SpawnEvent を Publish するためのアクセサ (Timer コールバックから使用)。
    void SpawnRandomEntities(acs::u32 n) noexcept;

private:
    acs::f32 _rnd() noexcept;

    acs::FSpriteBatch            _batch;
    acs::TUniquePtr<acs::IRhiTexture> _tex;
    acs::u32                    _seed = 0xCAFEBABEu;
};

} // namespace hello04
