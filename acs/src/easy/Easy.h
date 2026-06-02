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

// acs 側の入力コード列挙をこの名前空間でも使えるよう再公開する。
using acs::EKey;
using acs::EMouseButton;
using acs::EGamepadButton;

// ---- 色 --------------------------------------------------------------------
// 各成分は 0.0〜1.0。FColor::Red などの定数を使うか、Rgb(255,0,0) で作る。
struct FColor {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;   // 不透明度（0=透明, 1=不透明）

    static const FColor Red, Green, Blue, Yellow, Cyan, Magenta;
    static const FColor White, Black, Gray, Orange, Sky, Clear;
};

// 0〜255 の整数で色を作る（Rgb(255,128,0) など）
FColor Rgb (u8 r, u8 g, u8 b) noexcept;
FColor Rgba(u8 r, u8 g, u8 b, u8 a) noexcept;
// 不透明度（0〜1）だけを変えた色を返す。半透明描画やフェードに使う。
FColor Fade(FColor color, f32 alpha) noexcept;

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
                const char* title = "ACS FGame") noexcept;

// 1 フレーム進める。while の条件に使う。直前のフレームを画面に出し、次の
// フレームの準備をして true を返す。閉じられたら（後始末をして）false。
bool NextFrame() noexcept;

// ゲームを終了する。次の NextFrame() が false を返すようになる。
void Quit() noexcept;

// ウィンドウのタイトル文字列を変える。
void SetWindowTitle(const char* title) noexcept;

// 背景色を設定する（次のフレームから反映）。既定は濃い紺色。
void SetBackground(FColor color) noexcept;

// 全画面表示の ON/OFF（ボーダーレス）。次のフレームから反映される。
void SetFullscreen(bool on) noexcept;
void ToggleFullscreen() noexcept;
bool IsFullscreen() noexcept;

// ===========================================================================
// 図形を描く（OpenWindow の後、while(NextFrame()) ループの中で呼ぶ）
// ===========================================================================

void DrawRect(f32 x, f32 y, f32 width, f32 height, FColor color) noexcept;
void DrawRectOutline(f32 x, f32 y, f32 width, f32 height,
                     FColor color, f32 thickness = 2.0f) noexcept;
// 回転した塗りつぶし長方形。(x,y) は回転前の左上、回転は中心まわり。
void DrawRectRotated(f32 x, f32 y, f32 width, f32 height,
                     f32 degrees, FColor color) noexcept;
void DrawCircle(f32 x, f32 y, f32 radius, FColor color) noexcept;        // (x,y)=中心
void DrawCircleOutline(f32 x, f32 y, f32 radius,
                       FColor color, f32 thickness = 2.0f) noexcept;
void DrawLine(f32 x1, f32 y1, f32 x2, f32 y2,
              FColor color, f32 thickness = 2.0f) noexcept;
void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                  FColor color) noexcept;
void DrawTriangleOutline(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                         FColor color, f32 thickness = 2.0f) noexcept;
void DrawPixel(f32 x, f32 y, FColor color) noexcept;

// ===========================================================================
// 画像（スプライト）を描く
// ===========================================================================

void DrawSprite(Sprite sprite, f32 x, f32 y) noexcept;                       // 元サイズ
void DrawSprite(Sprite sprite, f32 x, f32 y, f32 width, f32 height) noexcept; // 伸縮
// 回転（+拡縮）。(x,y) は回転前の左上、回転は画像の中心まわり。tint で色掛け。
void DrawSpriteRotated(Sprite sprite, f32 x, f32 y, f32 degrees,
                       f32 scale = 1.0f, FColor tint = FColor{1,1,1,1}) noexcept;
void DrawSpriteTinted(Sprite sprite, f32 x, f32 y, FColor tint) noexcept;
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

void DrawString(f32 x, f32 y, const char* text, FColor color) noexcept;
void DrawString(f32 x, f32 y, const char* text, FColor color, f32 size) noexcept;
// center_x で中央そろえして描く。
void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color) noexcept;
void DrawStringCentered(f32 center_x, f32 y, const char* text,
                        FColor color, f32 size) noexcept;
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
bool IsKeyDown    (EKey key) noexcept;   // 押されている間ずっと true
bool IsKeyPressed (EKey key) noexcept;   // 押した瞬間のフレームだけ true
bool IsKeyReleased(EKey key) noexcept;   // 離した瞬間のフレームだけ true
// このフレームに入力された文字（UTF-8、IME 確定後。無ければ ""）。名前入力に。
// バックスペースや Enter は IsKeyPressed(EKey::Backspace / EKey::Enter) で取る。
const char* TextInput() noexcept;

// ===========================================================================
// 入力 — マウス
// ===========================================================================
f32  MouseX() noexcept;                 // マウス X（ウィンドウ内ピクセル＝画面座標）
f32  MouseY() noexcept;
bool IsMouseDown    (EMouseButton button = EMouseButton::Left) noexcept;
bool IsMousePressed (EMouseButton button = EMouseButton::Left) noexcept;
bool IsMouseReleased(EMouseButton button = EMouseButton::Left) noexcept;
f32  MouseWheel() noexcept;              // ホイール回転（奥+ / 手前-）

// ===========================================================================
// 入力 — ゲームパッド（player は 0〜3、省略時は 0）
// ===========================================================================
bool IsGamepadConnected(i32 player = 0) noexcept;
bool IsGamepadDown   (EGamepadButton button, i32 player = 0) noexcept;
bool IsGamepadPressed(EGamepadButton button, i32 player = 0) noexcept;
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

// ===========================================================================
// ジョブ / 並列  (初学者向け — 内部は本格的なワークスチール FThreadPool / 依存グラフ FJobGraph)
// ---------------------------------------------------------------------------
// ★重要: ジョブの中身(関数/ラムダ)は「別のスレッド」で走る。その中で描画系や他の easy 関数を
//   呼んではいけない(画面状態はメインスレッド専用)。中では計算だけして、結果は捕捉した変数に書く。
// ★初期化は不要: OpenWindow 済みならそのまま、未起動でも初回使用時に自動でワーカを起動する。
//   どうしても並列にできない環境(1コア等)では、自動的に「その場で順番に実行」へフォールバックする。
// ===========================================================================

// 非同期ジョブ群 1 つを指すハンドル(コピー可、id==0 は無効)。Sprite/Sound と同じ軽量値型。
struct JobBatch { u32 id = 0; };
// 依存グラフのノードを指すハンドル(コピー可、id==0 は無効)。
struct JobNode  { u32 id = 0; };

// 実装詳細(初学者は読まなくてよい)。ラムダを「関数ポインタ + ヒープ上のコピー」に型消去する。
namespace jobdetail {
    struct Closure {
        void (*invoke)(Closure*);   // ラムダ本体を呼ぶ
        void (*destroy)(Closure*);  // ラムダを破棄する
        // この構造体の直後(整列後)に Fn 本体を inline 格納する(1 回の確保で済ませる)。
    };
    template<typename Fn> inline usize PayloadOffset() noexcept {
        const usize a = alignof(Fn) > alignof(Closure) ? alignof(Fn) : alignof(Closure);
        return (sizeof(Closure) + (a - 1)) & ~(a - 1);
    }
    template<typename Fn> inline Closure* MakeClosure(const Fn& fn) noexcept {
        const usize a   = alignof(Fn) > alignof(Closure) ? alignof(Fn) : alignof(Closure);
        const usize off = PayloadOffset<Fn>();
        void* mem = acs::DefaultAllocator().Alloc(off + sizeof(Fn), a, acs::FSourceLoc::Current());
        if (!mem) return nullptr;
        Closure* c = static_cast<Closure*>(mem);
        ::new (static_cast<u8*>(mem) + off) Fn(fn);   // ラムダをコピー構築 (元 fn は呼び出し側で生存)
        c->invoke  = [](Closure* s) { (*reinterpret_cast<Fn*>(reinterpret_cast<u8*>(s) + PayloadOffset<Fn>()))(); };
        c->destroy = [](Closure* s) {  reinterpret_cast<Fn*>(reinterpret_cast<u8*>(s) + PayloadOffset<Fn>())->~Fn(); };
        return c;
    }
    // 非テンプレ実体 (Easy.cpp)
    bool     Ready() noexcept;                                   // ワーカ起動を保証 (成功で true)
    i32      AutoGrain(i32 begin, i32 end) noexcept;             // ParallelFor の自動チャンク幅
    JobBatch SubmitAsync(Closure* c, JobBatch existing) noexcept;
    JobNode  AddNode(Closure* c) noexcept;
}

// (1) 並列 for: [begin,end) を全コアで分担して fn(i) を呼び、完了まで待つ(同期)。
//     fn は i ごとに独立した処理だけ行うこと(他要素や共有変数の競合に注意)。grain 省略で自動。
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
template<typename Fn>
inline void ParallelFor(i32 begin, i32 end, Fn fn) noexcept { ParallelFor(begin, end, 0, fn); }

// (2) 非同期に 1 つ走らせてハンドルを返す(すぐ戻る)。WaitJobs で完了を待つ。
template<typename Fn>
inline JobBatch RunAsync(Fn fn) noexcept {
    if (!jobdetail::Ready()) { fn(); return JobBatch{}; }       // 並列不可 → その場で実行
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    if (!c) { fn(); return JobBatch{}; }                        // 確保失敗 → その場で実行
    return jobdetail::SubmitAsync(c, JobBatch{});
}
// 既存 batch に追加投入(同じ WaitJobs でまとめて待てる)。
template<typename Fn>
inline void RunAsync(JobBatch batch, Fn fn) noexcept {
    if (!jobdetail::Ready()) { fn(); return; }
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    if (!c) { fn(); return; }
    (void)jobdetail::SubmitAsync(c, batch);
}
void WaitJobs(JobBatch batch) noexcept;   // batch の全完了まで待つ(待ち終えた batch は無効化)
bool JobsDone(JobBatch batch) noexcept;   // 完了したか(待たずに確認)

// (3) 依存つき: Job でノードを作り、Then で順序を張り、RunJobs でまとめて実行+待機。
template<typename Fn>
inline JobNode Job(Fn fn) noexcept {
    jobdetail::Closure* c = jobdetail::MakeClosure(fn);
    return c ? jobdetail::AddNode(c) : JobNode{};
}
void Then(JobNode before, JobNode after) noexcept;   // before が終わってから after が走る
void RunJobs() noexcept;                              // 作った全ノードを依存順に実行し、全完了まで待つ

// (4) 情報
i32  WorkerCount() noexcept;   // 並列ワーカ数(1 以上)
bool IsWorker()    noexcept;   // 今このコードがワーカスレッド上で動いているか

} // namespace acs::easy
