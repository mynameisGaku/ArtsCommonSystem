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

/**
 * ステンシルマスク描画モード。
 *
 * @details
 * SetStencilMode で切り替える。stencil 付き深度バッファが bind されたパス
 * (FScene2D::SetStencilMaskEnabled(true)) でのみ意味を持つ。
 */
enum class EStencilMode : u8 {
    /** ステンシルテスト無し (ただし DSV bind パスでは DSV 整合 PSO を使う)。 */
    Off,

    /** 描いたピクセルにステンシル参照値を書く (マスク形状を焼く)。 */
    WriteMask,

    /** ステンシル == 参照値 の所だけ描く (マスク内側)。 */
    KeepInside,

    /** ステンシル != 参照値 の所だけ描く (マスク外側)。 */
    KeepOutside,
};

/**
 * ピクセル座標で 2D スプライト・矩形を描くバッチ式ヘルパ。
 *
 * @details
 * HUD や 2D ゲームの絵描き用。同じテクスチャの連続スプライトは自動でバッチされ、
 * テクスチャが切り替わるかバッチが満杯になると Flush して GPU に送る。座標系は
 * 左上原点・ピクセル単位で Y は下方向。Begin/Draw.../End の順に呼ぶ。
 */
class FSpriteBatch {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSpriteBatch() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSpriteBatch() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSpriteBatch(const FSpriteBatch&)            = delete;

    /** コピー代入も禁止。 */
    FSpriteBatch& operator=(const FSpriteBatch&) = delete;

    /**
     * VS+PS・パイプライン・頂点/インデックスバッファ・白テクスチャを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format レンダーターゲットのフォーマット。
     * @param max_sprites 1 フレームで描けるスプライト総数の上限。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format     = EFormat::B8G8R8A8_UNorm,
                      u32 max_sprites      = 4096) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 描画を開始する。
     *
     * @details screen サイズはピクセル → NDC 変換に使う。view はカメラ無しにリセットされる。
     * @param cl コマンドを積むコマンドリスト。
     * @param screen_w 画面幅 (ピクセル)。
     * @param screen_h 画面高さ (ピクセル)。
     */
    void Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    /**
     * テクスチャ全体を矩形に描く。
     *
     * @param tex 描画するテクスチャ。
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param tint 乗算色 (既定は白)。
     */
    void Draw(IRhiTexture& tex,
              f32 x, f32 y, f32 w, f32 h,
              FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * テクスチャの一部 (UV 0..1) を矩形に描く。
     *
     * @param tex 描画するテクスチャ。
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param u0 サンプル領域の左 U (0..1)。
     * @param v0 サンプル領域の上 V (0..1)。
     * @param u1 サンプル領域の右 U (0..1)。
     * @param v1 サンプル領域の下 V (0..1)。
     * @param tint 乗算色 (既定は白)。
     */
    void DrawSub(IRhiTexture& tex,
                 f32 x, f32 y, f32 w, f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * 単色矩形 (テクスチャ無し) を描く。
     *
     * @param x 左上 X (ピクセル)。
     * @param y 左上 Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param color 塗りつぶし色。
     */
    void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept;

    /**
     * UTF-8 テキストを描画する。
     *
     * @details \\n で改行し、フォントに未収録のグリフはスキップする。
     * @param font 描画に使うフォント (あらかじめ Init 済みのもの)。
     * @param utf8_text UTF-8 文字列。
     * @param x 行の左上 X (ピクセル)。
     * @param y 行の左上 Y (ピクセル)。
     * @param color 文字色 (既定は白)。
     */
    void DrawString(const class Font& font, const char* utf8_text,
                  f32 x, f32 y, FVec4 color = FVec4{1,1,1,1}) noexcept;

    /**
     * 回転付きでテクスチャ (の一部) を描く。
     *
     * @details (cx,cy) を中心に radians 回転する。通常スプライトと同じ 4 頂点 / 6 インデックスなので同一バッチに乗る。
     * @param tex 描画するテクスチャ。
     * @param cx 回転中心の X (ピクセル)。
     * @param cy 回転中心の Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param radians 回転角 (ラジアン)。
     * @param u0 サンプル領域の左 U (0..1)。
     * @param v0 サンプル領域の上 V (0..1)。
     * @param u1 サンプル領域の右 U (0..1)。
     * @param v1 サンプル領域の下 V (0..1)。
     * @param tint 乗算色 (既定は白)。
     */
    void DrawRotated(IRhiTexture& tex,
                     f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     f32 u0, f32 v0, f32 u1, f32 v1,
                     FVec4 tint = FVec4{1,1,1,1}) noexcept;

    /**
     * 回転付き単色矩形を描く。
     *
     * @param cx 回転中心の X (ピクセル)。
     * @param cy 回転中心の Y (ピクセル)。
     * @param w 幅 (ピクセル)。
     * @param h 高さ (ピクセル)。
     * @param radians 回転角 (ラジアン)。
     * @param color 塗りつぶし色。
     */
    void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                         FVec4 color) noexcept;

    /**
     * 単色の塗りつぶし三角形を描く。
     *
     * @details 4 頂点目に 3 頂点目を重ねた退化三角形を 1 枚挟み、通常スプライトと同じ 4 頂点バッチに乗せる。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param color 塗りつぶし色。
     */
    void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                      FVec4 color) noexcept;

    /**
     * 頂点カラー三角形 (グラデーション) を描く。
     *
     * @details 各頂点に別の色を与えるとシェーダが COLOR を補間する。水面の深さ勾配や泡のフェード等に使う。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param c0 頂点 0 の色。
     * @param c1 頂点 1 の色。
     * @param c2 頂点 2 の色。
     */
    void DrawTriangleVC(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                        FVec4 c0, FVec4 c1, FVec4 c2) noexcept;

    /**
     * 任意テクスチャを per-vertex UV で貼る三角形を描く。
     *
     * @details 水の反射 (シーン RT を鏡像 UV でサンプル) 等に使う。tint は全頂点共通の乗算色。
     * @param tex 貼り付けるテクスチャ。
     * @param x0 頂点 0 の X (ピクセル)。
     * @param y0 頂点 0 の Y (ピクセル)。
     * @param x1 頂点 1 の X (ピクセル)。
     * @param y1 頂点 1 の Y (ピクセル)。
     * @param x2 頂点 2 の X (ピクセル)。
     * @param y2 頂点 2 の Y (ピクセル)。
     * @param u0 頂点 0 の U。
     * @param v0 頂点 0 の V。
     * @param u1 頂点 1 の U。
     * @param v1 頂点 1 の V。
     * @param u2 頂点 2 の U。
     * @param v2 頂点 2 の V。
     * @param tint 全頂点共通の乗算色 (既定は白)。
     */
    void DrawTriangleSub(IRhiTexture& tex,
                         f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                         f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2,
                         FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

    /**
     * 2D カメラを設定する。
     *
     * @details (cam_x,cam_y) を画面中心に映し、zoom 倍で拡縮する。Begin() で恒等 (カメラ無し) にリセットされる。
     * @param cam_x カメラ中心の world X。
     * @param cam_y カメラ中心の world Y。
     * @param zoom ズーム倍率。
     */
    void SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept;

    /**
     * クリップ矩形を設定し、以降の描画をこの矩形内 (画面座標) に制限する。
     *
     * @param x クリップ矩形の左上 X (ピクセル)。
     * @param y クリップ矩形の左上 Y (ピクセル)。
     * @param w クリップ矩形の幅 (ピクセル)。
     * @param h クリップ矩形の高さ (ピクセル)。
     */
    void SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept;

    /** クリップ矩形を解除し、画面全体への描画に戻す。 */
    void ClearClipRect() noexcept;

    /**
     * ブレンドモードを切り替える。
     *
     * @details
     * バッチを flush してから PSO を切り替える。Additive PSO は遅延生成で、
     * コースティクス/光のきらめき等「背景を加算で明るくする」表現に使う。
     * Off に戻すときは AlphaBlend を渡す。ステンシルモードとは併用しないこと。
     * @param mode 適用するブレンドモード。
     */
    void SetBlendMode(EBlendMode mode) noexcept;

    /**
     * ステンシルマスクモードを切り替える。
     *
     * @details
     * バッチを flush してから PSO + 参照値を切り替える。任意形状のマスクで描画範囲を
     * 制限する用途で、WriteMask でマスク形状を焼き、KeepInside/KeepOutside でその内/外
     * だけに後続描画を通す。Off で解除。前提として stencil 付き深度バッファが bind された
     * パス (FScene2D::SetStencilMaskEnabled(true) が用意する) でのみ呼ぶこと。それ以外で
     * 呼ぶと DSV 不整合になるため、呼び出し側 (FScene2D / clip component) がガードする。
     * @param mode 適用するステンシルモード。
     * @param ref ステンシル参照値 (既定 1)。
     */
    void SetStencilMode(EStencilMode mode, u8 ref = 1) noexcept;

    /** 描画を終了し、残りのバッチを GPU に送る。 */
    void End() noexcept;

private:
    /** 1 スプライト頂点 (pos2D + uv + color)。 */
    struct Vertex {
        /** スクリーン X 座標。 */
        f32 x;

        /** スクリーン Y 座標。 */
        f32 y;

        /** テクスチャ U 座標。 */
        f32 u;

        /** テクスチャ V 座標。 */
        f32 v;

        /** 頂点カラーの赤成分。 */
        f32 r;

        /** 頂点カラーの緑成分。 */
        f32 g;

        /** 頂点カラーの青成分。 */
        f32 b;

        /** 頂点カラーのアルファ成分。 */
        f32 a;
    };

    /** 蓄積した頂点を GPU へ送り、DrawIndexed を発行する。 */
    void Flush() noexcept;

    /** screen サイズ + view を現在の view 定数バッファに書く。 */
    void WriteScreenCBuffer() noexcept;

    /** 次の view 定数バッファへ進める (フレーム内の view 切替ごと)。 */
    void AdvanceViewBuffer() noexcept;

    /** 現在の view 定数バッファを slot 0 に bind する。 */
    void BindViewBuffer() noexcept;

    /**
     * 4 種のステンシル PSO を遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureStencilPipelines() noexcept;

    /**
     * 加算ブレンド PSO を遅延生成する。
     *
     * @return 生成済み or 生成成功なら true。
     */
    bool EnsureAdditivePipeline() noexcept;

    /**
     * vs/ps/layout 等のパイプライン共通部を埋める。
     *
     * @param pd 埋める対象のパイプライン記述。
     */
    void FillCommonPipelineDesc(struct FPipelineDesc& pd) const noexcept;

    /** Init で受け取った device (ステンシル/加算 PSO の遅延生成用)。 */
    IRhiDevice*              m_Device  = nullptr;

    /** レンダーターゲットのフォーマット (遅延 PSO 生成で再利用)。 */
    EFormat                  m_RtFormat = EFormat::B8G8R8A8_UNorm;

    /** 頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 通常 (アルファブレンド・DSV 無し) 描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /**
     * ステンシルマスク用 PSO (DSVFormat=D24S8)。
     *
     * @details EStencilMode の順に [Off/Write/In/Out]。初回 SetStencilMode で遅延生成 (使わないシーンでは作らない)。
     */
    TUniquePtr<IRhiPipeline> m_StencilPipe[4];

    /** ステンシル PSO 群が生成済みかのフラグ。 */
    bool                     m_StencilReady = false;

    /** 現在のステンシルモード。 */
    EStencilMode             m_StencilMode  = EStencilMode::Off;

    /** 加算ブレンド用 PSO (DSV 無し、Additive)。初回 SetBlendMode(Additive) で遅延生成。 */
    TUniquePtr<IRhiPipeline> m_AdditivePipe;

    /** 加算ブレンド PSO が生成済みかのフラグ。 */
    bool                     m_AdditiveReady = false;

    /** 現在のブレンドモード。 */
    EBlendMode               m_BlendMode = EBlendMode::AlphaBlend;

    /** 頂点バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Vb;

    /** インデックスバッファ。 */
    TUniquePtr<IRhiBuffer>   m_Ib;

    /**
     * view 定数バッファのリング本数。
     *
     * @details
     * 1 フレームで world / HUD / 反射の各パスと複数の SetView があり、定数バッファは
     * 「フレーム内は単一アドレス上書き」なので 1 個だと先に積んだ DrawIndexed が後で
     * 上書きされた view を読んでしまう (world が HUD view で描かれて画面端に潰れる)。
     * view 切替ごとに別スロットへ書き root CBV を貼り直すことで、各 draw が記録時の
     * view を確実に読む。フレーム内の view 切替数 (反射込みで ~6) を十分上回る本数を
     * 確保し、in-flight 2 フレーム分でも巻き戻り衝突しないようにする。
     */
    static constexpr u32     kViewRing = 32;

    /** view 定数バッファのリング (screen size + view)。 */
    TUniquePtr<IRhiBuffer>   m_Cb[kViewRing];

    /** 現在使用中の view 定数バッファのインデックス。 */
    u32                      m_CbCur = 0;

    /** DrawRect 用の 1x1 白テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_White;

    /** CPU 側の頂点バッファステージ (Flush で GPU へコピー)。 */
    Vertex*          m_VertexCpu    = nullptr;

    /** 1 フレームで描けるスプライト総数の上限。 */
    u32              m_MaxSprites   = 0;

    /** フレーム内に積んだスプライトの累計数。 */
    u32              m_SpriteCount  = 0;

    /** 既に GPU に投入済みのスプライト数。 */
    u32              m_FlushedCount = 0;

    /** 現在バッチ中のテクスチャ (切り替わると Flush)。 */
    IRhiTexture*     m_CurrentTex   = nullptr;

    /** Begin で受け取ったコマンドリスト。 */
    IRhiCommandList* m_Cl            = nullptr;

    /** 画面幅 (ピクセル)。 */
    u32              m_ScreenW      = 1;

    /** 画面高さ (ピクセル)。 */
    u32              m_ScreenH      = 1;

    /** カメラ中心の world X。 */
    f32              m_ViewX        = 0.0f;

    /** カメラ中心の world Y。 */
    f32              m_ViewY        = 0.0f;

    /** カメラのズーム倍率。 */
    f32              m_ViewZoom     = 1.0f;
};

} // namespace acs
