// SPDX-License-Identifier: Apache-2.0
// 2D スプライト描画ヘルパ（バッチ式）
//
// 用途: ピクセル座標で 2D スプライト・矩形を描く。一般的な「ゲーム HUD」や
//       2D ゲームの絵描き用。同じテクスチャの連続スプライトは自動でバッチされる。
//
// 使い方:
//   SpriteBatch sb;
//   sb.Init(*renderer.Device(), renderer.ColorFormat(), max_sprites=4096);
//
//   // 描画フレーム中
//   auto* cl = renderer.CommandList();
//   sb.Begin(*cl, screen_w, screen_h);
//   sb.Draw(my_tex, 100, 200, 64, 64);                 // 64×64 を (100,200) に
//   sb.DrawRect(0, 0, screen_w, 32, Vec4{0,0,0,0.5f}); // 上部に半透明バー
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

class SpriteBatch {
public:
    SpriteBatch() noexcept = default;
    ~SpriteBatch() noexcept = default;

    SpriteBatch(const SpriteBatch&)            = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    // 初期化（VS+PS、パイプライン、頂点/インデックスバッファを作成）
    // max_sprites: 1 フレームで描けるスプライト総数の上限
    Result<void> Init(IRhiDevice& device,
                      EFormat rt_format     = EFormat::B8G8R8A8_UNorm,
                      u32 max_sprites      = 4096) noexcept;

    void Shutdown() noexcept;

    // 描画開始（screen サイズはピクセル → NDC 変換のために必要）
    void Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    // テクスチャ全体を矩形に描く
    void Draw(IRhiTexture& tex,
              f32 x, f32 y, f32 w, f32 h,
              Vec4 tint = Vec4{1,1,1,1}) noexcept;

    // テクスチャの一部を描く（UV 0..1）
    void DrawSub(IRhiTexture& tex,
                 f32 x, f32 y, f32 w, f32 h,
                 f32 u0, f32 v0, f32 u1, f32 v1,
                 Vec4 tint = Vec4{1,1,1,1}) noexcept;

    // 単色矩形（テクスチャ無し）
    void DrawRect(f32 x, f32 y, f32 w, f32 h, Vec4 color) noexcept;

    // テキスト描画（UTF-8、Font はあらかじめ Init 済みのもの、(x,y) は行の左上）
    // \n で改行、未収録グリフはスキップ。
    void DrawString(const class Font& font, const char* utf8_text,
                  f32 x, f32 y, Vec4 color = Vec4{1,1,1,1}) noexcept;

    // 回転付き描画。(cx,cy) を中心に radians だけ回転してテクスチャ(の一部)を描く。
    // 通常スプライトと同じ 4 頂点 / 6 インデックスなので同一バッチに乗る。
    void DrawRotated(IRhiTexture& tex,
                     f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     f32 u0, f32 v0, f32 u1, f32 v1,
                     Vec4 tint = Vec4{1,1,1,1}) noexcept;

    // 回転付き単色矩形。(cx,cy) を中心に radians だけ回転。
    void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                         Vec4 color) noexcept;

    // 単色の塗りつぶし三角形。4 頂点目に 3 頂点目を重ね、退化した三角形を
    // 1 枚挟むことで、通常スプライトと同じ 4 頂点バッチに乗せる。
    void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                      Vec4 color) noexcept;

    // 2D カメラ。(cam_x,cam_y) を画面中心に映し、zoom 倍で拡縮する。
    // Begin() で恒等（カメラ無し）にリセットされる。
    void SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept;

    // クリップ矩形。以降の描画をこの矩形内（画面座標）に制限する。
    void SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept;
    void ClearClipRect() noexcept;

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
    void WriteScreenCBuffer() noexcept;   // screen サイズ + view を _cb に書く

    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _vb;
    UniquePtr<IRhiBuffer>   _ib;
    UniquePtr<IRhiBuffer>   _cb;       // screen size (1/w, 1/h)
    UniquePtr<IRhiTexture>  _white;    // DrawRect 用 1×1 白テクスチャ

    Vertex*          _vertex_cpu    = nullptr;   // CPU 側の VB ステージ
    u32              _max_sprites   = 0;
    u32              _sprite_count  = 0;        // フレーム内累計
    u32              _flushed_count = 0;        // 既に GPU に投入済みのスプライト数
    IRhiTexture*     _current_tex   = nullptr;
    IRhiCommandList* _cl            = nullptr;
    u32              _screen_w      = 1;
    u32              _screen_h      = 1;
    f32              _view_x        = 0.0f;   // カメラ中心 X（ワールド座標）
    f32              _view_y        = 0.0f;   // カメラ中心 Y
    f32              _view_zoom     = 1.0f;   // ズーム倍率
};

} // namespace acs
