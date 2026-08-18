// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// GameFramework — 即時描画 (batch を持たずに «関数を呼ぶだけ» で描く)
// ----------------------------------------------------------------------------
// AScene の描画パスの間だけ「今の描画先」が publish され、この header の関数が
// そこへ直接描く。CSpriteBatch を受け取る必要も、Begin/End を書く必要もない。
//
//   class AMyScene final : public AScene {
//       ESvc WantedServices() const noexcept override { return kScene2DServices; }
//
//       void OnDrawHud() noexcept override {
//           DrawRect(12, 12, 360, 54, FVec4{0, 0, 0, 0.45f});
//           DrawString(24, 24, "Hello", FVec4{1, 1, 1, 1});
//       }
//   };
//
// 呼べる場所:
//   ・AScene::OnDrawWorld / OnDrawHud (引数あり版・なし版のどちら)
//   ・上記から呼ばれた自作関数、AComponent::OnDraw
//   いずれも AScene::OnRender の内側なので «今の描画先» が有効である。
//
// 座標系はパスによって変わる (これは従来の CSpriteBatch と同じ規約)。
//   ・world パス (OnDrawWorld) … world 座標。1.0 は 1 world 単位で、
//     画面上の大きさは PixelsPerUnit() と camera zoom で決まる。
//   ・HUD パス (OnDrawHud)     … 画面ピクセル座標。左上が原点、Y は下向き。
//   thickness / segments の既定値も «そのパスの単位» で解釈される。
//
// 描画先が無いとき (パスの外、sprite batch 未配線の 3D シーン等) は、全ての
// 関数が何もせずに戻る。呼んでも落ちない。
// ============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/Forward.h"

namespace acs {

class IRhiTexture;

namespace game {

class FRenderContext;

/**
 * 現在の描画先 (FRenderContext) を publish する (内部用。AScene が pass 中だけ設定)。
 *
 * @details nullptr を渡すと publish を解除する。main スレッドからのみ呼ぶ。
 * @param context 現在の描画コンテキスト (解除は nullptr)。
 */
void SetDrawContext_Internal(FRenderContext* context) noexcept;

/**
 * publish 済みの描画先を返す (内部用)。
 *
 * @return 現在の FRenderContext (パスの外なら nullptr)。
 */
FRenderContext* CurrentDrawContext_Internal() noexcept;

/**
 * 今この場で描けるか (描画パスの内側で、かつ sprite batch が配線済みか) を返す。
 *
 * @return 描けるなら true。
 */
bool IsDrawing() noexcept;

/**
 * 現在の描画先の画面幅を返す。
 *
 * @return 画面幅 (ピクセル)。パス外なら 0。
 */
u32 DrawWidth() noexcept;

/**
 * 現在の描画先の画面高さを返す。
 *
 * @return 画面高さ (ピクセル)。パス外なら 0。
 */
u32 DrawHeight() noexcept;

/**
 * 単色の塗りつぶし矩形を描く。
 *
 * @param x 左上 X。
 * @param y 左上 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param color 塗りつぶし色。
 */
void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept;

/**
 * 矩形の枠線を 4 辺で描く。
 *
 * @param x 左上 X。
 * @param y 左上 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param color 線の色。
 * @param thickness 線の太さ (現パスの単位)。
 */
void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, FVec4 color,
                     f32 thickness = 1.0f) noexcept;

/**
 * 中心まわりに回転した塗りつぶし矩形を描く。
 *
 * @param cx 回転中心 X。
 * @param cy 回転中心 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param radians 回転角 (ラジアン、正で時計回り)。
 * @param color 塗りつぶし色。
 */
void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     FVec4 color) noexcept;

/**
 * 塗りつぶし円を描く ((cx,cy) は中心)。
 *
 * @details 三角形ファンで近似するので、テクスチャを用意しなくても描ける。
 * @param cx 中心 X。
 * @param cy 中心 Y。
 * @param radius 半径。
 * @param color 塗りつぶし色。
 * @param segments 円周の分割数 (3 未満は 3 に切り上げ、上限 128)。
 */
void DrawCircle(f32 cx, f32 cy, f32 radius, FVec4 color,
                u32 segments = 32u) noexcept;

/**
 * 円の枠線を描く ((cx,cy) は中心)。
 *
 * @param cx 中心 X。
 * @param cy 中心 Y。
 * @param radius 半径。
 * @param color 線の色。
 * @param thickness 線の太さ (現パスの単位)。
 * @param segments 円周の分割数 (3 未満は 3 に切り上げ、上限 128)。
 */
void DrawCircleOutline(f32 cx, f32 cy, f32 radius, FVec4 color,
                       f32 thickness = 1.0f, u32 segments = 32u) noexcept;

/**
 * 2 点を結ぶ線を描く。
 *
 * @param x0 始点 X。
 * @param y0 始点 Y。
 * @param x1 終点 X。
 * @param y1 終点 Y。
 * @param color 線の色。
 * @param thickness 線の太さ (現パスの単位)。
 */
void DrawLine(f32 x0, f32 y0, f32 x1, f32 y1, FVec4 color,
              f32 thickness = 1.0f) noexcept;

/**
 * 単色の塗りつぶし三角形を描く。
 *
 * @param x0 頂点 0 の X。
 * @param y0 頂点 0 の Y。
 * @param x1 頂点 1 の X。
 * @param y1 頂点 1 の Y。
 * @param x2 頂点 2 の X。
 * @param y2 頂点 2 の Y。
 * @param color 塗りつぶし色。
 */
void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                  FVec4 color) noexcept;

/**
 * テクスチャを元のピクセルサイズで描く。
 *
 * @param texture 描画するテクスチャ。
 * @param x 左上 X。
 * @param y 左上 Y。
 * @param tint 乗算色 (既定は白)。
 */
void DrawTexture(IRhiTexture& texture, f32 x, f32 y,
                 FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

/**
 * テクスチャを指定サイズの矩形へ描く。
 *
 * @param texture 描画するテクスチャ。
 * @param x 左上 X。
 * @param y 左上 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param tint 乗算色 (既定は白)。
 */
void DrawTexture(IRhiTexture& texture, f32 x, f32 y, f32 w, f32 h,
                 FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

/**
 * 中心まわりに回転させてテクスチャを描く。
 *
 * @param texture 描画するテクスチャ。
 * @param cx 回転中心 X。
 * @param cy 回転中心 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param radians 回転角 (ラジアン、正で時計回り)。
 * @param tint 乗算色 (既定は白)。
 */
void DrawTextureRotated(IRhiTexture& texture, f32 cx, f32 cy, f32 w, f32 h,
                        f32 radians, FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

/**
 * テクスチャの一部 (UV 0..1) を矩形へ描く。
 *
 * @param texture 描画するテクスチャ。
 * @param x 左上 X。
 * @param y 左上 Y。
 * @param w 幅。
 * @param h 高さ。
 * @param u0 サンプル領域の左 U (0..1)。
 * @param v0 サンプル領域の上 V (0..1)。
 * @param u1 サンプル領域の右 U (0..1)。
 * @param v1 サンプル領域の下 V (0..1)。
 * @param tint 乗算色 (既定は白)。
 */
void DrawTextureSub(IRhiTexture& texture, f32 x, f32 y, f32 w, f32 h,
                    f32 u0, f32 v0, f32 u1, f32 v1,
                    FVec4 tint = FVec4{1, 1, 1, 1}) noexcept;

/**
 * UTF-8 テキストを描く。
 *
 * @details フォントは CGame が game 寿命で配線したものを使う。未配線なら何もしない。
 * @param x 行の左上 X。
 * @param y 行の左上 Y。
 * @param utf8_text UTF-8 文字列 (\\n で改行)。
 * @param color 文字色 (既定は白)。
 */
void DrawString(f32 x, f32 y, const char* utf8_text,
                FVec4 color = FVec4{1, 1, 1, 1}) noexcept;

} // namespace game

/** batch を持たずに描く即時描画関数をトップレベルから参照する正規入口。 */
using game::IsDrawing;
using game::DrawWidth;
using game::DrawHeight;
using game::DrawRect;
using game::DrawRectOutline;
using game::DrawRectRotated;
using game::DrawCircle;
using game::DrawCircleOutline;
using game::DrawLine;
using game::DrawTriangle;
using game::DrawTexture;
using game::DrawTextureRotated;
using game::DrawTextureSub;
using game::DrawString;

} // namespace acs
