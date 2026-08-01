// SPDX-License-Identifier: Apache-2.0
// HelloECS — FApplication 派生クラス。
//
// ACS の ECS / Event / Timer / 並列イテレーションを 1 つの app で紹介する小品。
//
// やってること:
//   ・World に 200 個の Entity を生成、各々 Position + Velocity + FColor
//   ・OnUpdate 毎フレーム: World.Query<Position, Velocity>().EachParallel(...) で
//     FThreadPool 経由の並列移動更新（壁で反射）
//   ・FSpriteBatch で円型テクスチャを使って粒子のように描画
//   ・CTimerManager で 5 秒おきに Entity を 30 個追加
//   ・MessageBroker で「Entity が追加された」イベントを pub/sub 通知
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"
#include "foundation/Types.h"

namespace hello04 {

class FHelloEcsApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

    // SpawnEvent を Publish するためのアクセサ (Timer コールバックから使用)。
    void SpawnRandomEntities(acs::u32 n) noexcept;

private:
    acs::f32 m_Rnd() noexcept;

    acs::FSpriteBatch            m_Batch;
    acs::TUniquePtr<acs::IRhiTexture> m_Tex;
    acs::u32                    m_Seed = 0xCAFEBABEu;
};

} // namespace hello04
