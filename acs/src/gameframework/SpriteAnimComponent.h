// SPDX-License-Identifier: Apache-2.0
// FSpriteAnimComponent — FSpriteAnimator (時間→frame index) を FSprite2DComponent
// (UV サブ矩形) に橋渡しする Component2D。同じ FNode2D に付いた FSprite2DComponent
// の UV を毎フレーム書き換えてスプライトシートアニメを再生する。
//
// 使い方 (グリッドシート: 1 枚のテクスチャを cols×rows に等分):
//   auto node = MakeUnique<FNode2D>();
//   auto& spr = node->AddComponent<FSprite2DComponent>(FVec2{1,1});
//   spr.SetTexture(sheet_tex);
//   auto& anim = node->AddComponent<FSpriteAnimComponent>();
//   anim.InitGrid(/*cols=*/4, /*rows=*/1, /*frame_count=*/4, /*fps=*/8.0f);
//   anim.Play();
//
// 使い方 (FSpritePack の名前付き frame からアニメ列を作る):
//   anim.BeginFrames(/*fps=*/12.0f, EPlayMode::Loop);
//   anim.AddFrameUv(pack.ComputeUv(*pack.FindFrame("Run_00")));
//   anim.AddFrameUv(pack.ComputeUv(*pack.FindFrame("Run_01")));
//   anim.EndFrames();
//   anim.Play();
//
// 設計:
//   ・sibling の FSprite2DComponent は OnAttach 時点では未登録のことがある
//     (add 順依存) ため、最初の OnUpdate で遅延 lookup する (add 順非依存)。
//   ・frame index → UV は m_FrameUvs に事前計算して持つ。InitGrid はグリッドを
//     計算、BeginFrames/AddFrameUv/EndFrames は任意 UV 列を積む。
//   ・FSpriteAnimator が時間管理 (Loop/PingPong/Once、frame event) を担う。
#pragma once

#include "gameframework/Component2D.h"
#include "gameframework/SpriteAnimator.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

class FSprite2DComponent;

class FSpriteAnimComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FSpriteAnimComponent)

    FSpriteAnimComponent() noexcept = default;

    // グリッドシート初期化: テクスチャを cols×rows の等分セルに割り、先頭から
    // frame_count 枚を順に再生する。frame_count==0 なら cols*rows を使う。
    void InitGrid(u32 cols, u32 rows, u32 frame_count, f32 fps,
                  EPlayMode mode = EPlayMode::Loop) noexcept;

    // 任意 UV 列を積む API。BeginFrames → AddFrameUv* → EndFrames の順で使う。
    void BeginFrames(f32 fps, EPlayMode mode = EPlayMode::Loop) noexcept;
    void AddFrameUv(FVec4 uv) noexcept;     // {u0, v0, u1, v1}
    void EndFrames() noexcept;              // m_Anim.Init(frame 数, fps, mode)

    // 再生制御 (FSpriteAnimator への委譲)。
    void Play()  noexcept { m_Anim.Play(); }
    void Pause() noexcept { m_Anim.Pause(); }
    void Stop()  noexcept { m_Anim.Stop(); }
    void SetFps(f32 fps) noexcept { m_Anim.SetFps(fps); }
    bool IsPlaying()  const noexcept { return m_Anim.IsPlaying(); }
    bool IsFinished() const noexcept { return m_Anim.IsFinished(); }
    u32  CurrentFrame() const noexcept { return m_Anim.CurrentFrame(); }

    // 下位 animator に直接触れたい場合 (frame event 登録など)。
    FSpriteAnimator& Animator() noexcept { return m_Anim; }

    // RequireComponent: 描画先の FSprite2DComponent を要求 (無ければ自動追加)。
    void OnRequire(FNode2D& owner) noexcept override;
    void OnUpdate(f32 dt) noexcept override;

private:
    void ApplyCurrentFrame() noexcept;

    FSpriteAnimator     m_Anim;
    TArray<FVec4>       m_FrameUvs;          // frame index → {u0,v0,u1,v1}
    FSprite2DComponent* m_Sprite   = nullptr; // sibling (遅延 lookup)
    f32                 m_PendingFps = 1.0f;
    EPlayMode           m_PendingMode = EPlayMode::Loop;
    bool                m_Building = false;   // BeginFrames..EndFrames 中
};

} // namespace acs::game
