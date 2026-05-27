// SPDX-License-Identifier: Apache-2.0
// ACS Easy — 実装
//
// 設計:
//   ・acs::FApplication は使わず、独立した入口として自前でエンジンを起動する。
//   ・状態は単一のグローバル EasyState（このファイル内のみ）。単一スレッド前提。
//   ・OpenWindow で起動、NextFrame で 1 フレーム駆動（前フレーム提示 → 入力 →
//     クリア → 描画開始）、ウィンドウが閉じたら NextFrame 内で後始末する。
//   ・図形・スプライト・文字は FSpriteBatch に集約。回転は FSpriteBatch::DrawRotated、
//     カメラは FSpriteBatch::SetView（VS でワールド→スクリーン変換）。
//   ・公開 API の位置 (x,y) はすべて図形・画像の左上に統一（円のみ中心）。
#include "easy/Easy.h"

#include "foundation/Move.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "container/String.h"
#include "memory/MemorySystem.h"
#include "memory/UniquePtr.h"
#include "memory/Rc.h"
#include "threading/ThreadPool.h"
#include "math/Vec.h"
#include "math/Math.h"
#include "platform/Window.h"
#include "platform/Input.h"
#include "platform/Time.h"
#include "platform/Event.h"
#include "render/Renderer.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/RenderAssets.h"
#include "render/RhiTypes.h"
#include "render/IRhiSwapchain.h"
#include "render/PostProcess.h"
#include "asset/AssetRegistry.h"
#include "asset/Asset.h"
#include "asset/ImageAsset.h"
#include "asset/AudioAsset.h"
#include "audio/AudioEngine.h"

#include <windows.h>   // MultiByteToWideChar
#include <cstring>     // strcmp, strlen
#include <cstdlib>     // atexit, strtol, strtod
#include <cstdio>      // セーブファイル I/O (fopen_s 等)

namespace acs::easy {

// ============================================================================
// 色定数
// ============================================================================
const FColor FColor::Red    { 0.92f, 0.26f, 0.26f, 1.0f };
const FColor FColor::Green  { 0.30f, 0.78f, 0.36f, 1.0f };
const FColor FColor::Blue   { 0.26f, 0.49f, 0.96f, 1.0f };
const FColor FColor::Yellow { 0.98f, 0.83f, 0.25f, 1.0f };
const FColor FColor::Cyan   { 0.25f, 0.83f, 0.93f, 1.0f };
const FColor FColor::Magenta{ 0.93f, 0.33f, 0.78f, 1.0f };
const FColor FColor::White  { 1.0f,  1.0f,  1.0f,  1.0f };
const FColor FColor::Black  { 0.0f,  0.0f,  0.0f,  1.0f };
const FColor FColor::Gray   { 0.52f, 0.54f, 0.60f, 1.0f };
const FColor FColor::Orange { 1.0f,  0.55f, 0.15f, 1.0f };
const FColor FColor::Sky    { 0.45f, 0.70f, 0.95f, 1.0f };
const FColor FColor::Clear  { 0.0f,  0.0f,  0.0f,  0.0f };

// ============================================================================
// 内部状態（このファイル内のみ）
// ============================================================================
namespace {

struct SpriteSlot {
    TUniquePtr<IRhiTexture> tex;
    FString                 path;
    f32                    w = 0.0f;
    f32                    h = 0.0f;
};

struct SoundSlot {
    TRc<Asset>   asset;     // FAudioAsset を再生中ずっと生かしておくため保持
    FString      path;
    SoundHandle loop{};    // PlayLoop 中のハンドル（StopSound 用、無効値で初期化）
};

struct EasyState {
    // ライフサイクルのフラグ
    bool booted      = false;  // OpenWindow に成功した
    bool boot_failed = false;  // OpenWindow に失敗した
    bool finished    = false;  // NextFrame が false を返し、後始末も済んだ
    bool frame_open  = false;  // FSpriteBatch::Begin 済み（この間だけ描画可能）
    bool quit_req    = false;  // Quit() が呼ばれた
    bool warned_draw = false;  // 「枠の外で描画した」警告を 1 度だけ出す用
    i32  fs_request  = -1;     // 保留中の全画面切替（-1:無し 0:窓 1:全画面）

    // エンジンオブジェクト（宣言順 = 構築順。破棄は逆順）
    FWindow        window;
    FRenderer      renderer;
    FAssetRegistry assets;
    FAudioEngine   audio;
    bool          audio_ok = false;
    FSpriteBatch   batch;
    Font          font;
    bool          font_ok  = false;
    TUniquePtr<IRhiTexture> circle_tex;

    TArray<SpriteSlot> sprites;
    TArray<SoundSlot>  sounds;

    FrameTimer  timer;
    f32         dt    = 0.0f;
    ClearColor  clear { 0.10f, 0.12f, 0.16f, 1.0f };

    // カメラ（毎フレーム恒等にリセットされる）
    f32 cam_x = 0.0f, cam_y = 0.0f, cam_zoom = 1.0f;

    // ポストプロセス（HDR 経路。Diligent backend でのみ有効）
    FPostProcess       post;
    bool              post_available = false;
    u32               post_buf_idx   = 0;
    PostProcessParams pp_params;

    wchar_t title_buf[256] = L"ACS Easy";
};

EasyState g_state;

// ---- 文字列変換 (UTF-8 -> UTF-16) ------------------------------------------
void ToWide(const char* utf8, wchar_t* out, int out_len) noexcept {
    if (out_len <= 0) return;
    out[0] = 0;
    if (!utf8) return;
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_len);
    if (n <= 0) out[0] = 0;            // 変換失敗 -> 空文字列
    out[out_len - 1] = 0;              // 念のため終端を保証
}

inline FVec4 ToVec4(FColor c)   noexcept { return FVec4{ c.r, c.g, c.b, c.a }; }
inline f32  Clamp01(f32 v)    noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// player_index を 0..3 に収める
inline u32 GpIdx(i32 player) noexcept {
    if (player < 0) return 0;
    if (player > 3) return 3;
    return static_cast<u32>(player);
}

// ---- 乱数（xorshift32。軽量で、ゲーム用途には十分な品質）--------------------
u32 g_rng = 0x9E3779B9u;
u32 NextRng() noexcept {
    u32 x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x ? x : 0x9E3779B9u;   // 0 に落ち込まないよう保険
    return g_rng;
}

// ---- 円テクスチャ生成（DrawCircle / DrawCircleOutline が使う白い円）---------
TUniquePtr<IRhiTexture> MakeCircleTexture(IRhiDevice& device) noexcept {
    constexpr u32 N = 128;
    TArray<u8> px;
    px.Resize(static_cast<usize>(N) * N * 4);
    u8* p = px.Data();
    const f32 center = (N - 1) * 0.5f;
    const f32 radius = N * 0.5f - 1.0f;
    for (u32 y = 0; y < N; ++y) {
        for (u32 x = 0; x < N; ++x) {
            const f32 fx = static_cast<f32>(x) - center;
            const f32 fy = static_cast<f32>(y) - center;
            const f32 d  = Sqrt(fx * fx + fy * fy);
            const f32 a  = Clamp01(radius - d + 0.5f);   // 端 1px をなめらかに
            const usize i = (static_cast<usize>(y) * N + x) * 4;
            p[i + 0] = 255;
            p[i + 1] = 255;
            p[i + 2] = 255;
            p[i + 3] = static_cast<u8>(a * 255.0f);
        }
    }
    FTextureDesc desc{};
    desc.width             = N;
    desc.height            = N;
    desc.format            = EFormat::R8G8B8A8_UNorm;
    desc.initial_data      = p;
    desc.initial_data_size = px.Size();
    auto r = CreateRhiTexture(device, desc);
    if (r.IsErr()) {
        ACS_LOG_WARN("easy: 円テクスチャの作成に失敗（DrawCircle が無効化）");
        return TUniquePtr<IRhiTexture>{};
    }
    return Move(r.Value());
}

// ---- 既定フォントの読み込み（OS 標準フォント候補を順に試す）----------------
// アトラスは 4096×4096（GPU テクスチャ約 67MB）。CJK 統合漢字 約 2 万字を
// 全部収めるには 2048 では足りず、溢れたグリフが脱落するため 4096 まで広げる。
bool LoadDefaultFont(IRhiDevice& device) noexcept {
    const wchar_t* candidates[] = {
        L"C:\\Windows\\Fonts\\meiryo.ttc",
        L"C:\\Windows\\Fonts\\YuGothM.ttc",
        L"C:\\Windows\\Fonts\\YuGothR.ttc",
        L"C:\\Windows\\Fonts\\msgothic.ttc",
        L"C:\\Windows\\Fonts\\segoeui.ttf",
    };
    for (const wchar_t* path : candidates) {
        if (g_state.font.LoadFromFile(device, path, 22.0f, 4096, true).IsOk())
            return true;
    }
    ACS_LOG_WARN("easy: フォントが見つかりません（DrawString は何も描きません）");
    return false;
}

// ---- サイズ可変テキスト描画（Font のグリフを並べ、scale 倍で拡縮）----------
void DrawTextScaled(f32 x, f32 y, const char* text, FVec4 color, f32 scale) noexcept {
    IRhiTexture* atlas = g_state.font.AtlasTexture();
    if (!atlas || !text) return;
    f32 pen_x     = x;
    f32 baseline = y + g_state.font.Ascent() * scale;
    const char* p = text;
    while (true) {
        u32 cp = DecodeUtf8(&p);
        if (cp == 0) break;
        if (cp == '\n') {
            pen_x      = x;
            baseline += g_state.font.LineHeight() * scale;
            continue;
        }
        GlyphInfo g{};
        if (!g_state.font.GetGlyph(cp, g)) continue;
        const f32 qx = pen_x     + g.x_offset * scale;
        const f32 qy = baseline + g.y_offset * scale;
        g_state.batch.DrawSub(*atlas, qx, qy, g.width * scale, g.height * scale,
                             g.u0, g.v0, g.u1, g.v1, color);
        pen_x += g.x_advance * scale;
    }
}

// ---- ウィンドウイベントを Input / FRenderer へ橋渡し ------------------------
void EasyEventBridge(void* /*user*/, const Event& e) noexcept {
    Input::OnEvent(e);
    if (e.type == EventType::WindowResize) {
        g_state.renderer.OnResize(e.resize.width, e.resize.height);
        if (g_state.post_available)
            (void)g_state.post.Resize(e.resize.width, e.resize.height);
    }
}

// ---- 後始末（NextFrame がウィンドウ閉鎖を検知したとき 1 度だけ呼ぶ）-------
void ShutdownEasy() noexcept {
    // GPU の処理完了を待ってから GPU リソースを解放する（use-after-free 防止）
    if (g_state.renderer.Device()) g_state.renderer.Device()->WaitIdle();
    g_state.batch.Shutdown();
    g_state.font.Shutdown();
    g_state.circle_tex.Reset();
    g_state.sprites = TArray<SpriteSlot>{};   // 全スプライトのテクスチャを解放
    if (g_state.audio_ok) g_state.audio.Shutdown();
    g_state.sounds = TArray<SoundSlot>{};
    g_state.assets.Clear();
    if (g_state.post_available) g_state.post.Shutdown();
    g_state.renderer.Shutdown();             // ここで GPU デバイスを破棄する
    FThreadPool::Shutdown();
    FMemorySystem::Shutdown();
    ACS_LOG_INFO("easy: 終了しました");
    FLogger::Flush();
    FLogger::Shutdown();
}

void RunShutdownOnce() {
    if (g_state.booted && !g_state.finished) {
        ShutdownEasy();
        g_state.finished = true;
    }
}

void WarnDrawOutsideFrame() noexcept {
    if (g_state.warned_draw) return;
    g_state.warned_draw = true;
    ACS_LOG_WARN("easy: 描画関数は OpenWindow() の後、while(NextFrame()) の中で呼んでください");
}

// ---- セーブデータ（key=value 形式の save.dat。OpenWindow 不要で使える）-----
struct SaveEntry { FString key; FString value; };
TArray<SaveEntry> g_save;
bool             g_save_loaded = false;

void EnsureSaveLoaded() noexcept {
    if (g_save_loaded) return;
    g_save_loaded = true;
    FILE* f = nullptr;
    if (fopen_s(&f, "save.dat", "rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 4 * 1024 * 1024) {
        TArray<char> buf;
        buf.Resize(static_cast<usize>(sz) + 1);
        const usize rd = fread(buf.Data(), 1, static_cast<usize>(sz), f);
        buf[rd] = 0;
        char* p = buf.Data();
        while (*p) {
            char* line = p;
            while (*p && *p != '\n' && *p != '\r') ++p;
            char* line_end = p;
            while (*p == '\n' || *p == '\r') ++p;   // 改行をスキップして次行へ
            *line_end = 0;                           // 行を NUL 終端
            char* eq = line;
            while (*eq && *eq != '=') ++eq;
            if (*eq == '=') {
                *eq = 0;                             // key 部を NUL 終端
                SaveEntry e;
                e.key   = FString{ line };
                e.value = FString{ eq + 1 };
                g_save.PushBack(Move(e));
            }
        }
    }
    fclose(f);
}

SaveEntry* FindSave(const char* key) noexcept {
    for (usize i = 0; i < g_save.Size(); ++i) {
        const char* k = g_save[i].key.Data();
        if (k && strcmp(k, key) == 0) return &g_save[i];
    }
    return nullptr;
}

void WriteSaveFile() noexcept {
    FILE* f = nullptr;
    if (fopen_s(&f, "save.dat", "wb") != 0 || !f) {
        // FLogger 稼働中のみ警告（OpenWindow 前/終了後の Save でも安全に）
        if (g_state.booted && !g_state.finished)
            ACS_LOG_WARN("easy: セーブファイルに書き込めません");
        return;
    }
    for (usize i = 0; i < g_save.Size(); ++i) {
        const char* k = g_save[i].key.Data();
        const char* v = g_save[i].value.Data();
        if (k && *k) fwrite(k, 1, strlen(k), f);
        fputc('=', f);
        if (v && *v) fwrite(v, 1, strlen(v), f);
        fputc('\n', f);
    }
    fclose(f);
}

void SetSaveValue(const char* key, const char* value) noexcept {
    EnsureSaveLoaded();
    SaveEntry* e = FindSave(key);
    if (e) {
        e->value = FString{ value };
    } else {
        SaveEntry ne;
        ne.key   = FString{ key };
        ne.value = FString{ value };
        g_save.PushBack(Move(ne));
    }
    WriteSaveFile();
}

} // namespace

// ============================================================================
// 色ヘルパ
// ============================================================================
FColor Rgb(u8 r, u8 g, u8 b) noexcept {
    return FColor{ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
}
FColor Rgba(u8 r, u8 g, u8 b, u8 a) noexcept {
    return FColor{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
}
FColor Fade(FColor color, f32 alpha) noexcept {
    color.a = Clamp01(alpha);
    return color;
}

// ============================================================================
// ゲームループ・ウィンドウ
// ============================================================================
void OpenWindow(i32 width, i32 height, const char* title) noexcept {
    if (g_state.booted || g_state.boot_failed) {
        ACS_LOG_WARN("easy: OpenWindow() は既に呼ばれています");
        return;
    }

    // 1. ロガー
    FLogConfig lc{};
    FLogger::Init(lc);

    // 2. メモリシステム
    if (auto r = FMemorySystem::Init(FMemorySystem::DefaultConfig()); r.IsErr()) {
        ACS_LOG_ERROR("easy: メモリシステムの初期化に失敗: %s", r.Error().message);
        g_state.boot_failed = true;
        return;
    }
    // 3. スレッドプール
    if (auto r = FThreadPool::Init(); r.IsErr()) {
        ACS_LOG_ERROR("easy: スレッドプールの初期化に失敗: %s", r.Error().message);
        g_state.boot_failed = true;
        return;
    }
    // 4. ウィンドウ
    ToWide(title ? title : "ACS FGame", g_state.title_buf, 256);
    FWindowConfig wc{};
    wc.title  = g_state.title_buf;
    wc.width  = (width  > 0) ? static_cast<u32>(width)  : 1280;
    wc.height = (height > 0) ? static_cast<u32>(height) : 720;
    auto wr = FWindow::Create(wc);
    if (wr.IsErr()) {
        ACS_LOG_ERROR("easy: ウィンドウの作成に失敗: %s", wr.Error().message);
        g_state.boot_failed = true;
        return;
    }
    g_state.window = Move(wr.Value());
    g_state.window.SetEventCallback(&EasyEventBridge, nullptr);

    // 5. レンダラ（easy は 2D 専用なので深度バッファは不要）
    if (auto r = g_state.renderer.Init(g_state.window, false, /*enable_depth=*/false); r.IsErr()) {
        ACS_LOG_ERROR("easy: レンダラの初期化に失敗: %s", r.Error().message);
        g_state.boot_failed = true;
        return;
    }
    IRhiDevice* dev = g_state.renderer.Device();

    // 6. ポストプロセス（HDR 経路。Diligent backend のみ。DX12 raw は非対応）
    {
        const char* bn = dev->BackendName();
        if (!(bn && strcmp(bn, "DX12") == 0)) {
            if (auto r = g_state.post.Init(*dev, g_state.window.Width(),
                                          g_state.window.Height(),
                                          g_state.renderer.ColorFormat()); r.IsOk()) {
                g_state.post_available = true;
                // 効果は既定で無効（HDR 経路を通すだけ）。Set...() で有効化する。
                g_state.pp_params.bloom_enabled        = false;
                g_state.pp_params.bloom_intensity      = 0.0f;
                g_state.pp_params.vignette_intensity   = 0.0f;
                g_state.pp_params.chromatic_aberration = 0.0f;
                g_state.pp_params.grain_intensity      = 0.0f;
            } else {
                ACS_LOG_WARN("easy: ポストプロセスを初期化できませんでした（効果は無効）");
            }
        }
    }

    // 7. アセット・描画ヘルパ（ポスト有効時は FSpriteBatch を HDR RT 向けに作る）
    g_state.assets.RegisterDefaultLoaders();
    const EFormat batch_fmt = g_state.post_available
        ? g_state.post.HdrFormat() : g_state.renderer.ColorFormat();
    if (auto r = g_state.batch.Init(*dev, batch_fmt, 16384); r.IsErr()) {
        ACS_LOG_ERROR("easy: 描画系の初期化に失敗: %s", r.Error().message);
        g_state.boot_failed = true;
        return;
    }
    g_state.circle_tex = MakeCircleTexture(*dev);
    g_state.font_ok    = LoadDefaultFont(*dev);

    // 8. 音声（失敗しても続行。音が鳴らないだけ）
    g_state.audio_ok = g_state.audio.Init().IsOk();
    if (!g_state.audio_ok)
        ACS_LOG_WARN("easy: 音声を初期化できませんでした（音は鳴りません）");

    g_state.timer  = FrameTimer{};
    g_rng          = static_cast<u32>(Clock::Ticks());   // 乱数の種を起動時刻で
    if (g_rng == 0) g_rng = 0x9E3779B9u;
    g_state.booted = true;
    std::atexit(&RunShutdownOnce);
    ACS_LOG_INFO("easy: 起動しました (%u x %u)", wc.width, wc.height);
}

bool NextFrame() noexcept {
    if (g_state.boot_failed || g_state.finished) return false;
    if (!g_state.booted) {
        ACS_LOG_ERROR("easy: NextFrame() の前に OpenWindow() を呼んでください");
        g_state.finished = true;
        return false;
    }

    // 直前のフレームを閉じて画面に出す
    if (g_state.frame_open) {
        g_state.batch.End();
        if (g_state.post_available) {
            // HDR RT への描画を終え、Bloom+Tonemap 等でバックバッファへ合成する
            IRhiCommandList* cl = g_state.renderer.CommandList();
            IRhiSwapchain*   sc = g_state.renderer.Swapchain();
            g_state.pp_params.grain_time += g_state.dt;   // フィルムグレインのアニメ用
            g_state.pp_params.delta_time  = g_state.dt;   // 自動露出の順応用
            cl->EndRenderToTexture(*g_state.post.HdrRenderTarget());
            g_state.post.Render(*cl, *sc, g_state.post_buf_idx, g_state.pp_params);
            cl->EndRenderToSwapchain(*sc, g_state.post_buf_idx);
            cl->End();
            cl->Submit();
            sc->Present();
        } else {
            g_state.renderer.EndFrame();
        }
        g_state.frame_open = false;
    }

    // フレーム先頭処理
    Input::Update();
    g_state.window.PollEvents();

    // 保留中の全画面切替を、フレームの外側（リサイズが安全なこの位置）で適用
    if (g_state.fs_request >= 0) {
        g_state.window.SetFullscreen(g_state.fs_request == 1);
        g_state.fs_request = -1;
    }

    FMemorySystem::ResetTemp();
    g_state.dt = g_state.timer.Tick();

    // 終了判定（× ボタン or Quit()）
    if (g_state.quit_req || g_state.window.ShouldClose()) {
        RunShutdownOnce();
        return false;
    }

    // 新しいフレームを開始
    IRhiCommandList* cl = nullptr;
    if (g_state.post_available) {
        // ポスト有効: HDR レンダーターゲットにシーンを描く
        IRhiSwapchain* sc  = g_state.renderer.Swapchain();
        IRhiTexture*   hdr = g_state.post.HdrRenderTarget();
        if (!sc || !hdr) {
            ACS_LOG_ERROR("easy: HDR レンダーターゲットを取得できません");
            RunShutdownOnce();
            return false;
        }
        g_state.post_buf_idx = sc->AcquireNextImage();
        cl = g_state.renderer.CommandList();
        if (cl) {
            cl->Begin();
            cl->BeginRenderToTexture(*hdr, g_state.clear, nullptr, 1.0f);
            FViewport vp{};
            vp.width  = static_cast<f32>(hdr->Width());
            vp.height = static_cast<f32>(hdr->Height());
            cl->SetViewport(vp);
            FScissorRect sr{};
            sr.right  = static_cast<i32>(hdr->Width());
            sr.bottom = static_cast<i32>(hdr->Height());
            cl->SetScissor(sr);
        }
    } else {
        // ポスト無効: 従来どおりバックバッファへ直接描く
        g_state.renderer.BeginFrame(g_state.clear);
        cl = g_state.renderer.CommandList();
    }
    if (!cl) {
        ACS_LOG_ERROR("easy: コマンドリストを取得できません（描画を継続できません）");
        RunShutdownOnce();
        return false;
    }
    g_state.batch.Begin(*cl, g_state.window.Width(), g_state.window.Height());
    // カメラを恒等（カメラ無し）にリセット。FSpriteBatch::Begin の既定と一致させる。
    g_state.cam_x    = static_cast<f32>(g_state.window.Width())  * 0.5f;
    g_state.cam_y    = static_cast<f32>(g_state.window.Height()) * 0.5f;
    g_state.cam_zoom = 1.0f;
    g_state.frame_open = true;
    return true;
}

void Quit() noexcept { g_state.quit_req = true; }

void SetWindowTitle(const char* title) noexcept {
    if (!g_state.booted) return;
    ToWide(title ? title : "", g_state.title_buf, 256);
    g_state.window.SetTitle(g_state.title_buf);
}

void SetBackground(FColor color) noexcept {
    g_state.clear = ClearColor{ color.r, color.g, color.b, color.a };
}

void SetFullscreen(bool on) noexcept {
    if (g_state.booted) g_state.fs_request = on ? 1 : 0;
}
void ToggleFullscreen() noexcept {
    if (g_state.booted)
        g_state.fs_request = g_state.window.IsFullscreen() ? 0 : 1;
}
bool IsFullscreen() noexcept {
    return g_state.booted && g_state.window.IsFullscreen();
}

// ============================================================================
// ポストプロセス（Diligent ビルドでのみ有効。IsPostProcessAvailable で確認）
// ============================================================================
bool IsPostProcessAvailable() noexcept { return g_state.post_available; }

void SetBloom(f32 intensity) noexcept {
    g_state.pp_params.bloom_enabled   = (intensity > 0.0001f);
    g_state.pp_params.bloom_intensity = intensity < 0.0f ? 0.0f : intensity;
}
void SetBloomThreshold(f32 threshold) noexcept {
    g_state.pp_params.bloom_threshold = threshold < 0.0f ? 0.0f : threshold;
}
void SetExposure(f32 exposure) noexcept {
    g_state.pp_params.exposure = exposure < 0.0f ? 0.0f : exposure;
}
void SetVignette(f32 intensity) noexcept {
    g_state.pp_params.vignette_intensity = Clamp01(intensity);
}
void SetChromaticAberration(f32 amount) noexcept {
    g_state.pp_params.chromatic_aberration = amount < 0.0f ? 0.0f : amount;
}
void SetFilmGrain(f32 intensity) noexcept {
    g_state.pp_params.grain_intensity = intensity < 0.0f ? 0.0f : intensity;
}
void SetColorGrading(f32 saturation, f32 contrast, f32 temperature) noexcept {
    g_state.pp_params.cg_saturation  = saturation < 0.0f ? 0.0f : saturation;
    g_state.pp_params.cg_contrast    = contrast   < 0.0f ? 0.0f : contrast;
    g_state.pp_params.cg_temperature = temperature < -1.0f ? -1.0f
                                   : (temperature > 1.0f ? 1.0f : temperature);
}
void SetSharpness(f32 strength) noexcept {
    g_state.pp_params.cas_strength = Clamp01(strength);
}
void SetTonemap(i32 mode) noexcept {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_state.pp_params.tonemap_kind = mode;
}
void SetAutoExposure(bool enabled) noexcept {
    g_state.pp_params.auto_exposure_enabled = enabled;
}

// ============================================================================
// 図形を描く
// ============================================================================
void DrawRect(f32 x, f32 y, f32 width, f32 height, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawRect(x, y, width, height, ToVec4(color));
}

void DrawRectOutline(f32 x, f32 y, f32 width, f32 height,
                     FColor color, f32 thickness) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (thickness < 1.0f) thickness = 1.0f;
    const FVec4 c = ToVec4(color);
    g_state.batch.DrawRect(x, y, width, thickness, c);                       // 上辺
    g_state.batch.DrawRect(x, y + height - thickness, width, thickness, c);  // 下辺
    g_state.batch.DrawRect(x, y, thickness, height, c);                      // 左辺
    g_state.batch.DrawRect(x + width - thickness, y, thickness, height, c);  // 右辺
}

void DrawRectRotated(f32 x, f32 y, f32 width, f32 height,
                     f32 degrees, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    // 左上指定を中心へ変換して FSpriteBatch に渡す
    g_state.batch.DrawRectRotated(x + width * 0.5f, y + height * 0.5f,
                                 width, height, degrees * kDeg2Rad, ToVec4(color));
}

void DrawCircle(f32 x, f32 y, f32 radius, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.circle_tex || radius <= 0.0f) return;
    g_state.batch.Draw(*g_state.circle_tex, x - radius, y - radius,
                      radius * 2.0f, radius * 2.0f, ToVec4(color));
}

void DrawCircleOutline(f32 x, f32 y, f32 radius,
                       FColor color, f32 thickness) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.circle_tex || radius <= 0.0f) return;
    if (thickness < 1.0f) thickness = 1.0f;
    const FVec4 c    = ToVec4(color);
    const f32  dot_r = thickness * 0.5f;
    // 円周に沿って小さな点を並べる。点が途切れないよう個数を円周長から決める。
    const f32 step = (dot_r > 1.0f) ? dot_r : 1.0f;
    i32 count = static_cast<i32>(kTwoPi * radius / step);
    if (count < 8)    count = 8;
    if (count > 2048) count = 2048;
    for (i32 i = 0; i < count; ++i) {
        const f32 a  = 360.0f * static_cast<f32>(i) / static_cast<f32>(count);  // 度
        const f32 px = x + Cos(a) * radius;
        const f32 py = y + Sin(a) * radius;
        g_state.batch.Draw(*g_state.circle_tex, px - dot_r, py - dot_r,
                          dot_r * 2.0f, dot_r * 2.0f, c);
    }
}

void DrawLine(f32 x1, f32 y1, f32 x2, f32 y2, FColor color, f32 thickness) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (thickness < 1.0f) thickness = 1.0f;
    const f32 dx  = x2 - x1;
    const f32 dy  = y2 - y1;
    const f32 len = Sqrt(dx * dx + dy * dy);
    const FVec4 c  = ToVec4(color);
    if (len < 0.001f) {   // 長さ 0 の線は小さな四角で代用
        g_state.batch.DrawRect(x1 - thickness * 0.5f, y1 - thickness * 0.5f,
                              thickness, thickness, c);
        return;
    }
    // 線分 = 中点を中心に、長さ×太さの矩形を線の向きへ回転したもの
    g_state.batch.DrawRectRotated((x1 + x2) * 0.5f, (y1 + y2) * 0.5f,
                                 len, thickness, ATan2(dy, dx), c);
}

void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                  FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawTriangle(x1, y1, x2, y2, x3, y3, ToVec4(color));
}

void DrawTriangleOutline(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                         FColor color, f32 thickness) noexcept {
    DrawLine(x1, y1, x2, y2, color, thickness);
    DrawLine(x2, y2, x3, y3, color, thickness);
    DrawLine(x3, y3, x1, y1, color, thickness);
}

void DrawPixel(f32 x, f32 y, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawRect(x, y, 1.0f, 1.0f, ToVec4(color));
}

// ============================================================================
// 画像（スプライト）を描く
// ============================================================================
void DrawSprite(Sprite sprite, f32 x, f32 y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, s.w, s.h, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

void DrawSprite(Sprite sprite, f32 x, f32 y, f32 width, f32 height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, width, height, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

void DrawSpriteRotated(Sprite sprite, f32 x, f32 y, f32 degrees,
                       f32 scale, FColor tint) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex) return;
    const f32 w = s.w * scale;
    const f32 h = s.h * scale;
    // 左上指定を中心へ変換。回転は画像の中心まわり。
    g_state.batch.DrawRotated(*s.tex, x + w * 0.5f, y + h * 0.5f, w, h,
                             degrees * kDeg2Rad, 0, 0, 1, 1, ToVec4(tint));
}

void DrawSpriteTinted(Sprite sprite, f32 x, f32 y, FColor tint) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, s.w, s.h, ToVec4(tint));
}

void DrawSpriteFlipped(Sprite sprite, f32 x, f32 y,
                       bool flip_x, bool flip_y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex) return;
    const f32 u0 = flip_x ? 1.0f : 0.0f, u1 = flip_x ? 0.0f : 1.0f;
    const f32 v0 = flip_y ? 1.0f : 0.0f, v1 = flip_y ? 0.0f : 1.0f;
    g_state.batch.DrawSub(*s.tex, x, y, s.w, s.h, u0, v0, u1, v1,
                         FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

void DrawSpritePart(Sprite sprite, f32 x, f32 y, f32 width, f32 height,
                    f32 src_x, f32 src_y, f32 src_width, f32 src_height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    SpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex || s.w <= 0.0f || s.h <= 0.0f) return;
    const f32 u0 = src_x / s.w,                v0 = src_y / s.h;
    const f32 u1 = (src_x + src_width)  / s.w,  v1 = (src_y + src_height) / s.h;
    g_state.batch.DrawSub(*s.tex, x, y, width, height, u0, v0, u1, v1,
                         FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

f32 SpriteWidth(Sprite sprite) noexcept {
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return 0.0f;
    return g_state.sprites[sprite.id - 1].w;
}

f32 SpriteHeight(Sprite sprite) noexcept {
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return 0.0f;
    return g_state.sprites[sprite.id - 1].h;
}

// ============================================================================
// 文字を描く
// ============================================================================
void DrawString(f32 x, f32 y, const char* text, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text) return;
    DrawTextScaled(x, y, text, ToVec4(color), 1.0f);
}

void DrawString(f32 x, f32 y, const char* text, FColor color, f32 size) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text || size <= 0.0f) return;
    const f32 base = g_state.font.PixelSize();
    DrawTextScaled(x, y, text, ToVec4(color), base > 0.0f ? size / base : 1.0f);
}

void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text) return;
    const f32 w = g_state.font.MeasureWidth(text);
    DrawTextScaled(center_x - w * 0.5f, y, text, ToVec4(color), 1.0f);
}

void DrawStringCentered(f32 center_x, f32 y, const char* text,
                        FColor color, f32 size) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text || size <= 0.0f) return;
    const f32 base  = g_state.font.PixelSize();
    const f32 scale = base > 0.0f ? size / base : 1.0f;
    const f32 w     = g_state.font.MeasureWidth(text) * scale;
    DrawTextScaled(center_x - w * 0.5f, y, text, ToVec4(color), scale);
}

f32 TextWidth(const char* text) noexcept {
    if (!g_state.font_ok || !text) return 0.0f;
    return g_state.font.MeasureWidth(text);
}

f32 TextWidth(const char* text, f32 size) noexcept {
    if (!g_state.font_ok || !text) return 0.0f;
    const f32 base = g_state.font.PixelSize();
    return g_state.font.MeasureWidth(text) * (base > 0.0f ? size / base : 1.0f);
}

f32 TextHeight() noexcept {
    return g_state.font_ok ? g_state.font.LineHeight() : 0.0f;
}

f32 TextHeight(f32 size) noexcept {
    if (!g_state.font_ok) return 0.0f;
    const f32 base = g_state.font.PixelSize();
    return g_state.font.LineHeight() * (base > 0.0f ? size / base : 1.0f);
}

// ============================================================================
// カメラとクリップ
// ============================================================================
void SetCamera(f32 x, f32 y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_x = x;
    g_state.cam_y = y;
    g_state.batch.SetView(g_state.cam_x, g_state.cam_y, g_state.cam_zoom);
}

void SetCameraZoom(f32 zoom) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_zoom = (zoom > 0.01f) ? zoom : 0.01f;
    g_state.batch.SetView(g_state.cam_x, g_state.cam_y, g_state.cam_zoom);
}

void ResetCamera() noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_x    = ScreenWidth()  * 0.5f;
    g_state.cam_y    = ScreenHeight() * 0.5f;
    g_state.cam_zoom = 1.0f;
    g_state.batch.SetView(g_state.cam_x, g_state.cam_y, g_state.cam_zoom);
}

f32 CameraX() noexcept { return g_state.cam_x; }
f32 CameraY() noexcept { return g_state.cam_y; }

f32 MouseWorldX() noexcept {
    return (MouseX() - ScreenWidth() * 0.5f) / g_state.cam_zoom + g_state.cam_x;
}
f32 MouseWorldY() noexcept {
    return (MouseY() - ScreenHeight() * 0.5f) / g_state.cam_zoom + g_state.cam_y;
}

void SetClipRect(f32 x, f32 y, f32 width, f32 height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.SetClipRect(static_cast<i32>(x), static_cast<i32>(y),
                             static_cast<i32>(width), static_cast<i32>(height));
}

void ClearClipRect() noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.ClearClipRect();
}

// ============================================================================
// 素材
// ============================================================================
Sprite LoadSprite(const char* path) noexcept {
    if (!g_state.booted) {
        ACS_LOG_WARN("easy: LoadSprite() は OpenWindow() の後で呼んでください");
        return Sprite{ 0 };
    }
    if (!path) return Sprite{ 0 };
    for (usize i = 0; i < g_state.sprites.Size(); ++i) {
        const char* p = g_state.sprites[i].path.Data();
        if (p && strcmp(p, path) == 0)
            return Sprite{ static_cast<u32>(i + 1) };
    }
    wchar_t wpath[512];
    ToWide(path, wpath, 512);
    auto r = g_state.assets.Load(wpath);
    if (r.IsErr()) {
        ACS_LOG_ERROR("easy: 画像を読み込めません '%s': %s", path, r.Error().message);
        return Sprite{ 0 };
    }
    TRc<Asset> asset = r.Value();
    Asset* base = asset.Get();
    if (!base || base->Type() != FImageAsset::StaticType()) {
        ACS_LOG_ERROR("easy: '%s' は画像ファイルではありません", path);
        return Sprite{ 0 };
    }
    FImageAsset* img = static_cast<FImageAsset*>(base);
    auto tx = UploadTexture(*g_state.renderer.Device(), *img);
    if (tx.IsErr()) {
        ACS_LOG_ERROR("easy: 画像を GPU に転送できません '%s': %s", path, tx.Error().message);
        return Sprite{ 0 };
    }
    SpriteSlot slot;
    slot.tex  = Move(tx.Value());
    slot.path = FString{ path };
    slot.w    = static_cast<f32>(img->Width());
    slot.h    = static_cast<f32>(img->Height());
    g_state.sprites.PushBack(Move(slot));
    return Sprite{ static_cast<u32>(g_state.sprites.Size()) };
}

Sound LoadSound(const char* path) noexcept {
    if (!g_state.booted) {
        ACS_LOG_WARN("easy: LoadSound() は OpenWindow() の後で呼んでください");
        return Sound{ 0 };
    }
    if (!path) return Sound{ 0 };
    for (usize i = 0; i < g_state.sounds.Size(); ++i) {
        const char* p = g_state.sounds[i].path.Data();
        if (p && strcmp(p, path) == 0)
            return Sound{ static_cast<u32>(i + 1) };
    }
    wchar_t wpath[512];
    ToWide(path, wpath, 512);
    auto r = g_state.assets.Load(wpath);
    if (r.IsErr()) {
        ACS_LOG_ERROR("easy: 音声を読み込めません '%s': %s", path, r.Error().message);
        return Sound{ 0 };
    }
    TRc<Asset> asset = r.Value();
    Asset* base = asset.Get();
    if (!base || base->Type() != FAudioAsset::StaticType()) {
        ACS_LOG_ERROR("easy: '%s' は音声ファイルではありません", path);
        return Sound{ 0 };
    }
    SoundSlot slot;
    slot.asset = asset;            // TRc をコピー保持 -> 再生中ずっと生かす
    slot.path  = FString{ path };
    g_state.sounds.PushBack(Move(slot));
    return Sound{ static_cast<u32>(g_state.sounds.Size()) };
}

// ============================================================================
// 音
// ============================================================================
void Play(Sound sound) noexcept { Play(sound, 1.0f); }

void Play(Sound sound, f32 volume) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    Asset* base = g_state.sounds[sound.id - 1].asset.Get();
    if (base)
        g_state.audio.Play(*static_cast<FAudioAsset*>(base), Clamp01(volume), false);
}

void PlayLoop(Sound sound) noexcept { PlayLoop(sound, 1.0f); }

void PlayLoop(Sound sound, f32 volume) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    SoundSlot& slot = g_state.sounds[sound.id - 1];
    Asset* base = slot.asset.Get();
    if (!base) return;
    if (slot.loop.IsValid()) g_state.audio.Stop(slot.loop);   // 二重ループを防ぐ
    slot.loop = g_state.audio.Play(*static_cast<FAudioAsset*>(base),
                                  Clamp01(volume), true);
}

void StopSound(Sound sound) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    SoundSlot& slot = g_state.sounds[sound.id - 1];
    if (slot.loop.IsValid()) {
        g_state.audio.Stop(slot.loop);
        slot.loop = SoundHandle{};
    }
}

void StopAllSounds() noexcept {
    if (!g_state.audio_ok) return;
    g_state.audio.StopAll();
    for (usize i = 0; i < g_state.sounds.Size(); ++i)
        g_state.sounds[i].loop = SoundHandle{};
}

void SetMasterVolume(f32 volume) noexcept {
    if (g_state.audio_ok) g_state.audio.SetMasterVolume(Clamp01(volume));
}

void PauseAllSounds() noexcept {
    if (g_state.audio_ok) g_state.audio.PauseAll();
}

void ResumeAllSounds() noexcept {
    if (g_state.audio_ok) g_state.audio.ResumeAll();
}

// ============================================================================
// 入力 — キーボード
// ============================================================================
bool IsKeyDown    (EKey key) noexcept { return Input::IsKeyDown(key); }
bool IsKeyPressed (EKey key) noexcept { return Input::IsKeyPressed(key); }
bool IsKeyReleased(EKey key) noexcept { return Input::IsKeyReleased(key); }
const char* TextInput() noexcept { return Input::TextInput(); }

// ============================================================================
// 入力 — マウス
// ============================================================================
f32  MouseX() noexcept { return Input::MousePos().x; }
f32  MouseY() noexcept { return Input::MousePos().y; }
bool IsMouseDown    (EMouseButton button) noexcept { return Input::IsMouseButtonDown(button); }
bool IsMousePressed (EMouseButton button) noexcept { return Input::IsMouseButtonPressed(button); }
bool IsMouseReleased(EMouseButton button) noexcept { return Input::IsMouseButtonReleased(button); }
f32  MouseWheel() noexcept { return Input::MouseWheel(); }

// ============================================================================
// 入力 — ゲームパッド
// ============================================================================
bool IsGamepadConnected(i32 player) noexcept {
    return Input::IsGamepadConnected(GpIdx(player));
}
bool IsGamepadDown(EGamepadButton button, i32 player) noexcept {
    return Input::IsGamepadButtonDown(GpIdx(player), button);
}
bool IsGamepadPressed(EGamepadButton button, i32 player) noexcept {
    return Input::IsGamepadButtonPressed(GpIdx(player), button);
}
f32 GamepadLeftX(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftX);
}
f32 GamepadLeftY(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftY);
}
f32 GamepadRightX(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightX);
}
f32 GamepadRightY(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightY);
}
f32 GamepadLeftTrigger(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftTrigger);
}
f32 GamepadRightTrigger(i32 player) noexcept {
    return Input::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightTrigger);
}

// ============================================================================
// 乱数
// ============================================================================
i32 RandomInt(i32 min, i32 max) noexcept {
    if (min > max) { i32 t = min; min = max; max = t; }
    // 符号なしで引き算（符号付きの桁あふれは未定義動作のため）
    const u32 range = static_cast<u32>(max) - static_cast<u32>(min);
    if (range == 0xFFFFFFFFu)                       // i32 全域
        return static_cast<i32>(NextRng());
    const u32 span      = range + 1u;
    const u32 threshold = (0u - span) % span;       // 2^32 % span（剰余バイアス境界）
    u32 r;
    do { r = NextRng(); } while (r < threshold);    // バイアスのある領域は棄却
    return static_cast<i32>(static_cast<u32>(min) + r % span);
}

f32 RandomFloat(f32 min, f32 max) noexcept {
    // 24bit 分の乱数を [0,1) に。float の仮数部に収まるので max ちょうどは出ない。
    const f32 t = static_cast<f32>(NextRng() >> 8) / 16777216.0f;
    return min + (max - min) * t;
}

bool RandomBool() noexcept { return (NextRng() & 1u) != 0u; }

void RandomSeed(u32 seed) noexcept {
    g_rng = seed ? seed : 0x9E3779B9u;
}

// ============================================================================
// 当たり判定
// ============================================================================
bool RectsOverlap(f32 x1, f32 y1, f32 w1, f32 h1,
                  f32 x2, f32 y2, f32 w2, f32 h2) noexcept {
    return x1 < x2 + w2 && x2 < x1 + w1 &&
           y1 < y2 + h2 && y2 < y1 + h1;
}

bool CirclesOverlap(f32 x1, f32 y1, f32 r1,
                    f32 x2, f32 y2, f32 r2) noexcept {
    const f32 dx = x2 - x1, dy = y2 - y1, rr = r1 + r2;
    return dx * dx + dy * dy < rr * rr;
}

bool PointInRect(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) noexcept {
    return px >= x && px < x + w && py >= y && py < y + h;
}

bool PointInCircle(f32 px, f32 py, f32 cx, f32 cy, f32 r) noexcept {
    const f32 dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy < r * r;
}

// ============================================================================
// 数学ヘルパ
// ============================================================================
f32 Clamp(f32 value, f32 lo, f32 hi) noexcept {
    return value < lo ? lo : (value > hi ? hi : value);
}
f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }
f32 Distance(f32 x1, f32 y1, f32 x2, f32 y2) noexcept {
    const f32 dx = x2 - x1, dy = y2 - y1;
    return Sqrt(dx * dx + dy * dy);
}
f32 Min(f32 a, f32 b) noexcept { return a < b ? a : b; }
f32 Max(f32 a, f32 b) noexcept { return a > b ? a : b; }
f32 Abs(f32 value)    noexcept { return value < 0.0f ? -value : value; }

f32 Sin(f32 degrees) noexcept { return acs::Sin(degrees * kDeg2Rad); }
f32 Cos(f32 degrees) noexcept { return acs::Cos(degrees * kDeg2Rad); }
f32 Sqrt(f32 value)  noexcept { return acs::Sqrt(value); }
f32 AngleTo(f32 x1, f32 y1, f32 x2, f32 y2) noexcept {
    return ATan2(y2 - y1, x2 - x1) * kRad2Deg;
}

// ============================================================================
// セーブ／ロード
// ============================================================================
void SaveInt(const char* key, i32 value) noexcept {
    if (!key) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    SetSaveValue(key, buf);
}

i32 LoadInt(const char* key, i32 default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    SaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    if (!s || !*s) return default_value;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return default_value;          // パース不可
    return static_cast<i32>(v);
}

void SaveFloat(const char* key, f32 value) noexcept {
    if (!key) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "%.9g", static_cast<f64>(value));
    SetSaveValue(key, buf);
}

f32 LoadFloat(const char* key, f32 default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    SaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    if (!s || !*s) return default_value;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return default_value;
    return static_cast<f32>(v);
}

void SaveString(const char* key, const char* value) noexcept {
    if (!key) return;
    SetSaveValue(key, value ? value : "");
}

const char* LoadString(const char* key, const char* default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    SaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    return s ? s : default_value;
}

bool HasSaveKey(const char* key) noexcept {
    if (!key) return false;
    EnsureSaveLoaded();
    return FindSave(key) != nullptr;
}

void DeleteAllSaves() noexcept {
    g_save = TArray<SaveEntry>{};
    g_save_loaded = true;        // 「ロード済み（空）」状態にする
    FILE* f = nullptr;
    if (fopen_s(&f, "save.dat", "wb") == 0 && f) fclose(f);
}

// ============================================================================
// 情報
// ============================================================================
f32 DeltaTime()    noexcept { return g_state.dt; }
f32 ElapsedTime()  noexcept { return static_cast<f32>(g_state.timer.TotalSeconds()); }
i32 Fps()          noexcept { return static_cast<i32>(g_state.timer.SmoothedFPS() + 0.5f); }
f32 ScreenWidth()  noexcept { return static_cast<f32>(g_state.window.Width()); }
f32 ScreenHeight() noexcept { return static_cast<f32>(g_state.window.Height()); }

} // namespace acs::easy
