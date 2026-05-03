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
                      Format rt_format     = Format::B8G8R8A8_UNorm,
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
};

} // namespace acs
