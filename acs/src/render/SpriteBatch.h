// SPDX-License-Identifier: Apache-2.0
// 2D スプライト描画ヘルパ（バッチ式）
//
// 用途: ピクセル座標で 2D スプライト・矩形を描く。一般的な「ゲーム HUD」や
//       2D ゲームの絵描き用。同じテクスチャの連続スプライトは自動でバッチされる。
//
// 使い方:
//   FSpriteBatch sb;
//   sb.Init(*renderer.Device(), renderer.ColorFormat(), max_sprites=4096);
//
//   // 描画フレーム中
//   auto* cl = renderer.CommandList();
//   sb.Begin(*cl, screen_w, screen_h);
//   sb.Draw(my_tex, 100, 200, 64, 64);                 // 64×64 を (100,200) に
//   sb.DrawRect(0, 0, screen_w, 32, FVec4{0,0,0,0.5f}); // 上部に半透明バー
//   sb.End();
//
// 座標系: 左上原点、ピクセル単位。Y が下方向。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiCommandList.h"
#include "render/RhiTypes.h"

namespace acs {

// ステンシルマスク描画モード。SetStencilMode で切り替える。stencil 付き深度バッファが
// bind されたパス (FScene2D::SetStencilMaskEnabled(true)) でのみ意味を持つ。
enum class EStencilMode : u8 {
    Off,          // ステンシルテスト無し (ただし DSV bind パスでは DSV 整合 PSO を使う)
    WriteMask,    // 描いたピクセルにステンシル参照値を書く (マスク形状を焼く)
    KeepInside,   // ステンシル == 参照値 の所だけ描く (マスク内側)
    KeepOutside,  // ステンシル != 参照値 の所だけ描く (マスク外側)
};

class FSpriteBatch {
public:
    FSpriteBatch() noexcept = default;
    ~FSpriteBatch() noexcept = default;

    FSpriteBatch(const FSpriteBatch&)            = delete;
    FSpriteBatch& operator=(const FSpriteBatch&) = delete;

    // 初期化（VS+PS、パイプライン、頂点/インデックスバッファを作成）
    // max_sprites: 1 フレームで描けるスプライト総数の上限
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format     = EFormat::B8G8R8A8_UNorm,
                      u32 max_sprites      = 4096) noexcept;

    void Shutdown() noexcept;

    // 描画開始（screen サイズはピクセル → NDC 変換のために必要）
    void Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    // テクスチャ全体を矩形に描く
    void Draw(IRhiTexture& tex,
              f32 x, f32 y, f32 w, f32 h,
              FVec4 tint = FVec4{1,1,1,1}) noexcept;

    // テクスチャの一部を描く（UV 0..1）
    void DrawSub(IRhiTexture& tex,
                 f32 x, f32 y, f32 w, f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 FVec4 tint = FVec4{1,1,1,1}) noexcept;

    // 単色矩形（テクスチャ無し）
    void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept;

    // テキスト描画（UTF-8、Font はあらかじめ Init 済みのもの、(x,y) は行の左上）
    // \n で改行、未収録グリフはスキップ。
    void DrawString(const class Font& font, const char* utf8_text,
                  f32 x, f32 y, FVec4 color = FVec4{1,1,1,1}) noexcept;

    // 回転付き描画。(cx,cy) を中心に radians だけ回転してテクスチャ(の一部)を描く。
    // 通常スプライトと同じ 4 頂点 / 6 インデックスなので同一バッチに乗る。
    void DrawRotated(IRhiTexture& tex,
                     f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     f32 u0, f32 v0, f32 u1, f32 v1,
                     FVec4 tint = FVec4{1,1,1,1}) noexcept;

    // 回転付き単色矩形。(cx,cy) を中心に radians だけ回転。
    void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                         FVec4 color) noexcept;

    // 単色の塗りつぶし三角形。4 頂点目に 3 頂点目を重ね、退化した三角形を
    // 1 枚挟むことで、通常スプライトと同じ 4 頂点バッチに乗せる。
    void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                      FVec4 color) noexcept;

    // 頂点カラー三角形 (gradient)。各頂点に別の色を与えると既存シェーダが
    // COLOR を補間する。水面の深さ勾配や泡のフェード等に使う。
    void DrawTriangleVC(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                        FVec4 c0, FVec4 c1, FVec4 c2) noexcept;

    // 任意テクスチャを per-vertex UV で貼る三角形。水の反射 (シーン RT を鏡像 UV で
    // サンプル) 等に使う。tint は全頂点共通の乗算色。
    void DrawTriangleSub(IRhiTexture& tex,
                         f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                         f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2,
                         FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

    // 2D カメラ。(cam_x,cam_y) を画面中心に映し、zoom 倍で拡縮する。
    // Begin() で恒等（カメラ無し）にリセットされる。
    void SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept;

    // クリップ矩形。以降の描画をこの矩形内（画面座標）に制限する。
    void SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept;
    void ClearClipRect() noexcept;

    // ブレンドモードを切り替える (バッチを flush してから PSO を切替)。Additive は
    // 遅延生成。コースティクス/光のきらめき等、背景を「加算で明るくする」表現に使う。
    // Off に戻すときは AlphaBlend を渡す。ステンシルモードとは併用しないこと。
    void SetBlendMode(EBlendMode mode) noexcept;

    // ステンシルマスクモードを切り替える (バッチを flush してから PSO + 参照値を切替)。
    // 任意形状のマスクで描画範囲を制限する用途。WriteMask でマスク形状を焼き、
    // KeepInside/KeepOutside でその内/外だけに後続描画を通す。Off で解除。
    // **前提**: stencil 付き深度バッファが bind されたパスでのみ呼ぶこと
    //   (FScene2D::SetStencilMaskEnabled(true) が用意する)。それ以外で呼ぶと
    //   DSV 不整合になるため、呼び出し側 (FScene2D / clip component) がガードする。
    void SetStencilMode(EStencilMode mode, u8 ref = 1) noexcept;

    // 描画終了（残りバッチを GPU に送る）
    void End() noexcept;

private:
    // 1 頂点 = pos2D + uv + color
    struct Vertex {
        f32 x, y;
        f32 u, v;
        f32 r, g, b, a;
    };

    void Flush() noexcept;
    void EnsurePipeline() noexcept;
    void WriteScreenCBuffer() noexcept;   // screen サイズ + view を現 view バッファに書く
    void AdvanceViewBuffer() noexcept;    // 次の view 定数バッファへ (フレーム内の view 切替ごと)
    void BindViewBuffer() noexcept;       // 現 view 定数バッファを slot0 に bind
    bool EnsureStencilPipelines() noexcept;  // 4 種のステンシル PSO を遅延生成
    bool EnsureAdditivePipeline() noexcept;  // 加算ブレンド PSO を遅延生成
    void FillCommonPipelineDesc(struct FPipelineDesc& pd) const noexcept;  // vs/ps/layout 等共通部

    IRhiDevice*              m_Device  = nullptr;   // ステンシル PSO 遅延生成用
    EFormat                  m_RtFormat = EFormat::B8G8R8A8_UNorm;

    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    // ステンシルマスク用 PSO (DSVFormat=D24S8)。EStencilMode の順に [Off/Write/In/Out]。
    // 初回 SetStencilMode で遅延生成 (使わないシーンでは作らない)。
    TUniquePtr<IRhiPipeline> m_StencilPipe[4];
    bool                     m_StencilReady = false;
    EStencilMode             m_StencilMode  = EStencilMode::Off;
    // 加算ブレンド用 PSO (DSV 無し、Additive)。初回 SetBlendMode(Additive) で遅延生成。
    TUniquePtr<IRhiPipeline> m_AdditivePipe;
    bool                     m_AdditiveReady = false;
    EBlendMode               m_BlendMode = EBlendMode::AlphaBlend;
    TUniquePtr<IRhiBuffer>   m_Vb;
    TUniquePtr<IRhiBuffer>   m_Ib;
    // view 定数バッファのリング (screen size + view)。1 フレームで world / HUD / 反射
    // 各パスと複数の SetView があり、定数バッファは「フレーム内は単一アドレス上書き」
    // なので、1 個だと先に積んだ DrawIndexed が後で上書きされた view を読んでしまう
    // (world が HUD view で描かれて画面端に潰れる)。view 切替ごとに別スロットへ書き、
    // root CBV を貼り直すことで、各 draw が記録時の view を確実に読む。リングは
    // フレーム内の view 切替数 (反射込みで ~6) を十分上回る本数を確保 (in-flight 2 フレーム
    // 分でも巻き戻り衝突しない)。
    static constexpr u32     kViewRing = 32;
    TUniquePtr<IRhiBuffer>   m_Cb[kViewRing];
    u32                      m_CbCur = 0;
    TUniquePtr<IRhiTexture>  m_White;    // DrawRect 用 1×1 白テクスチャ

    Vertex*          m_VertexCpu    = nullptr;   // CPU 側の VB ステージ
    u32              m_MaxSprites   = 0;
    u32              m_SpriteCount  = 0;        // フレーム内累計
    u32              m_FlushedCount = 0;        // 既に GPU に投入済みのスプライト数
    IRhiTexture*     m_CurrentTex   = nullptr;
    IRhiCommandList* m_Cl            = nullptr;
    u32              m_ScreenW      = 1;
    u32              m_ScreenH      = 1;
    f32              m_ViewX        = 0.0f;   // カメラ中心 X（ワールド座標）
    f32              m_ViewY        = 0.0f;   // カメラ中心 Y
    f32              m_ViewZoom     = 1.0f;   // ズーム倍率
};

} // namespace acs
