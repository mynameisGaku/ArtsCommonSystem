// SPDX-License-Identifier: Apache-2.0
// ASpriteAnimComponent — FSpriteAnimator (時間→frame index) を ASprite2DComponent
// (UV サブ矩形) に橋渡しする AComponent。同じ ANode に付いた ASprite2DComponent
// の UV を毎フレーム書き換えてスプライトシートアニメを再生する。
//
// 使い方 (グリッドシート: 1 枚のテクスチャを cols×rows に等分):
//   auto node = NewObject<ANode>();
//   auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{1,1});
//   spr.SetTexture(sheet_tex);
//   auto& anim = node->AddComponent<ASpriteAnimComponent>();
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
//   ・sibling の ASprite2DComponent は OnAttach 時点では未登録のことがある
//     (add 順依存) ため、最初の OnUpdate で遅延 lookup する (add 順非依存)。
//   ・frame index → UV は m_FrameUvs に事前計算して持つ。InitGrid はグリッドを
//     計算、BeginFrames/AddFrameUv/EndFrames は任意 UV 列を積む。
//   ・FSpriteAnimator が時間管理 (Loop/PingPong/Once、frame event) を担う。
#pragma once

#include "gameframework/AComponent.h"
#include "gameframework/SpriteAnimator.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

class ASprite2DComponent;

/**
 * 時間→frame index の FSpriteAnimator を sibling の ASprite2DComponent に橋渡しする AComponent。
 *
 * @details
 * 同じ ANode に付いた ASprite2DComponent の UV サブ矩形を毎フレーム書き換えて
 * スプライトシートアニメを再生する。frame index→UV は m_FrameUvs に事前計算して持ち、
 * InitGrid はグリッドシートを等分計算、BeginFrames/AddFrameUv/EndFrames は任意 UV 列を積む。
 * sibling の lookup は add 順依存を避けるため最初の OnUpdate まで遅延する。
 */
class ASpriteAnimComponent : public AComponent {
public:
    /** コンポーネント種別タグを宣言する (Kind() による型識別用)。 */
    ACS_GAME_COMPONENT_KIND(ASpriteAnimComponent)

    /** 空のアニメコンポーネントを構築する (frame 未設定・停止状態)。 */
    ASpriteAnimComponent() noexcept = default;

    /**
     * グリッドシートで初期化する。
     *
     * @details
     * テクスチャを cols×rows の等分セルに割り、先頭から frame_count 枚を順に再生する。
     * frame_count==0 なら cols*rows をそのまま使う。各セルの UV を m_FrameUvs に展開する。
     * @param cols 横方向のセル数。
     * @param rows 縦方向のセル数。
     * @param frame_count 再生する frame 数 (0 なら cols*rows)。
     * @param fps 毎秒の frame 切替数。
     * @param mode 再生モード (Loop/PingPong/Once、既定 Loop)。
     */
    void InitGrid(u32 cols, u32 rows, u32 frame_count, f32 fps,
                  EPlayMode mode = EPlayMode::Loop) noexcept;

    /**
     * 任意 UV 列の構築を開始する (BeginFrames → AddFrameUv* → EndFrames の順)。
     *
     * @param fps 毎秒の frame 切替数。
     * @param mode 再生モード (Loop/PingPong/Once、既定 Loop)。
     */
    void BeginFrames(f32 fps, EPlayMode mode = EPlayMode::Loop) noexcept;

    /**
     * 構築中の UV 列に 1 frame 分の UV を追加する。
     *
     * @param uv 追加する UV サブ矩形 {u0, v0, u1, v1}。
     */
    void AddFrameUv(FVec4 uv) noexcept;

    /** UV 列の構築を確定し、積んだ frame 数・fps・mode で内部 animator を初期化する。 */
    void EndFrames() noexcept;

    /** 再生を開始 (再開) する (内部 animator へ委譲)。 */
    void Play()  noexcept { m_Anim.Play(); }

    /** 位置を維持して一時停止する (内部 animator へ委譲)。 */
    void Pause() noexcept { m_Anim.Pause(); }

    /** 先頭に戻して停止する (内部 animator へ委譲)。 */
    void Stop()  noexcept { m_Anim.Stop(); }

    /**
     * 再生 fps を差し替える (内部 animator へ委譲)。
     *
     * @param fps 新しい毎秒 frame 切替数。
     */
    void SetFps(f32 fps) noexcept { m_Anim.SetFps(fps); }

    /**
     * 再生中かを返す。
     *
     * @return 再生中なら true。
     */
    bool IsPlaying()  const noexcept { return m_Anim.IsPlaying(); }

    /**
     * Once 再生が末尾で終了したかを返す。
     *
     * @return 終了済みなら true。
     */
    bool IsFinished() const noexcept { return m_Anim.IsFinished(); }

    /**
     * 現在の frame index を返す。
     *
     * @return 現在の frame index。
     */
    u32  CurrentFrame() const noexcept { return m_Anim.CurrentFrame(); }

    /**
     * 下位 animator への参照を返す (frame event 登録など直接操作用)。
     *
     * @return 内部 FSpriteAnimator への参照。
     */
    FSpriteAnimator& Animator() noexcept { return m_Anim; }

    /**
     * 描画先の ASprite2DComponent を要求する (RequireComponent、無ければ自動追加)。
     *
     * @param owner このコンポーネントが付く ANode。
     */
    void OnRequire(ANode& owner) noexcept override;

    /**
     * 毎フレーム animator を進め、現在 frame の UV を sibling sprite に反映する。
     *
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

private:
    /** 現在 frame の UV を遅延 lookup した sibling sprite に書き込む。 */
    void ApplyCurrentFrame() noexcept;

    /** 時間→frame index を管理する下位 animator。 */
    FSpriteAnimator     m_Anim;

    /** frame index → UV サブ矩形 {u0,v0,u1,v1} の事前計算テーブル。 */
    TArray<FVec4>       m_FrameUvs;

    /** 描画先 sibling の ASprite2DComponent (最初の OnUpdate で遅延 lookup)。 */
    ASprite2DComponent* m_Sprite   = nullptr;

    /** BeginFrames で受け取り EndFrames で animator に渡す保留 fps。 */
    f32                 m_PendingFps = 1.0f;

    /** BeginFrames で受け取り EndFrames で animator に渡す保留再生モード。 */
    EPlayMode           m_PendingMode = EPlayMode::Loop;

    /** BeginFrames..EndFrames の構築中フラグ。 */
    bool                m_Building = false;
};

} // namespace acs::game
