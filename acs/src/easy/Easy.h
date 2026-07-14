// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// ACS Easy — 初学者向けの「簡単モード」
// ----------------------------------------------------------------------------
// クラス・継承・テンプレート・エラー型を一切使わずに 2D ゲームを書くための
// API レイヤ。手続き的に「関数を呼ぶだけ」でウィンドウ・図形・画像・文字・
// 音・入力・乱数・当たり判定・カメラ・セーブが扱える。
//
// 最小のプログラム:
//
//   #include "easy/Easy.h"
//   using namespace acs::easy;
//
//   int main() {
//       OpenWindow(1280, 720, "はじめてのゲーム");
//       float x = 600;
//       while (NextFrame()) {                 // ウィンドウを閉じると false
//           if (IsKeyDown(EKey::Right)) x += 5;
//           if (IsKeyDown(EKey::Left))  x -= 5;
//           DrawRect(x, 320, 80, 80, FColor::Sky);
//       }
//   }
//
// 使い方のルール:
//   ・ゲームの状態は main の中のローカル変数でよい（グローバル変数は不要）。
//   ・描画関数は while ループの中（NextFrame() を呼んだ後）で呼ぶこと。
//   ・すべての関数を 1 本のスレッド（メインスレッド）から呼ぶこと。
//
// 座標と角度の約束:
//   ・座標は左上が原点、ピクセル単位、Y は下向き。
//   ・**位置 (x,y) はすべて図形・画像の「左上」**。回転する描画も left-top で
//     位置を指定し、回転は図形自身の中心まわりに行われる。
//   ・円だけは例外で (x,y) は中心。
//   ・回転角はすべて度（°）。正の値は時計回り。
//
// 補足:
//   ・型名 f32 は float、i32 は int、u8 は 0〜255 の整数の別名。引数には
//     ふつうに float や int を渡せばよい。
//   ・実行するとゲーム画面とは別に黒いコンソール窓が出る。これはログ表示用で
//     正常。ゲーム画面を閉じれば一緒に終了する。
//
// もっと本格的に作りたくなったら、acs::FApplication を直接使う方法へ進める
// （docs/QUICKSTART.md 参照）。easy はその入口に過ぎない。
// ============================================================================
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"
// 以下はジョブ/並列 API の「テンプレート実装」に必要 (初学者は意識しなくてよい)。
#include "foundation/SourceLoc.h"
#include "memory/Memory.h"            // DefaultAllocator
#include "memory/Allocator.h"         // FAllocator
#include "threading/ThreadPool.h"     // FThreadPool / Task / CompletionCounter
#include "threading/JobGraph.h"       // FJobGraph (依存グラフ)

namespace acs::easy {

/** acs::EKey をこの名前空間でも使えるよう再公開する。 */
using acs::EKey;

/** acs::EMouseButton をこの名前空間でも使えるよう再公開する。 */
using acs::EMouseButton;

/** acs::EGamepadButton をこの名前空間でも使えるよう再公開する。 */
using acs::EGamepadButton;

/**
 * RGBA 色 (各成分 0.0〜1.0)。
 *
 * @details
 * FColor::Red などの定数を使うか、Rgb(255,0,0) / Rgba(...) で作る。a は不透明度
 * (0=透明, 1=不透明)。
 */
struct FColor {
    /** 赤成分 (0.0〜1.0)。 */
    f32 r = 0.0f;

    /** 緑成分 (0.0〜1.0)。 */
    f32 g = 0.0f;

    /** 青成分 (0.0〜1.0)。 */
    f32 b = 0.0f;

    /** 不透明度 (0=透明, 1=不透明)。 */
    f32 a = 1.0f;

    /** 赤の定数色。 */
    static const FColor Red;

    /** 緑の定数色。 */
    static const FColor Green;

    /** 青の定数色。 */
    static const FColor Blue;

    /** 黄の定数色。 */
    static const FColor Yellow;

    /** シアンの定数色。 */
    static const FColor Cyan;

    /** マゼンタの定数色。 */
    static const FColor Magenta;

    /** 白の定数色。 */
    static const FColor White;

    /** 黒の定数色。 */
    static const FColor Black;

    /** 灰の定数色。 */
    static const FColor Gray;

    /** 橙の定数色。 */
    static const FColor Orange;

    /** 空色の定数色。 */
    static const FColor Sky;

    /** 完全透明 (全成分 0) の定数色。 */
    static const FColor Clear;
};

/**
 * 0〜255 の整数で不透明色を作る。
 *
 * @param r 赤成分 (0〜255)。
 * @param g 緑成分 (0〜255)。
 * @param b 青成分 (0〜255)。
 * @return 各成分を 0.0〜1.0 に変換した不透明な FColor。
 */
FColor Rgb (u8 r, u8 g, u8 b) noexcept;

/**
 * 0〜255 の整数で半透明込みの色を作る。
 *
 * @param r 赤成分 (0〜255)。
 * @param g 緑成分 (0〜255)。
 * @param b 青成分 (0〜255)。
 * @param a 不透明度 (0〜255)。
 * @return 各成分を 0.0〜1.0 に変換した FColor。
 */
FColor Rgba(u8 r, u8 g, u8 b, u8 a) noexcept;

/**
 * 不透明度だけを変えた色を返す (半透明描画やフェードに使う)。
 *
 * @param color 元の色。
 * @param alpha 設定する不透明度 (0〜1 にクランプされる)。
 * @return alpha を差し替えた FColor。
 */
FColor Fade(FColor color, f32 alpha) noexcept;

/**
 * LoadSprite が返す画像ハンドル (コピー可能な軽量値型、id==0 は無効)。
 */
struct Sprite {
    /** スプライトスロット番号 (1 始まり、0 は無効)。 */
    u32 id = 0;
};

/**
 * LoadSound が返す音声ハンドル (コピー可能な軽量値型、id==0 は無効)。
 */
struct Sound {
    /** サウンドスロット番号 (1 始まり、0 は無効)。 */
    u32 id = 0;
};

/**
 * ウィンドウを開いてゲームを初期化する (プログラムの最初に 1 回だけ呼ぶ)。
 *
 * @details
 * 初期化に失敗した場合はコンソールにエラーを出し、最初の NextFrame() が false を
 * 返す (ゲームは画面を出さずに静かに終了する)。
 * @param width ウィンドウ幅 (0 以下なら既定 1280)。
 * @param height ウィンドウ高さ (0 以下なら既定 720)。
 * @param title ウィンドウタイトル (UTF-8)。
 */
void OpenWindow(i32 width = 1280, i32 height = 720,
                const char* title = "ACS FGame") noexcept;

/**
 * 1 フレーム進める (while の条件に使う)。
 *
 * @details
 * 直前のフレームを画面に出し、次のフレームの準備をして true を返す。ウィンドウが
 * 閉じられたら後始末をして false を返す。
 * @return 続行するなら true、終了したなら false。
 */
bool NextFrame() noexcept;

/** ゲームを終了する (次の NextFrame() が false を返すようになる)。 */
void Quit() noexcept;

/**
 * ウィンドウのタイトル文字列を変える。
 *
 * @param title 新しいタイトル (UTF-8)。
 */
void SetWindowTitle(const char* title) noexcept;

/**
 * 背景色を設定する (次のフレームから反映)。既定は濃い紺色。
 *
 * @param color 新しい背景色。
 */
void SetBackground(FColor color) noexcept;

/**
 * 全画面表示 (ボーダーレス) の ON/OFF を要求する (次のフレームから反映)。
 *
 * @param on true で全画面、false で窓表示。
 */
void SetFullscreen(bool on) noexcept;

/** 全画面と窓表示を切り替える (次のフレームから反映)。 */
void ToggleFullscreen() noexcept;

/**
 * 現在全画面表示かを返す。
 *
 * @return 全画面なら true。
 */
bool IsFullscreen() noexcept;

/**
 * コンソール (コマンドプロンプト) ウィンドウの表示/非表示を切り替える。
 *
 * @details ゲームウィンドウとは別に開く黒いコンソール窓を隠したり、デバッグ時に
 *          再表示したりできる。コンソールが無い (GUI サブシステム) ビルドでは何もしない。
 * @param show true で表示、false で非表示。
 */
void ShowConsole(bool show) noexcept;

/** コンソール (コマンドプロンプト) ウィンドウの表示/非表示を反転する。 */
void ToggleConsole() noexcept;

/**
 * コンソール (コマンドプロンプト) ウィンドウが現在表示されているかを返す。
 *
 * @return 表示中なら true。コンソールが無いビルドでは false。
 */
bool IsConsoleVisible() noexcept;

/**
 * 塗りつぶし長方形を描く (OpenWindow の後、while(NextFrame()) ループ内で呼ぶ)。
 *
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param width 幅。
 * @param height 高さ。
 * @param color 塗り色。
 */
void DrawRect(f32 x, f32 y, f32 width, f32 height, FColor color) noexcept;

/**
 * 長方形の枠線を描く。
 *
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param width 幅。
 * @param height 高さ。
 * @param color 線色。
 * @param thickness 線の太さ (1 未満は 1 に丸められる)。
 */
void DrawRectOutline(f32 x, f32 y, f32 width, f32 height,
                     FColor color, f32 thickness = 2.0f) noexcept;

/**
 * 回転した塗りつぶし長方形を描く。
 *
 * @details (x,y) は回転前の左上、回転は図形の中心まわりに行う。
 * @param x 回転前の左上の X 座標。
 * @param y 回転前の左上の Y 座標。
 * @param width 幅。
 * @param height 高さ。
 * @param degrees 回転角 (度、時計回りが正)。
 * @param color 塗り色。
 */
void DrawRectRotated(f32 x, f32 y, f32 width, f32 height,
                     f32 degrees, FColor color) noexcept;

/**
 * 塗りつぶし円を描く。
 *
 * @details 円だけは (x,y) が中心を指す (他の図形は左上)。
 * @param x 中心の X 座標。
 * @param y 中心の Y 座標。
 * @param radius 半径。
 * @param color 塗り色。
 */
void DrawCircle(f32 x, f32 y, f32 radius, FColor color) noexcept;

/**
 * 円の枠線を描く。
 *
 * @details (x,y) は中心。内部は円周に沿って小さな点を並べて描画する。
 * @param x 中心の X 座標。
 * @param y 中心の Y 座標。
 * @param radius 半径。
 * @param color 線色。
 * @param thickness 線の太さ (1 未満は 1 に丸められる)。
 */
void DrawCircleOutline(f32 x, f32 y, f32 radius,
                       FColor color, f32 thickness = 2.0f) noexcept;

/**
 * 2 点を結ぶ線分を描く。
 *
 * @param x1 始点の X 座標。
 * @param y1 始点の Y 座標。
 * @param x2 終点の X 座標。
 * @param y2 終点の Y 座標。
 * @param color 線色。
 * @param thickness 線の太さ (1 未満は 1 に丸められる)。
 */
void DrawLine(f32 x1, f32 y1, f32 x2, f32 y2,
              FColor color, f32 thickness = 2.0f) noexcept;

/**
 * 塗りつぶし三角形を描く。
 *
 * @param x1 頂点 1 の X 座標。
 * @param y1 頂点 1 の Y 座標。
 * @param x2 頂点 2 の X 座標。
 * @param y2 頂点 2 の Y 座標。
 * @param x3 頂点 3 の X 座標。
 * @param y3 頂点 3 の Y 座標。
 * @param color 塗り色。
 */
void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                  FColor color) noexcept;

/**
 * 三角形の枠線を描く (3 辺を線分で結ぶ)。
 *
 * @param x1 頂点 1 の X 座標。
 * @param y1 頂点 1 の Y 座標。
 * @param x2 頂点 2 の X 座標。
 * @param y2 頂点 2 の Y 座標。
 * @param x3 頂点 3 の X 座標。
 * @param y3 頂点 3 の Y 座標。
 * @param color 線色。
 * @param thickness 線の太さ (1 未満は 1 に丸められる)。
 */
void DrawTriangleOutline(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                         FColor color, f32 thickness = 2.0f) noexcept;

/**
 * 1 ピクセルの点を描く。
 *
 * @param x X 座標。
 * @param y Y 座標。
 * @param color 色。
 */
void DrawPixel(f32 x, f32 y, FColor color) noexcept;

/**
 * スプライトを元サイズで描く。
 *
 * @param sprite 描画するスプライト。
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 */
void DrawSprite(Sprite sprite, f32 x, f32 y) noexcept;

/**
 * スプライトを指定サイズに伸縮して描く。
 *
 * @param sprite 描画するスプライト。
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param width 描画幅。
 * @param height 描画高さ。
 */
void DrawSprite(Sprite sprite, f32 x, f32 y, f32 width, f32 height) noexcept;

/**
 * スプライトを回転 (+拡縮・色掛け) して描く。
 *
 * @details (x,y) は回転前の左上、回転は画像の中心まわりに行う。
 * @param sprite 描画するスプライト。
 * @param x 回転前の左上の X 座標。
 * @param y 回転前の左上の Y 座標。
 * @param degrees 回転角 (度、時計回りが正)。
 * @param scale 拡大率 (1.0 で等倍)。
 * @param tint 乗算する色掛け (白で無加工)。
 */
void DrawSpriteRotated(Sprite sprite, f32 x, f32 y, f32 degrees,
                       f32 scale = 1.0f, FColor tint = FColor{1,1,1,1}) noexcept;

/**
 * スプライトに色を掛けて描く。
 *
 * @param sprite 描画するスプライト。
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param tint 乗算する色掛け。
 */
void DrawSpriteTinted(Sprite sprite, f32 x, f32 y, FColor tint) noexcept;

/**
 * スプライトを左右・上下反転して描く。
 *
 * @param sprite 描画するスプライト。
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param flip_x 左右反転するなら true。
 * @param flip_y 上下反転するなら true。
 */
void DrawSpriteFlipped(Sprite sprite, f32 x, f32 y,
                       bool flip_x, bool flip_y) noexcept;

/**
 * スプライトの一部分を切り出して描く (スプライトシート用)。
 *
 * @param sprite 描画するスプライト。
 * @param x 描画先左上の X 座標。
 * @param y 描画先左上の Y 座標。
 * @param width 描画幅。
 * @param height 描画高さ。
 * @param src_x 切り出し元の X 座標 (元画像のピクセル単位)。
 * @param src_y 切り出し元の Y 座標。
 * @param src_width 切り出し元の幅。
 * @param src_height 切り出し元の高さ。
 */
void DrawSpritePart(Sprite sprite, f32 x, f32 y, f32 width, f32 height,
                    f32 src_x, f32 src_y, f32 src_width, f32 src_height) noexcept;

/**
 * スプライトの元画像の幅を返す。
 *
 * @param sprite 対象スプライト。
 * @return 元画像の幅 (無効なら 0)。
 */
f32 SpriteWidth (Sprite sprite) noexcept;

/**
 * スプライトの元画像の高さを返す。
 *
 * @param sprite 対象スプライト。
 * @return 元画像の高さ (無効なら 0)。
 */
f32 SpriteHeight(Sprite sprite) noexcept;

/**
 * 文字列を既定サイズで描く (UTF-8、日本語可。\n で改行)。
 *
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param text 描画する文字列 (UTF-8)。
 * @param color 文字色。
 */
void DrawString(f32 x, f32 y, const char* text, FColor color) noexcept;

/**
 * 文字列を指定ピクセルサイズで描く。
 *
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param text 描画する文字列 (UTF-8)。
 * @param color 文字色。
 * @param size 文字サイズ (ピクセル)。
 */
void DrawString(f32 x, f32 y, const char* text, FColor color, f32 size) noexcept;

/**
 * 文字列を center_x で中央そろえして既定サイズで描く。
 *
 * @param center_x 中央そろえの基準となる X 座標。
 * @param y 左上の Y 座標。
 * @param text 描画する文字列 (UTF-8)。
 * @param color 文字色。
 */
void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color) noexcept;

/**
 * 文字列を center_x で中央そろえして指定サイズで描く。
 *
 * @param center_x 中央そろえの基準となる X 座標。
 * @param y 左上の Y 座標。
 * @param text 描画する文字列 (UTF-8)。
 * @param color 文字色。
 * @param size 文字サイズ (ピクセル)。
 */
void DrawStringCentered(f32 center_x, f32 y, const char* text,
                        FColor color, f32 size) noexcept;

/**
 * 既定サイズで描いたときの文字列の幅を返す。
 *
 * @param text 計測する文字列 (UTF-8)。
 * @return 文字列の幅 (ピクセル)。
 */
f32 TextWidth(const char* text) noexcept;

/**
 * 指定サイズで描いたときの文字列の幅を返す。
 *
 * @param text 計測する文字列 (UTF-8)。
 * @param size 文字サイズ (ピクセル)。
 * @return 文字列の幅 (ピクセル)。
 */
f32 TextWidth(const char* text, f32 size) noexcept;

/**
 * 既定サイズの 1 行の高さを返す。
 *
 * @return 行の高さ (ピクセル)。
 */
f32 TextHeight() noexcept;

/**
 * 指定サイズの 1 行の高さを返す。
 *
 * @param size 文字サイズ (ピクセル)。
 * @return 行の高さ (ピクセル)。
 */
f32 TextHeight(f32 size) noexcept;

// カメラは「ワールド」と「画面」を分ける。SetCamera 後に描く図形はワールド
// 座標として扱われ、カメラの位置・ズームに応じて画面に映る。カメラ設定は
// **毎フレーム呼ぶ**こと（描画関数と同じ。呼ばなければカメラ無し）。

/**
 * (x,y) のワールド座標が画面中央に映るようカメラを置く。
 *
 * @param x 画面中央に映すワールド X 座標。
 * @param y 画面中央に映すワールド Y 座標。
 */
void SetCamera(f32 x, f32 y) noexcept;

/**
 * カメラのズーム倍率を設定する。
 *
 * @param zoom ズーム倍率 (1.0 で等倍、2.0 で 2 倍に拡大。0.01 未満は丸められる)。
 */
void SetCameraZoom(f32 zoom) noexcept;

/** カメラを初期状態 (スクロール無し・ズーム 1.0) に戻す (HUD を描く前などに)。 */
void ResetCamera() noexcept;

/**
 * 現在のカメラ中心 X 座標を返す。
 *
 * @return カメラ中心の X 座標 (ワールド座標)。
 */
f32  CameraX() noexcept;

/**
 * 現在のカメラ中心 Y 座標を返す。
 *
 * @return カメラ中心の Y 座標 (ワールド座標)。
 */
f32  CameraY() noexcept;

/**
 * マウス位置をワールド座標で返す (カメラのスクロール・ズームを反映)。
 *
 * @return マウスのワールド X 座標。
 */
f32  MouseWorldX() noexcept;

/**
 * マウス位置をワールド座標で返す (カメラのスクロール・ズームを反映)。
 *
 * @return マウスのワールド Y 座標。
 */
f32  MouseWorldY() noexcept;

/**
 * 描画をこの矩形の中だけに制限する (画面座標、カメラの影響を受けない)。
 *
 * @param x クリップ矩形の左上 X 座標。
 * @param y クリップ矩形の左上 Y 座標。
 * @param width クリップ矩形の幅。
 * @param height クリップ矩形の高さ。
 */
void SetClipRect(f32 x, f32 y, f32 width, f32 height) noexcept;

/** クリップを解除して画面全体に描けるようにする。 */
void ClearClipRect() noexcept;

/**
 * 紙が燃えて消える per-pixel ディゾルブを矩形に重ねる (どのバックエンドでも動く)。
 *
 * @details 描画内容を先に描いてからこれを重ねると、progress 0→1 で燃え際 (白熱→橙→
 *          黒コゲ) が進みながら下の内容が現れる。セル/ドットでなくピクセル単位なので高解像度。
 * @param x,y 矩形左上 (px)。
 * @param w,h 矩形サイズ (px)。
 * @param progress 0=全部紙, 1=全部燃えて下が見える。
 * @param ember 燃え際の基準色。
 * @param paper 紙 (覆い) の色。
 * @param edge 燃え際の帯幅 (既定 0.12)。
 * @param freq ノイズ周波数 (既定 7。大きいほど細かい炎縁)。
 * @param time 炎ゆらぎ用の時間 (0 で静止)。
 * @param cells ドット調の分割数 (0=なめらか最高解像度、N=N分割の四角ドット)。
 * @return 描画できたら true、効果が使えなければ false。
 */
bool DrawBurnDissolve(f32 x, f32 y, f32 w, f32 h, f32 progress,
                      FColor ember = FColor{ 1.0f, 0.45f, 0.06f, 1.0f },
                      FColor paper = FColor{ 0.88f, 0.84f, 0.74f, 1.0f },
                      f32 edge = 0.12f, f32 freq = 7.0f, f32 time = 0.0f,
                      f32 cells = 0.0f) noexcept;

// Bloom（発光）・ビネット・カラーグレーディング等、画面全体にかかる効果。
// **Diligent バックエンドでビルドした場合のみ有効**（DX12 raw ビルドでは
// 各関数は無視される）。IsPostProcessAvailable() で利用可否を確認できる。
// 効果は既定ですべて無効。下の Set 関数で必要なものだけ有効化する。
// 注: 有効時は文字も含めて画面全体に効果がかかる。

/**
 * ポストプロセスが利用可能かを返す。
 *
 * @return Diligent バックエンドで初期化に成功していれば true (DX12 raw では false)。
 */
bool IsPostProcessAvailable() noexcept;

/**
 * Bloom (発光) の強さを設定する。
 *
 * @param intensity 発光の強さ (0=無効、0.6 が目安)。
 */
void SetBloom(f32 intensity) noexcept;

/**
 * Bloom が発光させる明るさの閾値を設定する。
 *
 * @param threshold 発光させる明るさの閾値 (負値は 0 に丸められる)。
 */
void SetBloomThreshold(f32 threshold) noexcept;

/**
 * 露出 (画面全体の明るさ) を設定する。
 *
 * @param exposure 露出 (1.0=標準、負値は 0 に丸められる)。
 */
void SetExposure(f32 exposure) noexcept;

/**
 * ビネット (画面端の暗化) の強さを設定する。
 *
 * @param intensity ビネットの強さ (0〜1、0=無効)。
 */
void SetVignette(f32 intensity) noexcept;

/**
 * 色収差の量を設定する。
 *
 * @param amount 色収差の量 (0=無効、0.002 が目安)。
 */
void SetChromaticAberration(f32 amount) noexcept;

/**
 * フィルムグレイン (フィルムノイズ) の強さを設定する。
 *
 * @param intensity ノイズの強さ (0=無効)。
 */
void SetFilmGrain(f32 intensity) noexcept;

/**
 * カラーグレーディング (彩度・コントラスト・色温度) を設定する。
 *
 * @param saturation 彩度 (1=標準)。
 * @param contrast コントラスト (1=標準)。
 * @param temperature 色温度 (-1=青寄り〜+1=橙寄り)。
 */
void SetColorGrading(f32 saturation, f32 contrast, f32 temperature) noexcept;

/**
 * 輪郭強調 (シャープネス) の強さを設定する。
 *
 * @param strength 輪郭強調の強さ (0〜1、0=無効)。
 */
void SetSharpness(f32 strength) noexcept;

/**
 * トーンマップの方式を設定する。
 *
 * @param mode トーンマップ方式 (0=ACES、1=AgX、2=Reinhard)。
 */
void SetTonemap(i32 mode) noexcept;

/**
 * 自動露出 (明るさの自動調整) の ON/OFF を設定する。
 *
 * @param enabled 有効にするなら true。
 */
void SetAutoExposure(bool enabled) noexcept;

// 同じパスを再度渡しても読み直さず同じハンドルを返す。失敗してもプログラムは
// 落ちない（無効ハンドルを返し、描画/再生は無視）。パスは実行時のカレント
// ディレクトリからの相対。見つからないときは絶対パスを使うか実行ファイルの
// 隣に素材を置くこと。

/**
 * 画像を読み込んでスプライトを返す (OpenWindow の後で呼ぶ)。
 *
 * @details 同じパスは読み直さず同じハンドルを返す。失敗しても落ちず無効ハンドルを返す。
 * @param path 画像ファイルのパス (カレントディレクトリからの相対も可)。
 * @return 読み込んだスプライト (失敗時は id==0 の無効ハンドル)。
 */
Sprite LoadSprite(const char* path) noexcept;

/**
 * 音声を読み込んでサウンドを返す (OpenWindow の後で呼ぶ)。
 *
 * @details 同じパスは読み直さず同じハンドルを返す。失敗しても落ちず無効ハンドルを返す。
 * @param path 音声ファイルのパス (カレントディレクトリからの相対も可)。
 * @return 読み込んだサウンド (失敗時は id==0 の無効ハンドル)。
 */
Sound  LoadSound (const char* path) noexcept;

/**
 * 効果音を 1 回再生する。
 *
 * @param sound 再生するサウンド。
 */
void Play(Sound sound) noexcept;

/**
 * 効果音を音量指定で 1 回再生する。
 *
 * @param sound 再生するサウンド。
 * @param volume 音量 (0.0〜1.0)。
 */
void Play(Sound sound, f32 volume) noexcept;

/**
 * 音をループ再生する (BGM 用)。
 *
 * @param sound 再生するサウンド。
 */
void PlayLoop(Sound sound) noexcept;

/**
 * 音を音量指定でループ再生する (BGM 用)。
 *
 * @details 同じサウンドが既にループ中なら、二重ループを防ぐため先に止めてから鳴らす。
 * @param sound 再生するサウンド。
 * @param volume 音量 (0.0〜1.0)。
 */
void PlayLoop(Sound sound, f32 volume) noexcept;

/**
 * そのサウンドのループ再生を止める。
 *
 * @param sound 止めるサウンド。
 */
void StopSound(Sound sound) noexcept;

/** 鳴っている音をすべて止める。 */
void StopAllSounds() noexcept;

/**
 * 全体音量 (マスターボリューム) を設定する (音量スライダー用)。
 *
 * @param volume マスター音量 (0.0〜1.0)。
 */
void SetMasterVolume(f32 volume) noexcept;

/** 鳴っている音をすべて一時停止する (再生位置は保たれる)。 */
void PauseAllSounds() noexcept;

/** 一時停止した音をすべて再開する。 */
void ResumeAllSounds() noexcept;

/**
 * キーが押されている間ずっと true を返す。
 *
 * @param key 調べるキー。
 * @return 押されているなら true。
 */
bool IsKeyDown    (EKey key) noexcept;

/**
 * キーを押した瞬間のフレームだけ true を返す。
 *
 * @param key 調べるキー。
 * @return 押した瞬間なら true。
 */
bool IsKeyPressed (EKey key) noexcept;

/**
 * キーを離した瞬間のフレームだけ true を返す。
 *
 * @param key 調べるキー。
 * @return 離した瞬間なら true。
 */
bool IsKeyReleased(EKey key) noexcept;

/**
 * このフレームに入力された文字を返す (名前入力などに使う)。
 *
 * @details UTF-8、IME 確定後の文字列。無ければ ""。バックスペースや Enter は
 * IsKeyPressed(EKey::Backspace / EKey::Enter) で取る。
 * @return 入力文字列 (無ければ空文字列)。
 */
const char* TextInput() noexcept;

/**
 * マウスの X 座標 (ウィンドウ内ピクセル=画面座標) を返す。
 *
 * @return マウスの X 座標。
 */
f32  MouseX() noexcept;

/**
 * マウスの Y 座標 (ウィンドウ内ピクセル=画面座標) を返す。
 *
 * @return マウスの Y 座標。
 */
f32  MouseY() noexcept;

/**
 * マウスボタンが押されている間ずっと true を返す。
 *
 * @param button 調べるマウスボタン (省略時は左)。
 * @return 押されているなら true。
 */
bool IsMouseDown    (EMouseButton button = EMouseButton::Left) noexcept;

/**
 * マウスボタンを押した瞬間のフレームだけ true を返す。
 *
 * @param button 調べるマウスボタン (省略時は左)。
 * @return 押した瞬間なら true。
 */
bool IsMousePressed (EMouseButton button = EMouseButton::Left) noexcept;

/**
 * マウスボタンを離した瞬間のフレームだけ true を返す。
 *
 * @param button 調べるマウスボタン (省略時は左)。
 * @return 離した瞬間なら true。
 */
bool IsMouseReleased(EMouseButton button = EMouseButton::Left) noexcept;

/**
 * マウスホイールの回転量を返す。
 *
 * @return ホイール回転量 (奥+ / 手前-)。
 */
f32  MouseWheel() noexcept;

/**
 * ゲームパッドが接続されているかを返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return 接続されていれば true。
 */
bool IsGamepadConnected(i32 player = 0) noexcept;

/**
 * ゲームパッドのボタンが押されている間ずっと true を返す。
 *
 * @param button 調べるボタン。
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return 押されているなら true。
 */
bool IsGamepadDown   (EGamepadButton button, i32 player = 0) noexcept;

/**
 * ゲームパッドのボタンを押した瞬間のフレームだけ true を返す。
 *
 * @param button 調べるボタン。
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return 押した瞬間なら true。
 */
bool IsGamepadPressed(EGamepadButton button, i32 player = 0) noexcept;

/**
 * 左スティックの X 軸値を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return スティック X 軸値 (-1.0〜+1.0)。
 */
f32  GamepadLeftX (i32 player = 0) noexcept;

/**
 * 左スティックの Y 軸値を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return スティック Y 軸値 (-1.0〜+1.0)。
 */
f32  GamepadLeftY (i32 player = 0) noexcept;

/**
 * 右スティックの X 軸値を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return スティック X 軸値 (-1.0〜+1.0)。
 */
f32  GamepadRightX(i32 player = 0) noexcept;

/**
 * 右スティックの Y 軸値を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return スティック Y 軸値 (-1.0〜+1.0)。
 */
f32  GamepadRightY(i32 player = 0) noexcept;

/**
 * 左トリガーの押し込み量を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return トリガー値 (0.0〜1.0)。
 */
f32  GamepadLeftTrigger (i32 player = 0) noexcept;

/**
 * 右トリガーの押し込み量を返す。
 *
 * @param player プレイヤー番号 (0〜3、省略時は 0)。
 * @return トリガー値 (0.0〜1.0)。
 */
f32  GamepadRightTrigger(i32 player = 0) noexcept;

/**
 * min 以上 max 以下の整数乱数を返す。
 *
 * @details min > max なら内部で入れ替える。剰余バイアスを棄却して一様にする。
 * @param min 下限 (含む)。
 * @param max 上限 (含む)。
 * @return [min, max] の一様乱数。
 */
i32  RandomInt(i32 min, i32 max) noexcept;

/**
 * min 以上 max 未満の小数乱数を返す。
 *
 * @param min 下限 (含む)。
 * @param max 上限 (含まない)。
 * @return [min, max) の乱数。
 */
f32  RandomFloat(f32 min, f32 max) noexcept;

/**
 * true / false をランダムに返す。
 *
 * @return ランダムな真偽値。
 */
bool RandomBool() noexcept;

/**
 * 乱数の種を固定する (同じ種なら同じ並びが出る)。
 *
 * @param seed 設定する種 (0 を渡すと既定値に置き換える)。
 */
void RandomSeed(u32 seed) noexcept;

/**
 * 2 つの矩形が重なっているかを返す。
 *
 * @param x1 矩形 1 の左上 X 座標。
 * @param y1 矩形 1 の左上 Y 座標。
 * @param w1 矩形 1 の幅。
 * @param h1 矩形 1 の高さ。
 * @param x2 矩形 2 の左上 X 座標。
 * @param y2 矩形 2 の左上 Y 座標。
 * @param w2 矩形 2 の幅。
 * @param h2 矩形 2 の高さ。
 * @return 重なっていれば true。
 */
bool RectsOverlap(f32 x1, f32 y1, f32 w1, f32 h1,
                  f32 x2, f32 y2, f32 w2, f32 h2) noexcept;

/**
 * 2 つの円が重なっているかを返す。
 *
 * @param x1 円 1 の中心 X 座標。
 * @param y1 円 1 の中心 Y 座標。
 * @param r1 円 1 の半径。
 * @param x2 円 2 の中心 X 座標。
 * @param y2 円 2 の中心 Y 座標。
 * @param r2 円 2 の半径。
 * @return 重なっていれば true。
 */
bool CirclesOverlap(f32 x1, f32 y1, f32 r1,
                    f32 x2, f32 y2, f32 r2) noexcept;

/**
 * 点が矩形の内側にあるかを返す。
 *
 * @param px 点の X 座標。
 * @param py 点の Y 座標。
 * @param x 矩形の左上 X 座標。
 * @param y 矩形の左上 Y 座標。
 * @param w 矩形の幅。
 * @param h 矩形の高さ。
 * @return 内側なら true。
 */
bool PointInRect(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) noexcept;

/**
 * 点が円の内側にあるかを返す。
 *
 * @param px 点の X 座標。
 * @param py 点の Y 座標。
 * @param cx 円の中心 X 座標。
 * @param cy 円の中心 Y 座標。
 * @param r 円の半径。
 * @return 内側なら true。
 */
bool PointInCircle(f32 px, f32 py, f32 cx, f32 cy, f32 r) noexcept;

/**
 * 値を [lo, hi] の範囲に収める。
 *
 * @param value 対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return クランプした値。
 */
f32 Clamp(f32 value, f32 lo, f32 hi) noexcept;

/**
 * a と b を t で線形補間する。
 *
 * @param a 開始値。
 * @param b 終了値。
 * @param t 補間係数 (0 で a、1 で b)。
 * @return 補間結果。
 */
f32 Lerp (f32 a, f32 b, f32 t) noexcept;

/**
 * 2 点間のユークリッド距離を返す。
 *
 * @param x1 点 1 の X 座標。
 * @param y1 点 1 の Y 座標。
 * @param x2 点 2 の X 座標。
 * @param y2 点 2 の Y 座標。
 * @return 2 点間の距離。
 */
f32 Distance(f32 x1, f32 y1, f32 x2, f32 y2) noexcept;

/**
 * 2 値の小さいほうを返す。
 *
 * @param a 比較する値 1。
 * @param b 比較する値 2。
 * @return 小さいほうの値。
 */
f32 Min(f32 a, f32 b) noexcept;

/**
 * 2 値の大きいほうを返す。
 *
 * @param a 比較する値 1。
 * @param b 比較する値 2。
 * @return 大きいほうの値。
 */
f32 Max(f32 a, f32 b) noexcept;

/**
 * 絶対値を返す。
 *
 * @param value 対象の値。
 * @return 絶対値。
 */
f32 Abs(f32 value) noexcept;

/**
 * 角度 (度) の正弦を返す。
 *
 * @details 角度は度 (°) で渡す (C++ 標準の sin はラジアンなので注意)。
 * @param degrees 角度 (度)。
 * @return 正弦値。
 */
f32 Sin(f32 degrees) noexcept;

/**
 * 角度 (度) の余弦を返す。
 *
 * @details 角度は度 (°) で渡す (C++ 標準の cos はラジアンなので注意)。
 * @param degrees 角度 (度)。
 * @return 余弦値。
 */
f32 Cos(f32 degrees) noexcept;

/**
 * 平方根を返す。
 *
 * @param value 対象の値。
 * @return 平方根。
 */
f32 Sqrt(f32 value) noexcept;

/**
 * (x1,y1) から (x2,y2) へ向かう向きの角度 (度) を返す。
 *
 * @details 戻り値は DrawSpriteRotated に渡せる (右が 0°、時計回りが正)。
 * @param x1 始点の X 座標。
 * @param y1 始点の Y 座標。
 * @param x2 終点の X 座標。
 * @param y2 終点の Y 座標。
 * @return 向きの角度 (度)。
 */
f32 AngleTo(f32 x1, f32 y1, f32 x2, f32 y2) noexcept;

// save.dat に key-value で保存する。ハイスコアや設定の永続化に使う。
// OpenWindow の前でも後でも呼べる。

/**
 * 整数値をキーに紐づけて保存する。
 *
 * @param key 保存キー。
 * @param value 保存する整数値。
 */
void SaveInt   (const char* key, i32 value) noexcept;

/**
 * キーに紐づく整数値を読み込む。
 *
 * @param key 読み込むキー。
 * @param default_value キーが無い・パース不可のときに返す値。
 * @return 保存値、無ければ default_value。
 */
i32  LoadInt   (const char* key, i32 default_value) noexcept;

/**
 * 小数値をキーに紐づけて保存する。
 *
 * @param key 保存キー。
 * @param value 保存する小数値。
 */
void SaveFloat (const char* key, f32 value) noexcept;

/**
 * キーに紐づく小数値を読み込む。
 *
 * @param key 読み込むキー。
 * @param default_value キーが無い・パース不可のときに返す値。
 * @return 保存値、無ければ default_value。
 */
f32  LoadFloat (const char* key, f32 default_value) noexcept;

/**
 * 文字列値をキーに紐づけて保存する。
 *
 * @details 値に改行 (\n) は使えない。
 * @param key 保存キー。
 * @param value 保存する文字列 (nullptr は空文字列として扱う)。
 */
void SaveString(const char* key, const char* value) noexcept;

/**
 * キーに紐づく文字列値を読み込む。
 *
 * @details 戻り値は次に Save 系を呼ぶまで有効。
 * @param key 読み込むキー。
 * @param default_value キーが無いときに返す値。
 * @return 保存値へのポインタ、無ければ default_value。
 */
const char* LoadString(const char* key, const char* default_value) noexcept;

/**
 * そのキーが保存済みかを返す。
 *
 * @param key 調べるキー。
 * @return 保存済みなら true。
 */
bool HasSaveKey(const char* key) noexcept;

/** 保存内容をすべて消去する (save.dat も空にする)。 */
void DeleteAllSaves() noexcept;

/**
 * 前フレームからの経過秒を返す。
 *
 * @return デルタタイム (秒)。
 */
f32  DeltaTime()   noexcept;

/**
 * OpenWindow からの累積秒を返す。
 *
 * @return 経過時間 (秒)。
 */
f32  ElapsedTime() noexcept;

/**
 * おおよその 1 秒あたりフレーム数を返す。
 *
 * @return 平滑化した FPS。
 */
i32  Fps()         noexcept;

/**
 * 画面 (ウィンドウ) の幅を返す。
 *
 * @return 画面幅 (ピクセル)。
 */
f32  ScreenWidth()  noexcept;

/**
 * 画面 (ウィンドウ) の高さを返す。
 *
 * @return 画面高さ (ピクセル)。
 */
f32  ScreenHeight() noexcept;

/**
 * 小数点以下を切り捨てる。
 *
 * @param v 対象の値。
 * @return 切り捨てた値。
 */
f32 Floor(f32 v) noexcept;

/**
 * 小数点以下を切り上げる。
 *
 * @param v 対象の値。
 * @return 切り上げた値。
 */
f32 Ceil (f32 v) noexcept;

/**
 * 四捨五入する。
 *
 * @param v 対象の値。
 * @return 四捨五入した値。
 */
f32 Round(f32 v) noexcept;

/**
 * 符号を返す。
 *
 * @param v 対象の値。
 * @return 正で +1、負で -1、0 で 0。
 */
f32 Sign (f32 v) noexcept;

/**
 * 累乗を返す。
 *
 * @param base 底。
 * @param exponent 指数。
 * @return base の exponent 乗。
 */
f32 Pow  (f32 base, f32 exponent) noexcept;

/**
 * 角度 (度) の正接を返す。
 *
 * @param degrees 角度 (度)。
 * @return 正接値。
 */
f32 Tan  (f32 degrees) noexcept;

/**
 * (x,y) 方向の角度 (度) を返す。
 *
 * @details AngleTo と同じ向き基準 (右が 0°、時計回りが正)。
 * @param y 方向ベクトルの Y 成分。
 * @param x 方向ベクトルの X 成分。
 * @return 角度 (度)。
 */
f32 Atan2(f32 y, f32 x) noexcept;

/**
 * current から target へ max_delta だけ近づけた値を返す (追従移動に使う)。
 *
 * @details target を行き過ぎることはない。
 * @param current 現在値。
 * @param target 目標値。
 * @param max_delta 1 回で進める最大量。
 * @return 近づけた後の値。
 */
f32 MoveTowards(f32 current, f32 target, f32 max_delta) noexcept;

/**
 * 値を [min, max) の範囲に巻き込む (はみ出したら反対側へ)。
 *
 * @param value 対象の値。
 * @param min 範囲の下限 (含む)。
 * @param max 範囲の上限 (含まない)。
 * @return 範囲内に巻き込んだ値。
 */
f32 Wrap   (f32 value, f32 min, f32 max) noexcept;

/**
 * 整数値を [min, max) の範囲に巻き込む。
 *
 * @param value 対象の値。
 * @param min 範囲の下限 (含む)。
 * @param max 範囲の上限 (含まない)。
 * @return 範囲内に巻き込んだ値。
 */
i32 WrapInt(i32 value, i32 min, i32 max) noexcept;

/**
 * [0, length) の範囲を繰り返す値を返す。
 *
 * @param t 入力値。
 * @param length 周期の長さ。
 * @return [0, length) に折り返した値。
 */
f32 Repeat (f32 t, f32 length) noexcept;

/**
 * 0 と length の間を往復する値を返す。
 *
 * @param t 入力値。
 * @param length 往復幅。
 * @return 0〜length を往復した値。
 */
f32 PingPong(f32 t, f32 length) noexcept;

/**
 * a→b をなめらかに補間する (両端で速度 0)。
 *
 * @param a 開始値。
 * @param b 終了値。
 * @param t 補間係数 (0〜1 にクランプされる)。
 * @return スムーズに補間した値。
 */
f32 SmoothStep(f32 a, f32 b, f32 t) noexcept;

/**
 * 角度 (度) を最短回りで補間する (回転の追従に使う)。
 *
 * @param a_deg 開始角度 (度)。
 * @param b_deg 終了角度 (度)。
 * @param t 補間係数 (0 で a_deg、1 で b_deg)。
 * @return 補間した角度 (度)。
 */
f32 LerpAngle(f32 a_deg, f32 b_deg, f32 t) noexcept;

/**
 * 2 点間の距離の 2 乗を返す (比較用で平方根を取らない分速い)。
 *
 * @param x1 点 1 の X 座標。
 * @param y1 点 1 の Y 座標。
 * @param x2 点 2 の X 座標。
 * @param y2 点 2 の Y 座標。
 * @return 距離の 2 乗。
 */
f32 DistanceSquared(f32 x1, f32 y1, f32 x2, f32 y2) noexcept;

/**
 * 原点 (0,0) から (x,y) までの距離を返す。
 *
 * @param x ベクトルの X 成分。
 * @param y ベクトルの Y 成分。
 * @return ベクトルの長さ。
 */
f32 Length(f32 x, f32 y) noexcept;

/**
 * 2 色を t で線形補間する。
 *
 * @param a 開始色。
 * @param b 終了色。
 * @param t 補間係数 (0 で a、1 で b)。
 * @return 補間した色。
 */
FColor LerpColor(FColor a, FColor b, f32 t) noexcept;

/**
 * HSV から RGB 色を作る。
 *
 * @param hue_degrees 色相 (0〜360 度)。
 * @param saturation 彩度 (0〜1)。
 * @param value 明度 (0〜1)。
 * @return 対応する不透明な FColor。
 */
FColor Hsv(f32 hue_degrees, f32 saturation, f32 value) noexcept;

/**
 * 色を白へ近づけて明るくする。
 *
 * @param c 元の色。
 * @param amount 明るくする量 (0〜1)。
 * @return 明るくした色 (不透明度は維持)。
 */
FColor Brighten(FColor c, f32 amount) noexcept;

/**
 * 色を黒へ近づけて暗くする。
 *
 * @param c 元の色。
 * @param amount 暗くする量 (0〜1)。
 * @return 暗くした色 (不透明度は維持)。
 */
FColor Darken  (FColor c, f32 amount) noexcept;

/**
 * 鮮やかな色をランダムに返す。
 *
 * @return ランダムな彩度・明度の高い色。
 */
FColor RandomColor() noexcept;

/**
 * 左右の移動入力を合成して返す。
 *
 * @details 左 ←/A・右 →/D・左スティック X を合成する。x += MoveX()*speed*DeltaTime() のように使う。
 * @return 横方向の入力 (-1.0〜+1.0)。
 */
f32  MoveX() noexcept;

/**
 * 上下の移動入力を合成して返す。
 *
 * @details 上 ↑/W・下 ↓/S・左スティック Y を合成する (Y-down なので下が +)。
 * @return 縦方向の入力 (-1.0〜+1.0)。
 */
f32  MoveY() noexcept;

/**
 * このフレームに何かキーが押されたかを返す (「押して開始」などに使う)。
 *
 * @return いずれかのキーが押された瞬間なら true。
 */
bool IsAnyKeyPressed() noexcept;

/**
 * 塗りつぶし長方形を中心基準で描く。
 *
 * @param cx 中心の X 座標。
 * @param cy 中心の Y 座標。
 * @param width 幅。
 * @param height 高さ。
 * @param color 塗り色。
 */
void DrawRectCentered(f32 cx, f32 cy, f32 width, f32 height, FColor color) noexcept;

/**
 * スプライトを中心基準で回転・拡縮・色掛けして描く。
 *
 * @param sprite 描画するスプライト。
 * @param cx 中心の X 座標。
 * @param cy 中心の Y 座標。
 * @param scale 拡大率 (1.0 で等倍)。
 * @param degrees 回転角 (度、時計回りが正)。
 * @param tint 乗算する色掛け (白で無加工)。
 */
void DrawSpriteCentered(Sprite sprite, f32 cx, f32 cy, f32 scale = 1.0f,
                        f32 degrees = 0.0f, FColor tint = FColor{1,1,1,1}) noexcept;

/**
 * 円と矩形が重なっているかを返す。
 *
 * @param cx 円の中心 X 座標。
 * @param cy 円の中心 Y 座標。
 * @param r 円の半径。
 * @param rx 矩形の左上 X 座標。
 * @param ry 矩形の左上 Y 座標。
 * @param rw 矩形の幅。
 * @param rh 矩形の高さ。
 * @return 重なっていれば true。
 */
bool CircleVsRect(f32 cx, f32 cy, f32 r,
                  f32 rx, f32 ry, f32 rw, f32 rh) noexcept;

/**
 * 2 つの線分が交差しているかを返す。
 *
 * @param ax 線分 1 の端点 a の X 座標。
 * @param ay 線分 1 の端点 a の Y 座標。
 * @param bx 線分 1 の端点 b の X 座標。
 * @param by 線分 1 の端点 b の Y 座標。
 * @param cx 線分 2 の端点 c の X 座標。
 * @param cy 線分 2 の端点 c の Y 座標。
 * @param dx 線分 2 の端点 d の X 座標。
 * @param dy 線分 2 の端点 d の Y 座標。
 * @return 交差していれば true。
 */
bool SegmentsIntersect(f32 ax, f32 ay, f32 bx, f32 by,
                       f32 cx, f32 cy, f32 dx, f32 dy) noexcept;

/**
 * 点が三角形の内側にあるかを返す。
 *
 * @param px 点の X 座標。
 * @param py 点の Y 座標。
 * @param x1 頂点 1 の X 座標。
 * @param y1 頂点 1 の Y 座標。
 * @param x2 頂点 2 の X 座標。
 * @param y2 頂点 2 の Y 座標。
 * @param x3 頂点 3 の X 座標。
 * @param y3 頂点 3 の Y 座標。
 * @return 内側 (辺上を含む) なら true。
 */
bool PointInTriangle(f32 px, f32 py, f32 x1, f32 y1,
                     f32 x2, f32 y2, f32 x3, f32 y3) noexcept;

/**
 * 矩形を、ふさがった矩形の外へ最小距離で押し戻す (壁めり込み防止に使う)。
 *
 * @details
 * 重なっていれば *x と *y を書き換えて true、重なっていなければ何もせず false を返す。
 * X 方向と Y 方向のうち食い込みの浅いほうへ押し出す。
 * @param x 動かす矩形の左上 X 座標 (重なり時に書き換わる)。
 * @param y 動かす矩形の左上 Y 座標 (重なり時に書き換わる)。
 * @param w 動かす矩形の幅。
 * @param h 動かす矩形の高さ。
 * @param sx ふさがった矩形の左上 X 座標。
 * @param sy ふさがった矩形の左上 Y 座標。
 * @param sw ふさがった矩形の幅。
 * @param sh ふさがった矩形の高さ。
 * @return 押し戻したら true、重なっていなければ false。
 */
bool ResolveRect(f32* x, f32* y, f32 w, f32 h,
                 f32 sx, f32 sy, f32 sw, f32 sh) noexcept;

/**
 * 時間を貯めて seconds ごとに 1 度だけ true を返す (周期実行)。
 *
 * @details accumulator には 0 初期化した変数を渡す。例: static f32 t=0; if (Every(2.0f,&t)) ...
 * @param seconds 発火する周期 (秒)。
 * @param accumulator 経過時間を貯める変数へのポインタ (呼び出し側が保持)。
 * @return この周期に達したフレームなら true。
 */
bool Every(f32 seconds, f32* accumulator) noexcept;

/**
 * クールダウンタイマーを処理し、再装填できたかを返す。
 *
 * @details timer が 0 以下なら true を返して timer=seconds に再装填、そうでなければ
 * DeltaTime ぶん減算して false を返す。連射制限などに使う。
 * @param timer 残り時間を保持する変数へのポインタ。
 * @param seconds 再装填する秒数。
 * @return 再装填した (= 行動できる) フレームなら true。
 */
bool Cooldown(f32* timer, f32 seconds) noexcept;

/**
 * 手動タイマーを DeltaTime ぶん 0 に向けて減らし、残り秒を返す。
 *
 * @param timer 残り時間を保持する変数へのポインタ。
 * @return 減算後の残り秒 (0 で終了)。
 */
f32  Countdown(f32* timer) noexcept;

/**
 * 画面を揺らす (画面シェイク)。
 *
 * @details 強さは累積し、毎フレーム減衰する。0.2〜1.0 が目安。
 * @param strength 加える揺れの強さ。
 */
void ScreenShake(f32 strength) noexcept;

/**
 * 全画面を一瞬光らせて減衰させる (フラッシュ)。
 *
 * @param color フラッシュの色。
 * @param seconds 減衰しきるまでの秒数。
 */
void ScreenFlash(FColor color, f32 seconds) noexcept;

/**
 * 加速するイージング (t は 0〜1)。
 *
 * @param t 進行度 (0〜1 にクランプされる)。
 * @return イージング後の値。
 */
f32 EaseIn   (f32 t) noexcept;

/**
 * 減速するイージング (t は 0〜1)。
 *
 * @param t 進行度 (0〜1 にクランプされる)。
 * @return イージング後の値。
 */
f32 EaseOut  (f32 t) noexcept;

/**
 * 加速→減速するイージング (t は 0〜1)。
 *
 * @param t 進行度 (0〜1 にクランプされる)。
 * @return イージング後の値。
 */
f32 EaseInOut(f32 t) noexcept;

/**
 * 行き過ぎて戻るイージング (t は 0〜1)。
 *
 * @param t 進行度。
 * @return イージング後の値 (途中で 1 を超える)。
 */
f32 EaseOutBack   (f32 t) noexcept;

/**
 * 跳ねるイージング (t は 0〜1)。
 *
 * @param t 進行度 (0〜1 にクランプされる)。
 * @return イージング後の値。
 */
f32 EaseOutBounce (f32 t) noexcept;

/**
 * ばねのように揺れて収束するイージング (t は 0〜1)。
 *
 * @param t 進行度。
 * @return イージング後の値。
 */
f32 EaseOutElastic(f32 t) noexcept;

// while(NextFrame()) の中で、HUD と同じ「カメラ無し（スクリーン座標）」で呼ぶこと。
// 状態は呼び出し側が持つ（Checkbox/Slider は値のポインタを渡す）。

/**
 * ボタンを描き、クリックされたかを返す。
 *
 * @details クリック (押して離す) された瞬間のフレームだけ true を返す。
 * @param x 左上の X 座標 (スクリーン座標)。
 * @param y 左上の Y 座標。
 * @param width ボタンの幅。
 * @param height ボタンの高さ。
 * @param label ボタンに表示するラベル。
 * @return クリックされたフレームなら true。
 */
bool Button(f32 x, f32 y, f32 width, f32 height, const char* label) noexcept;

/**
 * チェックボックスを描き、*value をトグルする。
 *
 * @details クリックで *value を反転し、変化したフレームだけ true を返す。
 * @param x 左上の X 座標 (スクリーン座標)。
 * @param y 左上の Y 座標。
 * @param size 箱の一辺の長さ。
 * @param label 横に表示するラベル。
 * @param value トグル対象の真偽値へのポインタ (呼び出し側が保持)。
 * @return 値が変化したフレームなら true。
 */
bool Checkbox(f32 x, f32 y, f32 size, const char* label, bool* value) noexcept;

/**
 * スライダーを描き、ドラッグで *value を変える。
 *
 * @details *value を [min, max] の範囲で動かし、値が変わった間 true を返す。
 * @param x 左上の X 座標 (スクリーン座標)。
 * @param y 左上の Y 座標。
 * @param width スライダーの幅。
 * @param value 操作対象の値へのポインタ (呼び出し側が保持)。
 * @param min 値の下限。
 * @param max 値の上限。
 * @return 値が動いたフレームなら true。
 */
bool Slider(f32 x, f32 y, f32 width, f32* value, f32 min, f32 max) noexcept;

/**
 * 即席 UI の配色を変える (既定は落ち着いた青系)。
 *
 * @param base 通常時の背景色。
 * @param hover ホバー時の背景色。
 * @param active 押下/ドラッグ中の背景色。
 * @param text 文字・チェック・ノブの色。
 */
void SetUiColors(FColor base, FColor hover, FColor active, FColor text) noexcept;

// 内部は本格的なワークスチール FThreadPool / 依存グラフ FJobGraph。
// ★重要: ジョブの中身(関数/ラムダ)は「別のスレッド」で走る。その中で描画系や他の easy 関数を
//   呼んではいけない(画面状態はメインスレッド専用)。中では計算だけして、結果は捕捉した変数に書く。
// ★初期化は不要: OpenWindow 済みならそのまま、未起動でも初回使用時に自動でワーカを起動する。
//   どうしても並列にできない環境(1コア等)では、自動的に「その場で順番に実行」へフォールバックする。

/**
 * 非同期ジョブ群 1 つを指すハンドル (コピー可、id==0 は無効)。
 */
struct JobBatch {
    /** バッチ識別子 (上位 16bit が世代、下位 16bit がスロット番号。0 は無効)。 */
    u32 id = 0;
};

/**
 * 依存グラフのノードを指すハンドル (コピー可、id==0 は無効)。
 */
struct JobNode  {
    /** ノード番号 (1 始まり、0 は無効)。 */
    u32 id = 0;
};

namespace jobdetail {
    /**
     * ラムダを「関数ポインタ + ヒープ上のコピー」に型消去したクロージャ。
     *
     * @details
     * この構造体の直後 (整列後) に Fn 本体を inline 格納し、1 回の確保で済ませる。
     * 実装詳細であり初学者が直接触る必要はない。
     */
    struct Closure {
        /** このクロージャを確保したアロケータ。 */
        FAllocator* allocation_allocator;

        /** allocation_allocator へ返す確保領域の先頭。 */
        void* allocation_base;

        /** 格納したラムダ本体を呼び出す関数ポインタ。 */
        void (*invoke)(Closure*);

        /** 格納したラムダを破棄する関数ポインタ。 */
        void (*destroy)(Closure*);
    };

    /**
     * Closure の直後に置く Fn 本体のバイトオフセットを返す。
     *
     * @details Fn と Closure の厳しいほうのアラインメントに合わせて切り上げる。
     * @tparam Fn 格納するラムダ/関数オブジェクト型。
     * @return Closure 先頭から Fn 本体までのオフセット (バイト)。
     */
    template<typename Fn> inline usize PayloadOffset() noexcept {
        const usize a = alignof(Fn) > alignof(Closure) ? alignof(Fn) : alignof(Closure);
        return (sizeof(Closure) + (a - 1)) & ~(a - 1);
    }

    /**
     * ラムダをヒープにコピーした Closure を確保して返す。
     *
     * @details
     * Closure + Fn 本体を 1 回の確保でまとめ、invoke/destroy に Fn を呼ぶ/破棄する
     * thunk を仕込む。確保失敗時は nullptr を返す。
     * @tparam Fn コピーするラムダ/関数オブジェクト型。
     * @param fn 型消去するラムダ (呼び出し側で生存している必要がある)。
     * @return 確保した Closure (失敗時は nullptr)。
     */
    template<typename Fn> inline Closure* MakeClosure(const Fn& fn) noexcept {
        const usize a   = alignof(Fn) > alignof(Closure) ? alignof(Fn) : alignof(Closure);
        const usize off = PayloadOffset<Fn>();
        if (off > (~usize(0)) - sizeof(Fn)) return nullptr;

        FAllocator& allocator = acs::DefaultAllocator();
        void* mem = allocator.Alloc(off + sizeof(Fn), a, acs::FSourceLoc::Current());
        if (!mem) return nullptr;
        Closure* c = static_cast<Closure*>(mem);
        ::new (static_cast<u8*>(mem) + off) Fn(fn);   // ラムダをコピー構築 (元 fn は呼び出し側で生存)
        c->allocation_allocator = &allocator;
        c->allocation_base = mem;
        c->invoke  = [](Closure* s) { (*reinterpret_cast<Fn*>(reinterpret_cast<u8*>(s) + PayloadOffset<Fn>()))(); };
        c->destroy = [](Closure* s) {  reinterpret_cast<Fn*>(reinterpret_cast<u8*>(s) + PayloadOffset<Fn>())->~Fn(); };
        return c;
    }

    /**
     * クロージャ本体を破棄し、生成時に記録した確保元へ領域を返す。
     *
     * @param closure 破棄するクロージャ。nullptr は no-op。
     */
    inline void DestroyClosure(Closure* closure) noexcept
    {
        if (closure == nullptr) return;
        FAllocator* const allocator = closure->allocation_allocator;
        void* const allocation_base = closure->allocation_base;
        closure->destroy(closure);
        if (allocator != nullptr && allocation_base != nullptr) {
            allocator->Free(allocation_base);
        }
    }

    /**
     * スレッドプールのワーカ起動を保証する。
     *
     * @details 未起動なら初回使用で FThreadPool を自動 Init する (Easy.cpp に実体)。
     * @return ワーカが 1 つ以上利用可能なら true。
     */
    bool     Ready() noexcept;

    /**
     * ParallelFor の自動チャンク幅 (grain) を求める。
     *
     * @param begin 範囲の開始インデックス。
     * @param end 範囲の終了インデックス (含まない)。
     * @return ワーカ当たり ~4 チャンクになるチャンク幅 (最低 1)。
     */
    i32      AutoGrain(i32 begin, i32 end) noexcept;

    /**
     * クロージャを非同期投入し、所属バッチのハンドルを返す。
     *
     * @details existing が有効ならそのバッチへ追加、無効なら新規スロットを確保する。
     * @param c 投入するクロージャ (バッチが所有権を持つ)。
     * @param existing 追加先の既存バッチ (無効なら新規作成)。
     * @return 投入先バッチのハンドル。
     */
    JobBatch SubmitAsync(Closure* c, JobBatch existing) noexcept;

    /**
     * 構築中の依存グラフにクロージャをノードとして追加する。
     *
     * @param c 追加するクロージャ (グラフが所有権を持つ)。
     * @return 追加したノードのハンドル。
     */
    JobNode  AddNode(Closure* c) noexcept;
}

/**
 * [begin,end) を全コアで分担して fn(i) を呼び、完了まで待つ (同期)。
 *
 * @details
 * fn は i ごとに独立した処理だけ行うこと (他要素や共有変数の競合に注意)。並列不可な
 * 環境では順次実行へフォールバックする。
 * @tparam Fn fn(i32) を呼べる関数オブジェクト型。
 * @param begin 範囲の開始インデックス。
 * @param end 範囲の終了インデックス (含まない)。
 * @param grain 1 チャンクの要素数 (0 以下なら AutoGrain で自動決定)。
 * @param fn 各インデックス i に対して呼ぶ処理。
 */
template<typename Fn>
inline void ParallelFor(i32 begin, i32 end, i32 grain, Fn fn) noexcept {
    if (end <= begin) return;
    if (!jobdetail::Ready()) { for (i32 i = begin; i < end; ++i) fn(i); return; }  // 並列不可 → 順次
    struct Ctx { Fn* fn; } ctx{ &fn };
    void (*thunk)(u32, u32, void*) =
        [](u32 i, u32, void* u) { (*static_cast<Ctx*>(u)->fn)(static_cast<i32>(i)); };
    const i32 g = grain > 0 ? grain : jobdetail::AutoGrain(begin, end);
    (void)acs::FThreadPool::ParallelFor(static_cast<u32>(begin), static_cast<u32>(end),
                                        static_cast<u32>(g), thunk, &ctx);
}

/**
 * [begin,end) を全コアで分担して fn(i) を呼び、完了まで待つ (チャンク幅は自動)。
 *
 * @tparam Fn fn(i32) を呼べる関数オブジェクト型。
 * @param begin 範囲の開始インデックス。
 * @param end 範囲の終了インデックス (含まない)。
 * @param fn 各インデックス i に対して呼ぶ処理。
 */
template<typename Fn>
inline void ParallelFor(i32 begin, i32 end, Fn fn) noexcept { ParallelFor(begin, end, 0, fn); }

/**
 * 処理を 1 つ非同期に走らせ、バッチハンドルを返す (すぐ戻る)。
 *
 * @details 並列不可・確保失敗時はその場で同期実行し、無効ハンドルを返す。完了は WaitJobs で待つ。
 * @tparam Fn 引数なしで呼べる関数オブジェクト型。
 * @param fn 別スレッドで走らせる処理。
 * @return 投入したバッチのハンドル (同期実行時は無効ハンドル)。
 */
template<typename Fn>
inline JobBatch RunAsync(Fn fn) noexcept {
    if (!jobdetail::Ready()) { fn(); return JobBatch{}; }       // 並列不可 → その場で実行
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    if (!c) { fn(); return JobBatch{}; }                        // 確保失敗 → その場で実行
    return jobdetail::SubmitAsync(c, JobBatch{});
}

/**
 * 既存バッチに処理を追加投入する (同じ WaitJobs でまとめて待てる)。
 *
 * @details 並列不可・確保失敗時はその場で同期実行する。
 * @tparam Fn 引数なしで呼べる関数オブジェクト型。
 * @param batch 追加先のバッチハンドル。
 * @param fn 別スレッドで走らせる処理。
 */
template<typename Fn>
inline void RunAsync(JobBatch batch, Fn fn) noexcept {
    if (!jobdetail::Ready()) { fn(); return; }
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    if (!c) { fn(); return; }
    (void)jobdetail::SubmitAsync(c, batch);
}

/**
 * バッチの全ジョブの完了を待つ (待ち終えたバッチは無効化される)。
 *
 * @param batch 待機するバッチハンドル。
 */
void WaitJobs(JobBatch batch) noexcept;

/**
 * バッチの全ジョブが完了したかを待たずに確認する。
 *
 * @param batch 確認するバッチハンドル。
 * @return 完了済み (または無効ハンドル) なら true。
 */
bool JobsDone(JobBatch batch) noexcept;

/**
 * 依存グラフのノードを 1 つ作る (Then で順序を張り RunJobs で実行する)。
 *
 * @tparam Fn 引数なしで呼べる関数オブジェクト型。
 * @param fn このノードで走らせる処理。
 * @return 作成したノードのハンドル (確保失敗時は無効ハンドル)。
 */
template<typename Fn>
inline JobNode Job(Fn fn) noexcept {
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    return c ? jobdetail::AddNode(c) : JobNode{};
}

/**
 * 2 ノード間に実行順序の依存を張る (before が終わってから after が走る)。
 *
 * @param before 先に走るノード。
 * @param after before の後に走るノード。
 */
void Then(JobNode before, JobNode after) noexcept;

/** 作った全ノードを依存順に実行し、全完了まで待つ (グラフは消費される)。 */
void RunJobs() noexcept;

/**
 * 並列ワーカ数を返す (未起動なら自動起動を試みる)。
 *
 * @return ワーカ数 (1 以上)。
 */
i32  WorkerCount() noexcept;

/**
 * 今このコードがワーカスレッド上で動いているかを返す。
 *
 * @return ワーカスレッド上なら true。
 */
bool IsWorker()    noexcept;

} // namespace acs::easy
