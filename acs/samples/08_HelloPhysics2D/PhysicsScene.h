// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — 物理シミュレーション + 描画を担当する scene。
// Application 派生はリソース所有 (SpriteBatch / Font / Texture) を担当し、
// 毎フレームの update / render を PhysicsScene に委譲する。
#pragma once

#include "Types.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

namespace hellophysics2d {

class PhysicsScene {
public:
    // 初期ボール (n 個) をランダム配置。Swapchain サイズで初期位置を決めるので
    // 呼び出し元は Renderer 準備後に呼ぶこと。
    void Init(acs::u32 screen_w, acs::u32 screen_h, acs::u32 initial_balls) noexcept;

    // ユーザー入力 + 1 フレームのシミュレーション。
    void Update(acs::f32 dt, acs::u32 screen_w, acs::u32 screen_h) noexcept;

    // SpriteBatch + テクスチャ + フォントを使って描画。
    void Render(acs::SpriteBatch& batch,
                acs::Font& font,
                acs::IRhiTexture& ball_tex,
                acs::f32 fps) noexcept;

    void Clear() noexcept { _ball_count = 0; }
    void SpawnBallAt(acs::f32 x, acs::f32 y) noexcept;

    acs::u32 BallCount() const noexcept { return _ball_count; }

private:
    void SpawnRandomBalls(acs::u32 n, acs::u32 screen_w, acs::u32 screen_h) noexcept;

    // xorshift32: 軽量・状態 4 byte のみで決定論的、サンプル用途に十分。
    acs::f32 RandF() noexcept;

    Ball     _balls[kMaxBalls] {};
    acs::u32 _ball_count = 0;
    acs::u32 _seed       = 0xCAFEBABEu;
};

} // namespace hellophysics2d
