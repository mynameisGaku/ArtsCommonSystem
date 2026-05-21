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
//           if (IsKeyDown(Key::Right)) x += 5;
//           if (IsKeyDown(Key::Left))  x -= 5;
//           DrawRect(x, 320, 80, 80, Color::Sky);
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
// もっと本格的に作りたくなったら、acs::Application を直接使う方法へ進める
// （docs/QUICKSTART.md 参照）。easy はその入口に過ぎない。
// ============================================================================
#pragma once

#include "foundation/Types.h"
#include "platform/InputCodes.h"

namespace acs::easy {

// acs 側の入力コード列挙をこの名前空間でも使えるよう再公開する。
using acs::Key;
using acs::MouseButton;
using acs::GamepadButton;

// ---- 色 --------------------------------------------------------------------
// 各成分は 0.0〜1.0。Color::Red などの定数を使うか、Rgb(255,0,0) で作る。
struct Color {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;   // 不透明度（0=透明, 1=不透明）

    static const Color Red, Green, Blue, Yellow, Cyan, Magenta;
    static const Color White, Black, Gray, Orange, Sky, Clear;
};

// 0〜255 の整数で色を作る（Rgb(255,128,0) など）
Color Rgb (u8 r, u8 g, u8 b) noexcept;
Color Rgba(u8 r, u8 g, u8 b, u8 a) noexcept;
// 不透明度（0〜1）だけを変えた色を返す。半透明描画やフェードに使う。
Color Fade(Color color, f32 alpha) noexcept;

// ---- 素材ハンドル ----------------------------------------------------------
// LoadSprite / LoadSound が返す、コピー可能な軽い値型（id==0 は無効）。
struct Sprite { u32 id = 0; };
struct Sound  { u32 id = 0; };

// ===========================================================================
// ゲームループ・ウィンドウ
// ===========================================================================

// ウィンドウを開いてゲームを初期化する。プログラムの最初に 1 回だけ呼ぶ。
// 初期化に失敗した場合はコンソールにエラーを出し、最初の NextFrame() が
// false を返す（ゲームは画面を出さずに静かに終了する）。
void OpenWindow(i32 width = 1280, i32 height = 720,
                const char* title = "ACS Game") noexcept;

// 1 フレーム進める。while の条件に使う。直前のフレームを画面に出し、次の
// フレームの準備をして true を返す。閉じられたら（後始末をして）false。
bool NextFrame() noexcept;

// ゲームを終了する。次の NextFrame() が false を返すようになる。
void Quit() noexcept;

// ウィンドウのタイトル文字列を変える。
void SetWindowTitle(const char* title) noexcept;

// 背景色を設定する（次のフレームから反映）。既定は濃い紺色。
void SetBackground(Color color) noexcept;

// 全画面表示の ON/OFF（ボーダーレス）。次のフレームから反映される。
void SetFullscreen(bool on) noexcept;
void ToggleFullscreen() noexcept;
bool IsFullscreen() noexcept;

// ===========================================================================
// 図形を描く（OpenWindow の後、while(NextFrame()) ループの中で呼ぶ）
// ===========================================================================

void DrawRect(f32 x, f32 y, f32 width, f32 height, Color color) noexcept;
void DrawRectOutline(f32 x, f32 y, f32 width, f32 height,
                     Color color, f32 thickness = 2.0f) noexcept;
// 回転した塗りつぶし長方形。(x,y) は回転前の左上、回転は中心まわり。
void DrawRectRotated(f32 x, f32 y, f32 width, f32 height,
                     f32 degrees, Color color) noexcept;
void DrawCircle(f32 x, f32 y, f32 radius, Color color) noexcept;        // (x,y)=中心
void DrawCircleOutline(f32 x, f32 y, f32 radius,
                       Color color, f32 thickness = 2.0f) noexcept;
void DrawLine(f32 x1, f32 y1, f32 x2, f32 y2,
              Color color, f32 thickness = 2.0f) noexcept;
void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                  Color color) noexcept;
void DrawTriangleOutline(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                         Color color, f32 thickness = 2.0f) noexcept;
void DrawPixel(f32 x, f32 y, Color color) noexcept;

// ===========================================================================
// 画像（スプライト）を描く
// ===========================================================================

void DrawSprite(Sprite sprite, f32 x, f32 y) noexcept;                       // 元サイズ
void DrawSprite(Sprite sprite, f32 x, f32 y, f32 width, f32 height) noexcept; // 伸縮
// 回転（+拡縮）。(x,y) は回転前の左上、回転は画像の中心まわり。tint で色掛け。
void DrawSpriteRotated(Sprite sprite, f32 x, f32 y, f32 degrees,
                       f32 scale = 1.0f, Color tint = Color{1,1,1,1}) noexcept;
void DrawSpriteTinted(Sprite sprite, f32 x, f32 y, Color tint) noexcept;
void DrawSpriteFlipped(Sprite sprite, f32 x, f32 y,
                       bool flip_x, bool flip_y) noexcept;
// 画像の一部分を切り出して描く（スプライトシート用）。
void DrawSpritePart(Sprite sprite, f32 x, f32 y, f32 width, f32 height,
                    f32 src_x, f32 src_y, f32 src_width, f32 src_height) noexcept;
f32 SpriteWidth (Sprite sprite) noexcept;
f32 SpriteHeight(Sprite sprite) noexcept;

// ===========================================================================
// 文字を描く（UTF-8、日本語可。\n で改行）
// ===========================================================================

void DrawString(f32 x, f32 y, const char* text, Color color) noexcept;
void DrawString(f32 x, f32 y, const char* text, Color color, f32 size) noexcept;
// center_x で中央そろえして描く。
void DrawStringCentered(f32 center_x, f32 y, const char* text, Color color) noexcept;
void DrawStringCentered(f32 center_x, f32 y, const char* text,
                        Color color, f32 size) noexcept;
f32 TextWidth(const char* text) noexcept;
f32 TextWidth(const char* text, f32 size) noexcept;
f32 TextHeight() noexcept;
f32 TextHeight(f32 size) noexcept;

// ===========================================================================
// カメラ（スクロール）とクリップ
// ===========================================================================
// カメラは「ワールド」と「画面」を分ける。SetCamera 後に描く図形はワールド
// 座標として扱われ、カメラの位置・ズームに応じて画面に映る。カメラ設定は
// **毎フレーム呼ぶ**こと（描画関数と同じ。呼ばなければカメラ無し）。

// (x,y) のワールド座標が画面中央に映るようカメラを置く。
void SetCamera(f32 x, f32 y) noexcept;
// ズーム倍率（1.0 で等倍、2.0 で 2 倍に拡大）。
void SetCameraZoom(f32 zoom) noexcept;
// カメラを初期状態（スクロール無し・ズーム 1.0）に戻す。HUD を描く前などに。
void ResetCamera() noexcept;
f32  CameraX() noexcept;          // 現在のカメラ中心 X（ワールド座標）
f32  CameraY() noexcept;          // 現在のカメラ中心 Y
// マウス位置をワールド座標で得る（カメラのスクロール・ズームを反映）。
f32  MouseWorldX() noexcept;
f32  MouseWorldY() noexcept;

// 描画をこの矩形の中だけに制限する（画面座標、カメラの影響を受けない）。
void SetClipRect(f32 x, f32 y, f32 width, f32 height) noexcept;
// クリップを解除して画面全体に描けるようにする。
void ClearClipRect() noexcept;

// ===========================================================================
// ポストプロセス（画面全体の効果）
// ===========================================================================
// Bloom（発光）・ビネット・カラーグレーディング等、画面全体にかかる効果。
// **Diligent バックエンドでビルドした場合のみ有効**（DX12 raw ビルドでは
// 各関数は無視される）。IsPostProcessAvailable() で利用可否を確認できる。
// 効果は既定ですべて無効。下の Set 関数で必要なものだけ有効化する。
// 注: 有効時は文字も含めて画面全体に効果がかかる。
bool IsPostProcessAvailable() noexcept;
void SetBloom(f32 intensity) noexcept;            // 発光の強さ（0=無効、0.6 が目安）
void SetBloomThreshold(f32 threshold) noexcept;   // 発光させる明るさの閾値
void SetExposure(f32 exposure) noexcept;          // 露出（明るさ。1.0=標準）
void SetVignette(f32 intensity) noexcept;         // 画面端の暗化（0〜1、0=無効）
void SetChromaticAberration(f32 amount) noexcept; // 色収差（0=無効、0.002 が目安）
void SetFilmGrain(f32 intensity) noexcept;        // フィルムノイズ（0=無効）
// 彩度（1=標準）・コントラスト（1=標準）・色温度（-1=青寄り〜+1=橙寄り）
void SetColorGrading(f32 saturation, f32 contrast, f32 temperature) noexcept;
void SetSharpness(f32 strength) noexcept;         // 輪郭強調（0〜1、0=無効）
void SetTonemap(i32 mode) noexcept;               // 0=ACES、1=AgX、2=Reinhard
void SetAutoExposure(bool enabled) noexcept;      // 自動露出（明るさ自動調整）

// ===========================================================================
// 素材の読み込み（OpenWindow の後で呼ぶ）
// ===========================================================================
// 同じパスを再度渡しても読み直さず同じハンドルを返す。失敗してもプログラムは
// 落ちない（無効ハンドルを返し、描画/再生は無視）。パスは実行時のカレント
// ディレクトリからの相対。見つからないときは絶対パスを使うか実行ファイルの
// 隣に素材を置くこと。
Sprite LoadSprite(const char* path) noexcept;
Sound  LoadSound (const char* path) noexcept;

// ===========================================================================
// 音
// ===========================================================================
void Play(Sound sound) noexcept;                      // 効果音を 1 回
void Play(Sound sound, f32 volume) noexcept;          // 音量 0.0〜1.0
void PlayLoop(Sound sound) noexcept;                  // ループ再生（BGM）
void PlayLoop(Sound sound, f32 volume) noexcept;
void StopSound(Sound sound) noexcept;                 // その音のループを止める
void StopAllSounds() noexcept;                        // 鳴っている音を全部止める
void SetMasterVolume(f32 volume) noexcept;            // 全体音量（音量スライダー用）
void PauseAllSounds() noexcept;                       // 一時停止（位置は保たれる）
void ResumeAllSounds() noexcept;

// ===========================================================================
// 入力 — キーボード
// ===========================================================================
bool IsKeyDown    (Key key) noexcept;   // 押されている間ずっと true
bool IsKeyPressed (Key key) noexcept;   // 押した瞬間のフレームだけ true
bool IsKeyReleased(Key key) noexcept;   // 離した瞬間のフレームだけ true
// このフレームに入力された文字（UTF-8、IME 確定後。無ければ ""）。名前入力に。
// バックスペースや Enter は IsKeyPressed(Key::Backspace / Key::Enter) で取る。
const char* TextInput() noexcept;

// ===========================================================================
// 入力 — マウス
// ===========================================================================
f32  MouseX() noexcept;                 // マウス X（ウィンドウ内ピクセル＝画面座標）
f32  MouseY() noexcept;
bool IsMouseDown    (MouseButton button = MouseButton::Left) noexcept;
bool IsMousePressed (MouseButton button = MouseButton::Left) noexcept;
bool IsMouseReleased(MouseButton button = MouseButton::Left) noexcept;
f32  MouseWheel() noexcept;              // ホイール回転（奥+ / 手前-）

// ===========================================================================
// 入力 — ゲームパッド（player は 0〜3、省略時は 0）
// ===========================================================================
bool IsGamepadConnected(i32 player = 0) noexcept;
bool IsGamepadDown   (GamepadButton button, i32 player = 0) noexcept;
bool IsGamepadPressed(GamepadButton button, i32 player = 0) noexcept;
f32  GamepadLeftX (i32 player = 0) noexcept;   // スティック -1.0〜+1.0
f32  GamepadLeftY (i32 player = 0) noexcept;
f32  GamepadRightX(i32 player = 0) noexcept;
f32  GamepadRightY(i32 player = 0) noexcept;
f32  GamepadLeftTrigger (i32 player = 0) noexcept;   // トリガー 0.0〜1.0
f32  GamepadRightTrigger(i32 player = 0) noexcept;

// ===========================================================================
// 乱数
// ===========================================================================
i32  RandomInt(i32 min, i32 max) noexcept;     // min 以上 max 以下の整数
f32  RandomFloat(f32 min, f32 max) noexcept;   // min 以上 max 未満の小数
bool RandomBool() noexcept;                     // true / false
void RandomSeed(u32 seed) noexcept;             // 種を固定（同じ種＝同じ並び）

// ===========================================================================
// 当たり判定（重なっていれば true）
// ===========================================================================
bool RectsOverlap(f32 x1, f32 y1, f32 w1, f32 h1,
                  f32 x2, f32 y2, f32 w2, f32 h2) noexcept;
bool CirclesOverlap(f32 x1, f32 y1, f32 r1,
                    f32 x2, f32 y2, f32 r2) noexcept;
bool PointInRect(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) noexcept;
bool PointInCircle(f32 px, f32 py, f32 cx, f32 cy, f32 r) noexcept;

// ===========================================================================
// 数学ヘルパ
// ===========================================================================
f32 Clamp(f32 value, f32 lo, f32 hi) noexcept;
f32 Lerp (f32 a, f32 b, f32 t) noexcept;
f32 Distance(f32 x1, f32 y1, f32 x2, f32 y2) noexcept;
f32 Min(f32 a, f32 b) noexcept;
f32 Max(f32 a, f32 b) noexcept;
f32 Abs(f32 value) noexcept;
// 三角関数。**角度は度（°）で渡す**（C++ 標準の sin/cos はラジアンなので注意）。
f32 Sin(f32 degrees) noexcept;
f32 Cos(f32 degrees) noexcept;
f32 Sqrt(f32 value) noexcept;
// (x1,y1) から (x2,y2) へ向かう向きの角度（度）。DrawSpriteRotated に渡せる。
f32 AngleTo(f32 x1, f32 y1, f32 x2, f32 y2) noexcept;

// ===========================================================================
// セーブ／ロード（実行ファイルの場所の save.dat に key-value で保存）
// ===========================================================================
// ハイスコアや設定の永続化に使う。OpenWindow の前でも後でも呼べる。
void SaveInt   (const char* key, i32 value) noexcept;
i32  LoadInt   (const char* key, i32 default_value) noexcept;
void SaveFloat (const char* key, f32 value) noexcept;
f32  LoadFloat (const char* key, f32 default_value) noexcept;
// 値に改行（\n）は使えない。
void SaveString(const char* key, const char* value) noexcept;
// 戻り値は次に Save 系を呼ぶまで有効。
const char* LoadString(const char* key, const char* default_value) noexcept;
bool HasSaveKey(const char* key) noexcept;     // そのキーが保存済みか
void DeleteAllSaves() noexcept;                 // 保存内容を全消去

// ===========================================================================
// 情報
// ===========================================================================
f32  DeltaTime()   noexcept;   // 前フレームからの経過秒
f32  ElapsedTime() noexcept;   // OpenWindow からの累積秒
i32  Fps()         noexcept;   // おおよその 1 秒あたりフレーム数
f32  ScreenWidth()  noexcept;
f32  ScreenHeight() noexcept;

} // namespace acs::easy
