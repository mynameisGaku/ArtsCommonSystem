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
#include "memory/SystemAllocator.h"
#include "memory/UniquePtr.h"
#include "memory/SharedPtr.h"
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
#include "render/BurnEffect.h"
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

/** 赤の定数色。 */
const FColor FColor::Red    { 0.92f, 0.26f, 0.26f, 1.0f };

/** 緑の定数色。 */
const FColor FColor::Green  { 0.30f, 0.78f, 0.36f, 1.0f };

/** 青の定数色。 */
const FColor FColor::Blue   { 0.26f, 0.49f, 0.96f, 1.0f };

/** 黄の定数色。 */
const FColor FColor::Yellow { 0.98f, 0.83f, 0.25f, 1.0f };

/** シアンの定数色。 */
const FColor FColor::Cyan   { 0.25f, 0.83f, 0.93f, 1.0f };

/** マゼンタの定数色。 */
const FColor FColor::Magenta{ 0.93f, 0.33f, 0.78f, 1.0f };

/** 白の定数色。 */
const FColor FColor::White  { 1.0f,  1.0f,  1.0f,  1.0f };

/** 黒の定数色。 */
const FColor FColor::Black  { 0.0f,  0.0f,  0.0f,  1.0f };

/** 灰の定数色。 */
const FColor FColor::Gray   { 0.52f, 0.54f, 0.60f, 1.0f };

/** 橙の定数色。 */
const FColor FColor::Orange { 1.0f,  0.55f, 0.15f, 1.0f };

/** 空色の定数色。 */
const FColor FColor::Sky    { 0.45f, 0.70f, 0.95f, 1.0f };

/** 完全透明 (全成分 0) の定数色。 */
const FColor FColor::Clear  { 0.0f,  0.0f,  0.0f,  0.0f };

namespace {

/**
 * Easy のプロセス寿命状態が使う明示的な確保元。
 *
 * @details MemorySystem の起動・停止をまたいで EasyState が生存するため、静的コンテナは
 * 実行時の DefaultAllocator を保持せず、常にプロセスヒープから確保する。
 */
FSystemAllocator g_easy_allocator;

/**
 * LoadSprite が確保する 1 枚分のスプライト情報。
 */
struct FSpriteSlot {
    /** プロセス寿命状態へ残るパスの確保元を固定する。 */
    FSpriteSlot() noexcept : path(g_easy_allocator)
    {
    }

    /** GPU テクスチャ (所有権を持つ)。 */
    TUniquePtr<IRhiTexture> tex;

    /** 読み込み元パス (再ロード時の同一判定に使う)。 */
    FString                 path;

    /** 元画像の幅 (ピクセル)。 */
    f32                    w = 0.0f;

    /** 元画像の高さ (ピクセル)。 */
    f32                    h = 0.0f;
};

/**
 * LoadSound が確保する 1 つ分のサウンド情報。
 */
struct FSoundSlot {
    /** プロセス寿命状態へ残るパスの確保元を固定する。 */
    FSoundSlot() noexcept : path(g_easy_allocator)
    {
    }

    /** 音声アセット (再生中ずっと生かしておくため TSharedPtr で保持)。 */
    TSharedPtr<FAsset>   asset;

    /** 読み込み元パス (再ロード時の同一判定に使う)。 */
    FString      path;

    /** PlayLoop 中の再生ハンドル (StopSound 用。無効値で初期化)。 */
    FSoundHandle loop{};
};

/**
 * easy API 全体の単一グローバル状態 (このファイル内のみ、単一スレッド前提)。
 *
 * @details
 * エンジンオブジェクトの宣言順 = 構築順 (破棄は逆順)。カメラは毎フレーム恒等に
 * リセットされ、演出 (シェイク/フラッシュ) は NextFrame で駆動される。
 */
struct FEasyState {
    /** プロセス寿命アロケータを静的コンテナへ固定して構築する。 */
    FEasyState() noexcept
        : assets(g_easy_allocator),
          sprites(g_easy_allocator),
          sounds(g_easy_allocator),
          graph_closures(g_easy_allocator),
          graph_handles(g_easy_allocator)
    {
    }

    /** OpenWindow に成功した。 */
    bool booted      = false;

    /** OpenWindow に失敗した。 */
    bool boot_failed = false;

    /** OpenWindow がロガーを起動し、終了責任を持つか。 */
    bool logger_owned = false;

    /** OpenWindow が MemorySystem を起動し、終了責任を持つか。 */
    bool memory_system_owned = false;

    /** OpenWindow が ThreadPool を起動または jobs から引き継ぎ、終了責任を持つか。 */
    bool thread_pool_owned = false;

    /** NextFrame が false を返し、後始末も済んだ。 */
    bool finished    = false;

    /** FSpriteBatch::Begin 済み (この間だけ描画可能)。 */
    bool frame_open  = false;

    /** Quit() が呼ばれた。 */
    bool quit_req    = false;

    /** Resize failure: do not record into a possibly missing backbuffer. */
    bool renderer_failure_pending = false;

    /** 「枠の外で描画した」警告を 1 度だけ出すためのフラグ。 */
    bool warned_draw = false;

    /** 保留中の全画面切替 (-1:無し 0:窓 1:全画面)。 */
    i32  fs_request  = -1;

    /** OS ウィンドウ。 */
    FWindow        window;

    /** レンダラ (GPU デバイス・スワップチェインを保持)。 */
    FRenderer      renderer;

    /** アセットレジストリ (画像・音声の読み込み)。 */
    FAssetRegistry assets;

    /** 音声エンジン。 */
    CAudioEngine   audio;

    /** 音声エンジンの初期化に成功したか。 */
    bool          audio_ok = false;

    /** 図形・スプライト・文字の描画を集約するスプライトバッチ。 */
    FSpriteBatch   batch;

    /** 紙が燃える per-pixel ディゾルブ効果 (どのバックエンドでも動く)。 */
    FBurnEffect    burn;

    /** 既定フォント (DrawString 用)。 */
    FFont          font;

    /** フォントの読み込みに成功したか。 */
    bool          font_ok  = false;

    /** DrawCircle 用の白い円テクスチャ。 */
    TUniquePtr<IRhiTexture> circle_tex;

    /** 読み込み済みスプライトのスロット配列 (id-1 で参照)。 */
    TArray<FSpriteSlot> sprites;

    /** 読み込み済みサウンドのスロット配列 (id-1 で参照)。 */
    TArray<FSoundSlot>  sounds;

    /** フレーム時間計測 (dt/FPS/累積時間)。 */
    FFrameTimer  timer;

    /** 前フレームからの経過秒。 */
    f32         dt    = 0.0f;

    /** 背景クリア色 (既定は濃い紺色)。 */
    FClearColor  clear { 0.10f, 0.12f, 0.16f, 1.0f };

    /** カメラ中心の X 座標 (ワールド座標)。 */
    f32 cam_x = 0.0f;

    /** カメラ中心の Y 座標 (ワールド座標)。 */
    f32 cam_y = 0.0f;

    /** カメラのズーム倍率。 */
    f32 cam_zoom = 1.0f;

    /** 画面シェイクのトラウマ値 (0〜1、毎フレーム減衰)。 */
    f32    shake_trauma = 0.0f;

    /** 画面シェイクの現フレーム X オフセット。 */
    f32    shake_dx     = 0.0f;

    /** 画面シェイクの現フレーム Y オフセット。 */
    f32    shake_dy = 0.0f;

    /** フラッシュ演出の色。 */
    FColor flash_color{};

    /** フラッシュの残り時間 (秒)。 */
    f32    flash_timer  = 0.0f;

    /** フラッシュの全体持続時間 (秒)。 */
    f32    flash_dur = 0.0f;

    /** ドラッグ/押下中のウィジェット id (0=無し)。 */
    u32    ui_active = 0;

    /** 即席 UI の通常時背景色。 */
    FColor ui_base  { 0.20f, 0.40f, 0.75f, 0.95f };

    /** 即席 UI のホバー時背景色。 */
    FColor ui_hover { 0.30f, 0.52f, 0.92f, 1.0f  };

    /** 即席 UI の押下時背景色。 */
    FColor ui_press { 0.14f, 0.30f, 0.58f, 1.0f  };

    /** 即席 UI の文字・ノブ色。 */
    FColor ui_text  { 1.0f,  1.0f,  1.0f,  1.0f  };

    /** ポストプロセス (HDR 経路。Diligent backend でのみ有効)。 */
    FPostProcess       post;

    /** ポストプロセスが利用可能か。 */
    bool              post_available = false;

    /** 現フレームのスワップチェインバックバッファ番号。 */
    u32               post_buf_idx   = 0;

    /** ポストプロセスのパラメータ (Bloom/露出など)。 */
    FPostProcessParams pp_params;

    /** ウィンドウタイトルの UTF-16 バッファ。 */
    wchar_t title_buf[256] = L"ACS Easy";

    /** 非同期バッチスロットの最大数。 */
    static constexpr u32 kMaxBatches = 64;

    /**
     * RunAsync で投入したジョブ群 1 つ分の管理スロット。
     *
     * @details メインスレッドからのみ操作する。スロットは世代付きで再利用する。
     */
    struct FAsyncBatch {
        /** クロージャ配列の確保元をプロセス寿命アロケータへ固定する。 */
        FAsyncBatch() noexcept : closures(g_easy_allocator)
        {
        }

        /** このバッチの全ジョブの完了カウンタ (非コピーなので in-place 構築)。 */
        FCompletionCounter counter;

        /** このバッチが所有する jobdetail::Closure* の配列。 */
        TArray<void*>     closures;

        /** 世代 (スロット再利用で古い JobBatch を無効化するため)。 */
        u16               gen  = 0;

        /** スロットが使用中か。 */
        bool              live = false;
    };

    /** 非同期バッチスロットの固定配列。 */
    FAsyncBatch        async_batches[kMaxBatches];

    /** RunJobs 用に構築中の依存グラフ (未構築なら nullptr)。 */
    FJobGraph*        pending_graph = nullptr;

    /** pending_graph の各ノードの Closure* (所有)。 */
    TArray<void*>     graph_closures;

    /** JobNode.id-1 → JobHandle の対応表。 */
    TArray<FJobHandle> graph_handles;

    /** jobs が自前で ThreadPool を Init したか (後始末の責任判定)。 */
    bool              jobs_pool_owned = false;

    /** jobs 用 atexit を登録済みか。 */
    bool              jobs_atexit     = false;

    /** Easy 本体の atexit を登録済みか。再起動しても重複登録しない。 */
    bool shutdown_atexit = false;
};

/** easy API 全体の唯一のグローバル状態インスタンス。 */
FEasyState g_state;

/**
 * UTF-8 文字列を UTF-16 に変換して out に書く。
 *
 * @details 変換失敗時は空文字列にし、末尾の NUL 終端を必ず保証する。
 * @param utf8 変換元の UTF-8 文字列 (nullptr 可)。
 * @param out 変換結果を書き込む UTF-16 バッファ。
 * @param out_len out の要素数。
 */
void ToWide(const char* utf8, wchar_t* out, int out_len) noexcept {
    if (out_len <= 0) return;
    out[0] = 0;
    if (!utf8) return;
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_len);
    if (n <= 0) out[0] = 0;            // 変換失敗 -> 空文字列
    out[out_len - 1] = 0;              // 念のため終端を保証
}

/**
 * FColor を FVec4 (r,g,b,a) に変換する。
 *
 * @param c 変換元の色。
 * @return 対応する FVec4。
 */
inline FVec4 ToVec4(FColor c)   noexcept { return FVec4{ c.r, c.g, c.b, c.a }; }

/**
 * 値を [0,1] にクランプする。
 *
 * @param v 対象の値。
 * @return [0,1] に収めた値。
 */
inline f32  Clamp01(f32 v)    noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/**
 * プレイヤー番号を 0..3 に収める。
 *
 * @param player 入力のプレイヤー番号。
 * @return 0〜3 にクランプしたインデックス。
 */
inline u32 GpIdx(i32 player) noexcept {
    if (player < 0) return 0;
    if (player > 3) return 3;
    return static_cast<u32>(player);
}

/** 乱数の状態 (xorshift32。軽量でゲーム用途には十分な品質)。 */
u32 g_rng = 0x9E3779B9u;

/**
 * xorshift32 で次の乱数を生成して返す。
 *
 * @details 状態が 0 に落ち込まないよう保険を入れる。
 * @return 0 でない 32bit 乱数。
 */
u32 NextRng() noexcept {
    u32 x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x ? x : 0x9E3779B9u;   // 0 に落ち込まないよう保険
    return g_rng;
}

/**
 * DrawCircle / DrawCircleOutline が使う白い円テクスチャを生成する。
 *
 * @details 128x128 の R8G8B8A8 で、端 1px をアルファでなめらかにする。失敗時は空ハンドル。
 * @param device テクスチャ生成に使う RHI デバイス。
 * @return 生成した円テクスチャ (失敗時は空)。
 */
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

/**
 * OS 標準フォント候補を順に試して既定フォントを読み込む。
 *
 * @details
 * アトラスは 4096x4096 (GPU テクスチャ約 67MB)。CJK 統合漢字約 2 万字を全部収めるには
 * 2048 では足りず溢れたグリフが脱落するため 4096 まで広げている。すべて失敗したら警告を出す。
 * @param device フォントアトラス生成に使う RHI デバイス。
 * @return いずれかのフォントを読み込めたら true。
 */
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

/**
 * フォントのグリフを並べ、scale 倍で拡縮してテキストを描く。
 *
 * @details UTF-8 をデコードしながらアトラスのグリフを FSpriteBatch に積む。\n で改行する。
 * @param x 左上の X 座標。
 * @param y 左上の Y 座標。
 * @param text 描画する文字列 (UTF-8)。
 * @param color 文字色 (RGBA)。
 * @param scale 拡大率 (1.0 で基準サイズ)。
 */
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
        FGlyphInfo g{};
        if (!g_state.font.GetGlyph(cp, g)) continue;
        const f32 qx = pen_x     + g.x_offset * scale;
        const f32 qy = baseline + g.y_offset * scale;
        g_state.batch.DrawSub(*atlas, qx, qy, g.width * scale, g.height * scale,
                             g.u0, g.v0, g.u1, g.v1, color);
        pen_x += g.x_advance * scale;
    }
}

/**
 * ウィンドウイベントを Input / FRenderer へ橋渡しするコールバック。
 *
 * @details リサイズ時はレンダラとポストプロセスのリサイズも行う。
 * @param user 未使用のユーザポインタ。
 * @param e 処理するウィンドウイベント。
 */
void EasyEventBridge(void* /*user*/, const FEvent& e) noexcept {
    FInput::OnEvent(e);
    if (g_state.renderer_failure_pending) return;
    if (e.type == EEventType::WindowResize) {
        if (!g_state.renderer.OnResize(
                e.resize.width, e.resize.height)) {
            g_state.renderer_failure_pending = true;
            return;
        }
        if (g_state.post_available)
            (void)g_state.post.Resize(e.resize.width, e.resize.height);
    }
}

/**
 * FThreadPool / FJobGraph に渡すタスク関数 (Closure を実行する)。
 *
 * @details 完了カウンタは pool が自動で Done するためここでは触らない。
 * @param user 実行する jobdetail::Closure へのポインタ。
 * @param worker 呼び出し元ワーカ番号 (未使用)。
 */
void RunClosureTask(void* user, u32 /*worker*/) noexcept {
    auto* c = static_cast<jobdetail::FClosure*>(user);
    c->invoke(c);
}

/**
 * Closure のラムダ本体を破棄してメモリを解放する。
 *
 * @param c 解放するクロージャ (nullptr 可)。
 */
void FreeClosure(jobdetail::FClosure* c) noexcept {
    jobdetail::DestroyClosure(c);
}

/**
 * 未 Wait のバッチを待って解放し、構築途中のグラフも破棄する。
 *
 * @details 冪等で、Shutdown / atexit から呼ばれる。
 */
void CleanupJobs() noexcept {
    for (u32 i = 0; i < FEasyState::kMaxBatches; ++i) {
        FEasyState::FAsyncBatch& b = g_state.async_batches[i];
        if (!b.live) continue;
        FThreadPool::Wait(b.counter);
        for (usize k = 0; k < b.closures.Size(); ++k)
            FreeClosure(static_cast<jobdetail::FClosure*>(b.closures[k]));
        b.closures.ReleaseStorage();
        b.live = false;
    }
    if (g_state.pending_graph) {
        for (usize k = 0; k < g_state.graph_closures.Size(); ++k)
            FreeClosure(static_cast<jobdetail::FClosure*>(g_state.graph_closures[k]));
        g_state.graph_closures.ReleaseStorage();
        g_state.graph_handles.ReleaseStorage();
        delete g_state.pending_graph;
        g_state.pending_graph = nullptr;
    }
}

/**
 * OpenWindow を呼ばずにジョブだけ使った場合の後始末 (atexit から呼ばれる)。
 *
 * @details jobs が自前で ThreadPool を起動していたらここで Shutdown する。
 */
void JobsAtexit() {
    CleanupJobs();
    if (g_state.jobs_pool_owned) { FThreadPool::Shutdown(); g_state.jobs_pool_owned = false; }
}

/** セーブ用の静的コンテナを、メモリシステム停止前に解放する。 */
void ReleaseSaveStorage() noexcept;

/**
 * easy の全リソースを後始末する (NextFrame がウィンドウ閉鎖を検知したとき 1 度だけ呼ぶ)。
 *
 * @details
 * 未完了ジョブを待ってから GPU の処理完了を待ち、描画系・音声・アセット・レンダラ・
 * スレッドプール・メモリシステムを宣言と逆順で破棄する (use-after-free 防止)。
 */
void ShutdownEasy() noexcept {
    CleanupJobs();                            // 未完了ジョブを待ってクロージャを解放 (pool 破棄前)
    // GPU の処理完了を待ってから GPU リソースを解放する（use-after-free 防止）
    if (g_state.renderer.Device() &&
        g_state.renderer.Device()->IsOperational()) {
        g_state.renderer.Device()->WaitIdle();
    }
    g_state.batch.Shutdown();
    g_state.font.Shutdown();
    g_state.font_ok = false;
    g_state.circle_tex.Reset();
    // 静的状態が持つ配列の確保元は維持する。空配列のムーブ代入で現在の
    // MemorySystem アロケータへ差し替えると、停止後の再利用時に無効な確保元を触る。
    g_state.sprites.ReleaseStorage(); // 全スプライトのテクスチャを解放
    // 部分初期化で失敗していても Shutdown は安全なので、成功フラグに依存させない。
    g_state.audio.Shutdown();
    g_state.audio_ok = false;
    g_state.sounds.ReleaseStorage();
    g_state.assets.Shutdown();
    ReleaseSaveStorage(); // FString の確保元が生きている間に静的容量も返す
    g_state.burn.Shutdown();
    g_state.post.Shutdown();
    g_state.post_available = false;
    g_state.renderer.Shutdown();             // ここで GPU デバイスを破棄する
    g_state.window = FWindow{};              // HWND をプロセス終了まで保持しない

    if (g_state.thread_pool_owned) {
        FThreadPool::Shutdown();
        g_state.thread_pool_owned = false;
    }
    if (g_state.memory_system_owned) {
        FMemorySystem::Shutdown();
        g_state.memory_system_owned = false;
    }
    ACS_LOG_INFO("easy: 終了しました");
    if (g_state.logger_owned) {
        FLogger::Flush();
        FLogger::Shutdown();
        g_state.logger_owned = false;
    }

    g_state.booted = false;
    g_state.frame_open = false;
    g_state.quit_req = false;
    g_state.renderer_failure_pending = false;
}

/** OpenWindow の途中失敗を逆順に巻き戻し、失敗状態を確定する。 */
void FailEasyStartup() noexcept
{
    ShutdownEasy();
    g_state.boot_failed = true;
}

/** 起動済みかつ未終了なら ShutdownEasy を 1 度だけ呼ぶ (多重後始末防止)。 */
void RunShutdownOnce() {
    if (g_state.booted && !g_state.finished) {
        ShutdownEasy();
        g_state.finished = true;
    }
}

/** 「フレーム外で描画した」警告を 1 度だけ出す。 */
void WarnDrawOutsideFrame() noexcept {
    if (g_state.warned_draw) return;
    g_state.warned_draw = true;
    ACS_LOG_WARN("easy: 描画関数は OpenWindow() の後、while(NextFrame()) の中で呼んでください");
}

/**
 * 全画面フラッシュ演出をフレーム最前面に重ね、残り時間を減衰させる。
 *
 * @details NextFrame の提示直前に呼ぶ。フラッシュ無効時は何もしない。
 */
void DrawScreenOverlays() noexcept {
    if (g_state.flash_timer <= 0.0f || g_state.flash_dur <= 0.0f) return;
    const f32 w = ScreenWidth(), h = ScreenHeight();
    g_state.batch.SetView(w * 0.5f, h * 0.5f, 1.0f);
    FColor c = g_state.flash_color;
    c.a *= Clamp01(g_state.flash_timer / g_state.flash_dur);
    g_state.batch.DrawRect(0.0f, 0.0f, w, h, ToVec4(c));
    g_state.flash_timer -= g_state.dt;
    if (g_state.flash_timer < 0.0f) g_state.flash_timer = 0.0f;
}

/**
 * セーブデータ 1 件分の key=value エントリ。
 */
struct FSaveEntry {
    /** OpenWindow の有無に依存しないセーブ API 用に確保元を固定する。 */
    FSaveEntry() noexcept : key(g_easy_allocator), value(g_easy_allocator)
    {
    }

    /** エントリのキー。 */
    FString key;

    /** エントリの値。 */
    FString value;
};

/** ロード済みセーブエントリの配列 (save.dat の内容)。 */
TArray<FSaveEntry> g_save(g_easy_allocator);

/** save.dat を 1 度ロード済みか (再ロードを避ける)。 */
bool             g_save_loaded = false;

/** セーブ内容と予約済み容量を解放し、次回アクセス時に再読込できる状態へ戻す。 */
void ReleaseSaveStorage() noexcept
{
    g_save.ReleaseStorage();
    g_save_loaded = false;
}

/**
 * save.dat を一度だけ読み込んで g_save に展開する。
 *
 * @details 既にロード済みなら何もしない。ファイルが無い・空のときは空のまま終える。
 */
void EnsureSaveLoaded() noexcept {
    if (g_save_loaded) return;
    g_save_loaded = true;
    FILE* f = nullptr;
    if (fopen_s(&f, "save.dat", "rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
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
                FSaveEntry e;
                e.key = FString{line, g_easy_allocator};
                e.value = FString{eq + 1, g_easy_allocator};
                g_save.PushBack(Move(e));
            }
        }
    }
    fclose(f);
}

/**
 * キーに一致するセーブエントリを線形探索して返す。
 *
 * @param key 探すキー。
 * @return 一致したエントリへのポインタ (無ければ nullptr)。
 */
FSaveEntry* FindSave(const char* key) noexcept {
    for (usize i = 0; i < g_save.Size(); ++i) {
        const char* k = g_save[i].key.Data();
        if (k && strcmp(k, key) == 0) return &g_save[i];
    }
    return nullptr;
}

/**
 * g_save の全エントリを key=value 形式で save.dat に書き出す。
 *
 * @details 書き込めない場合は (ロガー稼働中のみ) 警告を出す。
 */
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

/**
 * キーに値を設定し (既存なら上書き、無ければ追加)、即座に save.dat へ書き出す。
 *
 * @param key 設定するキー。
 * @param value 設定する値。
 */
void SetSaveValue(const char* key, const char* value) noexcept {
    EnsureSaveLoaded();
    FSaveEntry* e = FindSave(key);
    if (e) {
        e->value = FString{value, g_easy_allocator};
    } else {
        FSaveEntry ne;
        ne.key = FString{key, g_easy_allocator};
        ne.value = FString{value, g_easy_allocator};
        g_save.PushBack(Move(ne));
    }
    WriteSaveFile();
}

} // namespace

/** 0〜255 の整数で不透明色を作る。 */
FColor Rgb(u8 r, u8 g, u8 b) noexcept {
    return FColor{ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
}

/** 0〜255 の整数で半透明込みの色を作る。 */
FColor Rgba(u8 r, u8 g, u8 b, u8 a) noexcept {
    return FColor{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
}

/** 不透明度だけを変えた色を返す。 */
FColor Fade(FColor color, f32 alpha) noexcept {
    color.a = Clamp01(alpha);
    return color;
}

/** ウィンドウを開いてエンジン一式を初期化する。 */
void OpenWindow(i32 width, i32 height, const char* title) noexcept {
    if (g_state.booted || g_state.boot_failed) {
        ACS_LOG_WARN("easy: OpenWindow() は既に呼ばれています");
        return;
    }

    // 正常終了後の再起動では前 lifecycle の終了フラグを持ち越さない。
    g_state.finished = false;
    g_state.frame_open = false;
    g_state.quit_req = false;
    g_state.renderer_failure_pending = false;
    g_state.warned_draw = false;
    g_state.fs_request = -1;
    g_state.ui_active = 0;

    // 1. ロガー。呼出側が既に起動している場合は借用し、終了時にも停止しない。
    if (!FLogger::IsInitialized()) {
        FLogConfig lc{};
        FLogger::Init(lc);
        g_state.logger_owned = FLogger::IsInitialized();
    }

    // 2. メモリシステム
    if (FMemorySystem::Get(ESegment::Default) == nullptr) {
        if (auto r = FMemorySystem::Init(FMemorySystem::DefaultConfig()); r.IsErr()) {
            ACS_LOG_ERROR("easy: メモリシステムの初期化に失敗: %s", r.Error().message);
            FailEasyStartup();
            return;
        }
        g_state.memory_system_owned = true;
    }
    // 3. スレッドプール
    if (FThreadPool::WorkerCount() == 0) {
        if (auto r = FThreadPool::Init(); r.IsErr()) {
            ACS_LOG_ERROR("easy: スレッドプールの初期化に失敗: %s", r.Error().message);
            FailEasyStartup();
            return;
        }
        g_state.thread_pool_owned = true;
    } else if (g_state.jobs_pool_owned) {
        // OpenWindow より先に jobs が起動したプールを Easy 本体へ引き継ぐ。
        g_state.jobs_pool_owned = false;
        g_state.thread_pool_owned = true;
    }
    // 4. ウィンドウ
    ToWide(title ? title : "ACS Game", g_state.title_buf, 256);
    FWindowConfig wc{};
    wc.title  = g_state.title_buf;
    wc.width  = (width  > 0) ? static_cast<u32>(width)  : 1280;
    wc.height = (height > 0) ? static_cast<u32>(height) : 720;
    auto wr = FWindow::Create(wc);
    if (wr.IsErr()) {
        ACS_LOG_ERROR("easy: ウィンドウの作成に失敗: %s", wr.Error().message);
        FailEasyStartup();
        return;
    }
    g_state.window = Move(wr.Value());
    g_state.window.SetEventCallback(&EasyEventBridge, nullptr);

    // 5. レンダラ（easy は 2D 専用なので深度バッファは不要）
    if (auto r = g_state.renderer.Init(g_state.window, false, /*enable_depth=*/false); r.IsErr()) {
        ACS_LOG_ERROR("easy: レンダラの初期化に失敗: %s", r.Error().message);
        FailEasyStartup();
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
    g_state.assets.Restart();
    g_state.assets.RegisterDefaultLoaders();
    const EFormat batch_fmt = g_state.post_available
        ? g_state.post.HdrFormat() : g_state.renderer.ColorFormat();
    if (auto r = g_state.batch.Init(*dev, batch_fmt, 16384); r.IsErr()) {
        ACS_LOG_ERROR("easy: 描画系の初期化に失敗: %s", r.Error().message);
        FailEasyStartup();
        return;
    }
    g_state.circle_tex = MakeCircleTexture(*dev);
    g_state.font_ok    = LoadDefaultFont(*dev);

    // 燃えディゾルブ効果 (失敗しても続行: DrawBurnDissolve が no-op になるだけ)
    if (auto r = g_state.burn.Init(*dev, batch_fmt); r.IsErr())
        ACS_LOG_WARN("easy: 燃えディゾルブ効果を初期化できませんでした");

    // 8. 音声（失敗しても続行。音が鳴らないだけ）
    g_state.audio_ok = g_state.audio.Init().IsOk();
    if (!g_state.audio_ok)
        ACS_LOG_WARN("easy: 音声を初期化できませんでした（音は鳴りません）");

    g_state.timer  = FFrameTimer{};
    g_rng          = static_cast<u32>(FClock::Ticks());   // 乱数の種を起動時刻で
    if (g_rng == 0) g_rng = 0x9E3779B9u;
    g_state.booted = true;
    if (!g_state.shutdown_atexit) {
        std::atexit(&RunShutdownOnce);
        g_state.shutdown_atexit = true;
    }
    ACS_LOG_INFO("easy: 起動しました (%u x %u)", wc.width, wc.height);
}

/** 1 フレームを駆動する (前フレーム提示 → 入力 → クリア → 新フレーム開始)。 */
bool NextFrame() noexcept {
    if (g_state.boot_failed || g_state.finished) return false;
    if (!g_state.booted) {
        ACS_LOG_ERROR("easy: NextFrame() の前に OpenWindow() を呼んでください");
        g_state.finished = true;
        return false;
    }

    // 直前のフレームを閉じて画面に出す
    if (g_state.frame_open) {
        DrawScreenOverlays();
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
            if (!cl->Submit() || !sc->Present()) {
                ACS_LOG_ERROR(
                    "easy: renderer submit/present failed; "
                    "stopping the frame loop");
                g_state.frame_open = false;
                RunShutdownOnce();
                return false;
            }
        } else {
            if (!g_state.renderer.EndFrame()) {
                ACS_LOG_ERROR(
                    "easy: renderer submit/present failed; "
                    "stopping the frame loop");
                g_state.frame_open = false;
                RunShutdownOnce();
                return false;
            }
        }
        g_state.frame_open = false;
    }

    // フレーム先頭処理
    FInput::Update();
    g_state.window.PollEvents();

    // 保留中の全画面切替を、フレームの外側（リサイズが安全なこの位置）で適用
    if (g_state.fs_request >= 0) {
        g_state.window.SetFullscreen(g_state.fs_request == 1);
        g_state.fs_request = -1;
    }
    if (g_state.renderer_failure_pending) {
        ACS_LOG_ERROR(
            "easy: renderer resize failed; "
            "stopping before frame recording");
        RunShutdownOnce();
        return false;
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

    // 画面シェイク: trauma を減衰させ、view にランダムオフセットを与える
    if (g_state.shake_trauma > 0.0f) {
        g_state.shake_trauma = Max(0.0f, g_state.shake_trauma - g_state.dt * 1.6f);
        const f32 amount = g_state.shake_trauma * g_state.shake_trauma * 22.0f;
        g_state.shake_dx = RandomFloat(-amount, amount);
        g_state.shake_dy = RandomFloat(-amount, amount);
        g_state.batch.SetView(g_state.cam_x + g_state.shake_dx,
                              g_state.cam_y + g_state.shake_dy, g_state.cam_zoom);
    } else {
        g_state.shake_dx = g_state.shake_dy = 0.0f;
    }

    g_state.frame_open = true;
    return true;
}

/** 終了を要求する (次の NextFrame が false を返す)。 */
void Quit() noexcept { g_state.quit_req = true; }

/** ウィンドウタイトルを差し替える。 */
void SetWindowTitle(const char* title) noexcept {
    if (!g_state.booted) return;
    ToWide(title ? title : "", g_state.title_buf, 256);
    g_state.window.SetTitle(g_state.title_buf);
}

/** 背景クリア色を設定する。 */
void SetBackground(FColor color) noexcept {
    g_state.clear = FClearColor{ color.r, color.g, color.b, color.a };
}

/** 全画面切替を要求する (フレーム境界で適用)。 */
void SetFullscreen(bool on) noexcept {
    if (g_state.booted) g_state.fs_request = on ? 1 : 0;
}

/** 全画面と窓表示を切り替えるよう要求する。 */
void ToggleFullscreen() noexcept {
    if (g_state.booted)
        g_state.fs_request = g_state.window.IsFullscreen() ? 0 : 1;
}

/** 現在全画面表示かを返す。 */
bool IsFullscreen() noexcept {
    return g_state.booted && g_state.window.IsFullscreen();
}

/** コンソール窓の表示/非表示を切り替える (GUI ビルドでは何もしない)。 */
void ShowConsole(bool show) noexcept {
    HWND con = ::GetConsoleWindow();
    if (con) ::ShowWindow(con, show ? SW_SHOW : SW_HIDE);
}

/** コンソール窓の表示/非表示を反転する。 */
void ToggleConsole() noexcept {
    ShowConsole(!IsConsoleVisible());
}

/** コンソール窓が現在表示されているかを返す。 */
bool IsConsoleVisible() noexcept {
    HWND con = ::GetConsoleWindow();
    return con && ::IsWindowVisible(con);
}

/** ポストプロセスが利用可能かを返す。 */
bool IsPostProcessAvailable() noexcept { return g_state.post_available; }

/** Bloom の強さを設定する。 */
void SetBloom(f32 intensity) noexcept {
    g_state.pp_params.bloom_enabled   = (intensity > 0.0001f);
    g_state.pp_params.bloom_intensity = intensity < 0.0f ? 0.0f : intensity;
}

/** Bloom が発光させる明るさの閾値を設定する。 */
void SetBloomThreshold(f32 threshold) noexcept {
    g_state.pp_params.bloom_threshold = threshold < 0.0f ? 0.0f : threshold;
}

/** 露出 (画面全体の明るさ) を設定する。 */
void SetExposure(f32 exposure) noexcept {
    g_state.pp_params.exposure = exposure < 0.0f ? 0.0f : exposure;
}

/** ビネット (画面端の暗化) の強さを設定する。 */
void SetVignette(f32 intensity) noexcept {
    g_state.pp_params.vignette_intensity = Clamp01(intensity);
}

/** 色収差の量を設定する。 */
void SetChromaticAberration(f32 amount) noexcept {
    g_state.pp_params.chromatic_aberration = amount < 0.0f ? 0.0f : amount;
}

/** フィルムグレインの強さを設定する。 */
void SetFilmGrain(f32 intensity) noexcept {
    g_state.pp_params.grain_intensity = intensity < 0.0f ? 0.0f : intensity;
}

/** カラーグレーディング (彩度・コントラスト・色温度) を設定する。 */
void SetColorGrading(f32 saturation, f32 contrast, f32 temperature) noexcept {
    g_state.pp_params.cg_saturation  = saturation < 0.0f ? 0.0f : saturation;
    g_state.pp_params.cg_contrast    = contrast   < 0.0f ? 0.0f : contrast;
    g_state.pp_params.cg_temperature = temperature < -1.0f ? -1.0f
                                   : (temperature > 1.0f ? 1.0f : temperature);
}

/** 輪郭強調 (シャープネス) の強さを設定する。 */
void SetSharpness(f32 strength) noexcept {
    g_state.pp_params.cas_strength = Clamp01(strength);
}

/** トーンマップの方式を設定する (0=ACES、1=AgX、2=Reinhard)。 */
void SetTonemap(i32 mode) noexcept {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_state.pp_params.tonemap_kind = mode;
}

/** 自動露出の ON/OFF を設定する。 */
void SetAutoExposure(bool enabled) noexcept {
    g_state.pp_params.auto_exposure_enabled = enabled;
}

/** 塗りつぶし長方形を描く。 */
void DrawRect(f32 x, f32 y, f32 width, f32 height, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawRect(x, y, width, height, ToVec4(color));
}

/** 長方形の枠線を 4 辺の細い矩形で描く。 */
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

/** 回転した塗りつぶし長方形を描く (回転は中心まわり)。 */
void DrawRectRotated(f32 x, f32 y, f32 width, f32 height,
                     f32 degrees, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    // 左上指定を中心へ変換して FSpriteBatch に渡す
    g_state.batch.DrawRectRotated(x + width * 0.5f, y + height * 0.5f,
                                 width, height, degrees * kDeg2Rad, ToVec4(color));
}

/** 塗りつぶし円を円テクスチャで描く ((x,y) は中心)。 */
void DrawCircle(f32 x, f32 y, f32 radius, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.circle_tex || radius <= 0.0f) return;
    g_state.batch.Draw(*g_state.circle_tex, x - radius, y - radius,
                      radius * 2.0f, radius * 2.0f, ToVec4(color));
}

/** 円の枠線を円周に沿った小さな点列で描く ((x,y) は中心)。 */
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

/** 2 点を結ぶ線分を、線の向きへ回転させた細長い矩形で描く。 */
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

/** 塗りつぶし三角形を描く。 */
void DrawTriangle(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                  FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawTriangle(x1, y1, x2, y2, x3, y3, ToVec4(color));
}

/** 三角形の枠線を 3 辺の線分で描く。 */
void DrawTriangleOutline(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
                         FColor color, f32 thickness) noexcept {
    DrawLine(x1, y1, x2, y2, color, thickness);
    DrawLine(x2, y2, x3, y3, color, thickness);
    DrawLine(x3, y3, x1, y1, color, thickness);
}

/** 1 ピクセルの点を 1x1 の矩形で描く。 */
void DrawPixel(f32 x, f32 y, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.DrawRect(x, y, 1.0f, 1.0f, ToVec4(color));
}

/** スプライトを元サイズで描く。 */
void DrawSprite(FSprite sprite, f32 x, f32 y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, s.w, s.h, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

/** スプライトを指定サイズに伸縮して描く。 */
void DrawSprite(FSprite sprite, f32 x, f32 y, f32 width, f32 height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, width, height, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

/** スプライトを回転 (+拡縮・色掛け) して描く (回転は画像の中心まわり)。 */
void DrawSpriteRotated(FSprite sprite, f32 x, f32 y, f32 degrees,
                       f32 scale, FColor tint) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex) return;
    const f32 w = s.w * scale;
    const f32 h = s.h * scale;
    // 左上指定を中心へ変換。回転は画像の中心まわり。
    g_state.batch.DrawRotated(*s.tex, x + w * 0.5f, y + h * 0.5f, w, h,
                             degrees * kDeg2Rad, 0, 0, 1, 1, ToVec4(tint));
}

/** スプライトに色を掛けて元サイズで描く。 */
void DrawSpriteTinted(FSprite sprite, f32 x, f32 y, FColor tint) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (s.tex)
        g_state.batch.Draw(*s.tex, x, y, s.w, s.h, ToVec4(tint));
}

/** スプライトを UV を入れ替えて左右・上下反転して描く。 */
void DrawSpriteFlipped(FSprite sprite, f32 x, f32 y,
                       bool flip_x, bool flip_y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex) return;
    const f32 u0 = flip_x ? 1.0f : 0.0f, u1 = flip_x ? 0.0f : 1.0f;
    const f32 v0 = flip_y ? 1.0f : 0.0f, v1 = flip_y ? 0.0f : 1.0f;
    g_state.batch.DrawSub(*s.tex, x, y, s.w, s.h, u0, v0, u1, v1,
                         FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

/** スプライトの一部分を UV 範囲で切り出して描く (スプライトシート用)。 */
void DrawSpritePart(FSprite sprite, f32 x, f32 y, f32 width, f32 height,
                    f32 src_x, f32 src_y, f32 src_width, f32 src_height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return;
    FSpriteSlot& s = g_state.sprites[sprite.id - 1];
    if (!s.tex || s.w <= 0.0f || s.h <= 0.0f) return;
    const f32 u0 = src_x / s.w,                v0 = src_y / s.h;
    const f32 u1 = (src_x + src_width)  / s.w,  v1 = (src_y + src_height) / s.h;
    g_state.batch.DrawSub(*s.tex, x, y, width, height, u0, v0, u1, v1,
                         FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
}

/** スプライトの元画像の幅を返す。 */
f32 SpriteWidth(FSprite sprite) noexcept {
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return 0.0f;
    return g_state.sprites[sprite.id - 1].w;
}

/** スプライトの元画像の高さを返す。 */
f32 SpriteHeight(FSprite sprite) noexcept {
    if (sprite.id == 0 || sprite.id > g_state.sprites.Size()) return 0.0f;
    return g_state.sprites[sprite.id - 1].h;
}

/** 文字列を既定サイズで描く。 */
void DrawString(f32 x, f32 y, const char* text, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text) return;
    DrawTextScaled(x, y, text, ToVec4(color), 1.0f);
}

/** 文字列を指定ピクセルサイズで描く。 */
void DrawString(f32 x, f32 y, const char* text, FColor color, f32 size) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text || size <= 0.0f) return;
    const f32 base = g_state.font.PixelSize();
    DrawTextScaled(x, y, text, ToVec4(color), base > 0.0f ? size / base : 1.0f);
}

/** 文字列を center_x で中央そろえして既定サイズで描く。 */
void DrawStringCentered(f32 center_x, f32 y, const char* text, FColor color) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text) return;
    const f32 w = g_state.font.MeasureWidth(text);
    DrawTextScaled(center_x - w * 0.5f, y, text, ToVec4(color), 1.0f);
}

/** 文字列を center_x で中央そろえして指定サイズで描く。 */
void DrawStringCentered(f32 center_x, f32 y, const char* text,
                        FColor color, f32 size) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    if (!g_state.font_ok || !text || size <= 0.0f) return;
    const f32 base  = g_state.font.PixelSize();
    const f32 scale = base > 0.0f ? size / base : 1.0f;
    const f32 w     = g_state.font.MeasureWidth(text) * scale;
    DrawTextScaled(center_x - w * 0.5f, y, text, ToVec4(color), scale);
}

/** 既定サイズで描いたときの文字列の幅を返す。 */
f32 TextWidth(const char* text) noexcept {
    if (!g_state.font_ok || !text) return 0.0f;
    return g_state.font.MeasureWidth(text);
}

/** 指定サイズで描いたときの文字列の幅を返す。 */
f32 TextWidth(const char* text, f32 size) noexcept {
    if (!g_state.font_ok || !text) return 0.0f;
    const f32 base = g_state.font.PixelSize();
    return g_state.font.MeasureWidth(text) * (base > 0.0f ? size / base : 1.0f);
}

/** 既定サイズの 1 行の高さを返す。 */
f32 TextHeight() noexcept {
    return g_state.font_ok ? g_state.font.LineHeight() : 0.0f;
}

/** 指定サイズの 1 行の高さを返す。 */
f32 TextHeight(f32 size) noexcept {
    if (!g_state.font_ok) return 0.0f;
    const f32 base = g_state.font.PixelSize();
    return g_state.font.LineHeight() * (base > 0.0f ? size / base : 1.0f);
}

/** カメラ中心を (x,y) に置き、view を更新する。 */
void SetCamera(f32 x, f32 y) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_x = x;
    g_state.cam_y = y;
    g_state.batch.SetView(g_state.cam_x + g_state.shake_dx, g_state.cam_y + g_state.shake_dy, g_state.cam_zoom);
}

/** カメラのズーム倍率を設定し、view を更新する (下限 0.01)。 */
void SetCameraZoom(f32 zoom) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_zoom = (zoom > 0.01f) ? zoom : 0.01f;
    g_state.batch.SetView(g_state.cam_x + g_state.shake_dx, g_state.cam_y + g_state.shake_dy, g_state.cam_zoom);
}

/** カメラを画面中央・ズーム 1.0 にリセットし、view を更新する。 */
void ResetCamera() noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.cam_x    = ScreenWidth()  * 0.5f;
    g_state.cam_y    = ScreenHeight() * 0.5f;
    g_state.cam_zoom = 1.0f;
    g_state.batch.SetView(g_state.cam_x + g_state.shake_dx, g_state.cam_y + g_state.shake_dy, g_state.cam_zoom);
}

/** 現在のカメラ中心 X 座標を返す。 */
f32 CameraX() noexcept { return g_state.cam_x; }

/** 現在のカメラ中心 Y 座標を返す。 */
f32 CameraY() noexcept { return g_state.cam_y; }

/** マウスの X 座標をワールド座標に変換して返す。 */
f32 MouseWorldX() noexcept {
    return (MouseX() - ScreenWidth() * 0.5f) / g_state.cam_zoom + g_state.cam_x;
}

/** マウスの Y 座標をワールド座標に変換して返す。 */
f32 MouseWorldY() noexcept {
    return (MouseY() - ScreenHeight() * 0.5f) / g_state.cam_zoom + g_state.cam_y;
}

/** 描画を矩形内に制限するクリップを設定する (画面座標)。 */
void SetClipRect(f32 x, f32 y, f32 width, f32 height) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.SetClipRect(static_cast<i32>(x), static_cast<i32>(y),
                             static_cast<i32>(width), static_cast<i32>(height));
}

/** クリップを解除して画面全体に描けるようにする。 */
void ClearClipRect() noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return; }
    g_state.batch.ClearClipRect();
}

/** 紙が燃える per-pixel ディゾルブを矩形に重ねる。描けたら true (未対応なら false)。 */
bool DrawBurnDissolve(f32 x, f32 y, f32 w, f32 h, f32 progress,
                      FColor ember, FColor paper, f32 edge, f32 freq, f32 time, f32 cells) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return false; }
    if (!g_state.burn.Ready()) return false;
    IRhiCommandList* cl = g_state.renderer.CommandList();
    if (!cl) return false;
    g_state.batch.FlushPending();                 // 保留スプライトを先に確定
    FBurnParams bp;
    bp.progress = progress; bp.edge = edge; bp.freq = freq; bp.time = time; bp.cells = cells;
    bp.ember = FVec3{ ember.r, ember.g, ember.b };
    bp.paper = FVec3{ paper.r, paper.g, paper.b };
    g_state.burn.Draw(*cl, x, y, w, h,
                      static_cast<f32>(g_state.window.Width()),
                      static_cast<f32>(g_state.window.Height()), bp);
    g_state.batch.Rebind();                        // バッチの bind を貼り直す
    return true;
}

/** 画像を読み込み GPU に転送してスプライトを返す (同一パスはキャッシュ)。 */
FSprite LoadSprite(const char* path) noexcept {
    if (!g_state.booted) {
        ACS_LOG_WARN("easy: LoadSprite() は OpenWindow() の後で呼んでください");
        return FSprite{ 0 };
    }
    if (!path) return FSprite{ 0 };
    for (usize i = 0; i < g_state.sprites.Size(); ++i) {
        const char* p = g_state.sprites[i].path.Data();
        if (p && strcmp(p, path) == 0)
            return FSprite{ static_cast<u32>(i + 1) };
    }
    wchar_t wpath[512];
    ToWide(path, wpath, 512);
    auto r = g_state.assets.Load(wpath);
    if (r.IsErr()) {
        ACS_LOG_ERROR("easy: 画像を読み込めません '%s': %s", path, r.Error().message);
        return FSprite{ 0 };
    }
    const TSharedPtr<FAsset> asset = r.Value();
    FAsset* base = asset.Get();
    if (!base || base->Type() != FImageAsset::StaticType()) {
        ACS_LOG_ERROR("easy: '%s' は画像ファイルではありません", path);
        return FSprite{ 0 };
    }
    FImageAsset* img = static_cast<FImageAsset*>(base);
    auto tx = UploadTexture(*g_state.renderer.Device(), *img);
    if (tx.IsErr()) {
        ACS_LOG_ERROR("easy: 画像を GPU に転送できません '%s': %s", path, tx.Error().message);
        return FSprite{ 0 };
    }
    FSpriteSlot slot;
    slot.tex  = Move(tx.Value());
    slot.path = FString{path, g_easy_allocator};
    slot.w    = static_cast<f32>(img->Width());
    slot.h    = static_cast<f32>(img->Height());
    g_state.sprites.PushBack(Move(slot));
    return FSprite{ static_cast<u32>(g_state.sprites.Size()) };
}

/** 音声を読み込みアセットを保持してサウンドを返す (同一パスはキャッシュ)。 */
FSound LoadSound(const char* path) noexcept {
    if (!g_state.booted) {
        ACS_LOG_WARN("easy: LoadSound() は OpenWindow() の後で呼んでください");
        return FSound{ 0 };
    }
    if (!path) return FSound{ 0 };
    for (usize i = 0; i < g_state.sounds.Size(); ++i) {
        const char* p = g_state.sounds[i].path.Data();
        if (p && strcmp(p, path) == 0)
            return FSound{ static_cast<u32>(i + 1) };
    }
    wchar_t wpath[512];
    ToWide(path, wpath, 512);
    auto r = g_state.assets.Load(wpath);
    if (r.IsErr()) {
        ACS_LOG_ERROR("easy: 音声を読み込めません '%s': %s", path, r.Error().message);
        return FSound{ 0 };
    }
    const TSharedPtr<FAsset> asset = r.Value();
    FAsset* base = asset.Get();
    if (!base || base->Type() != FAudioAsset::StaticType()) {
        ACS_LOG_ERROR("easy: '%s' は音声ファイルではありません", path);
        return FSound{ 0 };
    }
    FSoundSlot slot;
    slot.asset = asset;            // TSharedPtr をコピー保持 -> 再生中ずっと生かす
    slot.path = FString{path, g_easy_allocator};
    g_state.sounds.PushBack(Move(slot));
    return FSound{ static_cast<u32>(g_state.sounds.Size()) };
}

/** 効果音を既定音量で 1 回再生する。 */
void Play(FSound sound) noexcept { Play(sound, 1.0f); }

/** 効果音を音量指定で 1 回再生する。 */
void Play(FSound sound, f32 volume) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    FAsset* base = g_state.sounds[sound.id - 1].asset.Get();
    if (base)
        g_state.audio.Play(*static_cast<FAudioAsset*>(base), Clamp01(volume), false);
}

/** 音を既定音量でループ再生する。 */
void PlayLoop(FSound sound) noexcept { PlayLoop(sound, 1.0f); }

/** 音を音量指定でループ再生する (既存ループは二重再生防止に止める)。 */
void PlayLoop(FSound sound, f32 volume) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    FSoundSlot& slot = g_state.sounds[sound.id - 1];
    FAsset* base = slot.asset.Get();
    if (!base) return;
    if (slot.loop.IsValid()) g_state.audio.Stop(slot.loop);   // 二重ループを防ぐ
    slot.loop = g_state.audio.Play(*static_cast<FAudioAsset*>(base),
                                  Clamp01(volume), true);
}

/** そのサウンドのループ再生を止める。 */
void StopSound(FSound sound) noexcept {
    if (!g_state.audio_ok) return;
    if (sound.id == 0 || sound.id > g_state.sounds.Size()) return;
    FSoundSlot& slot = g_state.sounds[sound.id - 1];
    if (slot.loop.IsValid()) {
        g_state.audio.Stop(slot.loop);
        slot.loop = FSoundHandle{};
    }
}

/** 鳴っている音をすべて止め、各スロットのループハンドルを無効化する。 */
void StopAllSounds() noexcept {
    if (!g_state.audio_ok) return;
    g_state.audio.StopAll();
    for (usize i = 0; i < g_state.sounds.Size(); ++i)
        g_state.sounds[i].loop = FSoundHandle{};
}

/** マスター音量を設定する。 */
void SetMasterVolume(f32 volume) noexcept {
    if (g_state.audio_ok) g_state.audio.SetMasterVolume(Clamp01(volume));
}

/** 鳴っている音をすべて一時停止する。 */
void PauseAllSounds() noexcept {
    if (g_state.audio_ok) g_state.audio.PauseAll();
}

/** 一時停止した音をすべて再開する。 */
void ResumeAllSounds() noexcept {
    if (g_state.audio_ok) g_state.audio.ResumeAll();
}

/** キーが押されている間ずっと true を返す。 */
bool IsKeyDown    (EKey key) noexcept { return FInput::IsKeyDown(key); }

/** キーを押した瞬間のフレームだけ true を返す。 */
bool IsKeyPressed (EKey key) noexcept { return FInput::IsKeyPressed(key); }

/** キーを離した瞬間のフレームだけ true を返す。 */
bool IsKeyReleased(EKey key) noexcept { return FInput::IsKeyReleased(key); }

/** このフレームに入力された文字 (UTF-8、IME 確定後) を返す。 */
const char* TextInput() noexcept { return FInput::TextInput(); }

/** マウスの X 座標 (画面座標) を返す。 */
f32  MouseX() noexcept { return FInput::MousePos().x; }

/** マウスの Y 座標 (画面座標) を返す。 */
f32  MouseY() noexcept { return FInput::MousePos().y; }

/** マウスボタンが押されている間ずっと true を返す。 */
bool IsMouseDown    (EMouseButton button) noexcept { return FInput::IsMouseButtonDown(button); }

/** マウスボタンを押した瞬間のフレームだけ true を返す。 */
bool IsMousePressed (EMouseButton button) noexcept { return FInput::IsMouseButtonPressed(button); }

/** マウスボタンを離した瞬間のフレームだけ true を返す。 */
bool IsMouseReleased(EMouseButton button) noexcept { return FInput::IsMouseButtonReleased(button); }

/** マウスホイールの回転量を返す。 */
f32  MouseWheel() noexcept { return FInput::MouseWheel(); }

/** ゲームパッドが接続されているかを返す。 */
bool IsGamepadConnected(i32 player) noexcept {
    return FInput::IsGamepadConnected(GpIdx(player));
}

/** ゲームパッドのボタンが押されている間ずっと true を返す。 */
bool IsGamepadDown(EGamepadButton button, i32 player) noexcept {
    return FInput::IsGamepadButtonDown(GpIdx(player), button);
}

/** ゲームパッドのボタンを押した瞬間のフレームだけ true を返す。 */
bool IsGamepadPressed(EGamepadButton button, i32 player) noexcept {
    return FInput::IsGamepadButtonPressed(GpIdx(player), button);
}

/** 左スティックの X 軸値 (-1.0〜+1.0) を返す。 */
f32 GamepadLeftX(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftX);
}

/** 左スティックの Y 軸値 (-1.0〜+1.0) を返す。 */
f32 GamepadLeftY(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftY);
}

/** 右スティックの X 軸値 (-1.0〜+1.0) を返す。 */
f32 GamepadRightX(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightX);
}

/** 右スティックの Y 軸値 (-1.0〜+1.0) を返す。 */
f32 GamepadRightY(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightY);
}

/** 左トリガーの押し込み量 (0.0〜1.0) を返す。 */
f32 GamepadLeftTrigger(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::LeftTrigger);
}

/** 右トリガーの押し込み量 (0.0〜1.0) を返す。 */
f32 GamepadRightTrigger(i32 player) noexcept {
    return FInput::GamepadAxisValue(GpIdx(player), EGamepadAxis::RightTrigger);
}

/** min 以上 max 以下の整数乱数を一様に返す (剰余バイアスを棄却)。 */
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

/** min 以上 max 未満の小数乱数を返す。 */
f32 RandomFloat(f32 min, f32 max) noexcept {
    // 24bit 分の乱数を [0,1) に。float の仮数部に収まるので max ちょうどは出ない。
    const f32 t = static_cast<f32>(NextRng() >> 8) / 16777216.0f;
    return min + (max - min) * t;
}

/** true / false をランダムに返す。 */
bool RandomBool() noexcept { return (NextRng() & 1u) != 0u; }

/** 乱数の種を固定する (0 は既定値に置き換える)。 */
void RandomSeed(u32 seed) noexcept {
    g_rng = seed ? seed : 0x9E3779B9u;
}

/** 2 つの矩形が重なっているかを返す。 */
bool RectsOverlap(f32 x1, f32 y1, f32 w1, f32 h1,
                  f32 x2, f32 y2, f32 w2, f32 h2) noexcept {
    return x1 < x2 + w2 && x2 < x1 + w1 &&
           y1 < y2 + h2 && y2 < y1 + h1;
}

/** 2 つの円が重なっているかを返す。 */
bool CirclesOverlap(f32 x1, f32 y1, f32 r1,
                    f32 x2, f32 y2, f32 r2) noexcept {
    const f32 dx = x2 - x1, dy = y2 - y1, rr = r1 + r2;
    return dx * dx + dy * dy < rr * rr;
}

/** 点が矩形の内側にあるかを返す。 */
bool PointInRect(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) noexcept {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/** 点が円の内側にあるかを返す。 */
bool PointInCircle(f32 px, f32 py, f32 cx, f32 cy, f32 r) noexcept {
    const f32 dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy < r * r;
}

/** 値を [lo, hi] の範囲に収める。 */
f32 Clamp(f32 value, f32 lo, f32 hi) noexcept {
    return value < lo ? lo : (value > hi ? hi : value);
}

/** a と b を t で線形補間する。 */
f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

/** 2 点間のユークリッド距離を返す。 */
f32 Distance(f32 x1, f32 y1, f32 x2, f32 y2) noexcept {
    const f32 dx = x2 - x1, dy = y2 - y1;
    return Sqrt(dx * dx + dy * dy);
}

/** 2 値の小さいほうを返す。 */
f32 Min(f32 a, f32 b) noexcept { return a < b ? a : b; }

/** 2 値の大きいほうを返す。 */
f32 Max(f32 a, f32 b) noexcept { return a > b ? a : b; }

/** 絶対値を返す。 */
f32 Abs(f32 value)    noexcept { return value < 0.0f ? -value : value; }

/** 角度 (度) の正弦を返す。 */
f32 Sin(f32 degrees) noexcept { return acs::Sin(degrees * kDeg2Rad); }

/** 角度 (度) の余弦を返す。 */
f32 Cos(f32 degrees) noexcept { return acs::Cos(degrees * kDeg2Rad); }

/** 平方根を返す。 */
f32 Sqrt(f32 value)  noexcept { return acs::Sqrt(value); }

/** (x1,y1) から (x2,y2) へ向かう向きの角度 (度) を返す。 */
f32 AngleTo(f32 x1, f32 y1, f32 x2, f32 y2) noexcept {
    return ATan2(y2 - y1, x2 - x1) * kRad2Deg;
}

/** 小数点以下を切り捨てる。 */
f32 Floor(f32 v) noexcept { return acs::Floor(v); }

/** 小数点以下を切り上げる。 */
f32 Ceil (f32 v) noexcept { return acs::Ceil(v); }

/** 四捨五入する。 */
f32 Round(f32 v) noexcept { return acs::Round(v); }

/** 符号を返す (正で +1、負で -1、0 で 0)。 */
f32 Sign (f32 v) noexcept { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

/** 累乗 (base の exponent 乗) を返す。 */
f32 Pow  (f32 base, f32 exponent) noexcept { return acs::Pow(base, exponent); }

/** 角度 (度) の正接を返す。 */
f32 Tan  (f32 degrees) noexcept { return acs::Sin(degrees * kDeg2Rad) / acs::Cos(degrees * kDeg2Rad); }

/** (x,y) 方向の角度 (度) を返す。 */
f32 Atan2(f32 y, f32 x) noexcept { return ATan2(y, x) * kRad2Deg; }

/** current から target へ max_delta だけ近づけた値を返す (行き過ぎない)。 */
f32 MoveTowards(f32 current, f32 target, f32 max_delta) noexcept {
    const f32 d = target - current;
    if (Abs(d) <= max_delta) return target;
    return current + (d > 0.0f ? max_delta : -max_delta);
}

/** 値を [min, max) の範囲に巻き込む (はみ出したら反対側へ)。 */
f32 Wrap(f32 value, f32 min, f32 max) noexcept {
    const f32 range = max - min;
    if (range <= 0.0f) return min;
    const f32 t = value - min - acs::Floor((value - min) / range) * range;
    return min + t;
}

/** 整数値を [min, max) の範囲に巻き込む。 */
i32 WrapInt(i32 value, i32 min, i32 max) noexcept {
    const i32 range = max - min;
    if (range <= 0) return min;
    i32 t = (value - min) % range;
    if (t < 0) t += range;
    return min + t;
}

/** [0, length) の範囲を繰り返す値を返す。 */
f32 Repeat(f32 t, f32 length) noexcept {
    if (length <= 0.0f) return 0.0f;
    return t - acs::Floor(t / length) * length;
}

/** 0 と length の間を往復する値を返す。 */
f32 PingPong(f32 t, f32 length) noexcept {
    if (length <= 0.0f) return 0.0f;
    const f32 r = Repeat(t, length * 2.0f);
    return length - Abs(r - length);
}

/** a→b をなめらかに補間する (両端で速度 0)。 */
f32 SmoothStep(f32 a, f32 b, f32 t) noexcept {
    const f32 x = Clamp01(t);
    return a + (b - a) * (x * x * (3.0f - 2.0f * x));
}

/** 角度 (度) を最短回りで補間する。 */
f32 LerpAngle(f32 a_deg, f32 b_deg, f32 t) noexcept {
    f32 d = b_deg - a_deg;
    d -= acs::Floor((d + 180.0f) / 360.0f) * 360.0f;
    return a_deg + d * t;
}

/** 2 点間の距離の 2 乗を返す (比較用、平方根を取らない)。 */
f32 DistanceSquared(f32 x1, f32 y1, f32 x2, f32 y2) noexcept {
    const f32 dx = x2 - x1, dy = y2 - y1;
    return dx * dx + dy * dy;
}

/** 原点から (x,y) までの距離 (ベクトルの長さ) を返す。 */
f32 Length(f32 x, f32 y) noexcept { return acs::Sqrt(x * x + y * y); }

/** 2 色を t で線形補間する。 */
FColor LerpColor(FColor a, FColor b, f32 t) noexcept {
    return FColor{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                   a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

/** HSV から RGB の不透明色を作る。 */
FColor Hsv(f32 hue_degrees, f32 saturation, f32 value) noexcept {
    const f32 h = Repeat(hue_degrees, 360.0f) / 60.0f;
    const f32 c = Clamp01(value) * Clamp01(saturation);
    const f32 x = c * (1.0f - Abs(Repeat(h, 2.0f) - 1.0f));
    const f32 m = Clamp01(value) - c;
    f32 r = 0.0f, g = 0.0f, b = 0.0f;
    switch (static_cast<i32>(h)) {
        case 0:  r = c; g = x; break;
        case 1:  r = x; g = c; break;
        case 2:  g = c; b = x; break;
        case 3:  g = x; b = c; break;
        case 4:  r = x; b = c; break;
        default: r = c; b = x; break;
    }
    return FColor{ r + m, g + m, b + m, 1.0f };
}

/** 色を白へ近づけて明るくする (不透明度は維持)。 */
FColor Brighten(FColor c, f32 amount) noexcept {
    const f32 t = Clamp01(amount);
    return FColor{ c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t, c.b + (1.0f - c.b) * t, c.a };
}

/** 色を黒へ近づけて暗くする (不透明度は維持)。 */
FColor Darken(FColor c, f32 amount) noexcept {
    const f32 t = 1.0f - Clamp01(amount);
    return FColor{ c.r * t, c.g * t, c.b * t, c.a };
}

/** 鮮やかな色をランダムに返す。 */
FColor RandomColor() noexcept { return Hsv(RandomFloat(0.0f, 360.0f), 0.75f, 0.95f); }

/** 左右の移動入力 (キー + 左スティック X) を合成して返す。 */
f32 MoveX() noexcept {
    f32 v = 0.0f;
    if (IsKeyDown(EKey::Left)  || IsKeyDown(EKey::A)) v -= 1.0f;
    if (IsKeyDown(EKey::Right) || IsKeyDown(EKey::D)) v += 1.0f;
    v += GamepadLeftX(0);
    return Clamp(v, -1.0f, 1.0f);
}

/** 上下の移動入力 (キー + 左スティック Y) を合成して返す (Y-down)。 */
f32 MoveY() noexcept {
    f32 v = 0.0f;
    if (IsKeyDown(EKey::Up)   || IsKeyDown(EKey::W)) v -= 1.0f;
    if (IsKeyDown(EKey::Down) || IsKeyDown(EKey::S)) v += 1.0f;
    v -= GamepadLeftY(0);   // スティックは上が +、画面は Y-down なので反転
    return Clamp(v, -1.0f, 1.0f);
}

/** このフレームに何かキーが押されたかを返す。 */
bool IsAnyKeyPressed() noexcept {
    for (u16 k = 0; k < static_cast<u16>(EKey::_Count); ++k)
        if (IsKeyPressed(static_cast<EKey>(k))) return true;
    return false;
}

/** 塗りつぶし長方形を中心基準で描く。 */
void DrawRectCentered(f32 cx, f32 cy, f32 width, f32 height, FColor color) noexcept {
    DrawRect(cx - width * 0.5f, cy - height * 0.5f, width, height, color);
}

/** スプライトを中心基準で回転・拡縮・色掛けして描く。 */
void DrawSpriteCentered(FSprite sprite, f32 cx, f32 cy, f32 scale, f32 degrees, FColor tint) noexcept {
    const f32 w = SpriteWidth(sprite)  * scale;
    const f32 h = SpriteHeight(sprite) * scale;
    DrawSpriteRotated(sprite, cx - w * 0.5f, cy - h * 0.5f, degrees, scale, tint);
}

/** 円と矩形が重なっているかを返す。 */
bool CircleVsRect(f32 cx, f32 cy, f32 r, f32 rx, f32 ry, f32 rw, f32 rh) noexcept {
    const f32 nx = Clamp(cx, rx, rx + rw);
    const f32 ny = Clamp(cy, ry, ry + rh);
    const f32 dx = cx - nx, dy = cy - ny;
    return dx * dx + dy * dy <= r * r;
}

/** 2 つの線分が交差しているかを返す。 */
bool SegmentsIntersect(f32 ax, f32 ay, f32 bx, f32 by,
                       f32 cx, f32 cy, f32 dx, f32 dy) noexcept {
    const auto cross = [](f32 ox, f32 oy, f32 px, f32 py, f32 qx, f32 qy) noexcept {
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
    };
    const f32 d1 = cross(cx, cy, dx, dy, ax, ay);
    const f32 d2 = cross(cx, cy, dx, dy, bx, by);
    const f32 d3 = cross(ax, ay, bx, by, cx, cy);
    const f32 d4 = cross(ax, ay, bx, by, dx, dy);
    return ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f));
}

/** 点が三角形の内側 (辺上を含む) にあるかを返す。 */
bool PointInTriangle(f32 px, f32 py, f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3) noexcept {
    const f32 d1 = (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2);
    const f32 d2 = (px - x3) * (y2 - y3) - (x2 - x3) * (py - y3);
    const f32 d3 = (px - x1) * (y3 - y1) - (x3 - x1) * (py - y1);
    const bool has_neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    const bool has_pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(has_neg && has_pos);
}

/** 矩形を、ふさがった矩形の外へ食い込みの浅い軸方向に最小距離で押し戻す。 */
bool ResolveRect(f32* x, f32* y, f32 w, f32 h, f32 sx, f32 sy, f32 sw, f32 sh) noexcept {
    if (!x || !y) return false;
    const f32 ox = Min(*x + w, sx + sw) - Max(*x, sx);
    const f32 oy = Min(*y + h, sy + sh) - Max(*y, sy);
    if (ox <= 0.0f || oy <= 0.0f) return false;
    const f32 mcx = *x + w * 0.5f, scx = sx + sw * 0.5f;
    const f32 mcy = *y + h * 0.5f, scy = sy + sh * 0.5f;
    if (ox < oy) *x += (mcx < scx) ? -ox : ox;
    else         *y += (mcy < scy) ? -oy : oy;
    return true;
}

/** 時間を貯めて seconds ごとに 1 度だけ true を返す (周期実行)。 */
bool Every(f32 seconds, f32* accumulator) noexcept {
    if (!accumulator || seconds <= 0.0f) return false;
    *accumulator += g_state.dt;
    if (*accumulator >= seconds) { *accumulator -= seconds; return true; }
    return false;
}

/** クールダウンを処理し、再装填できた (=行動できる) フレームなら true を返す。 */
bool Cooldown(f32* timer, f32 seconds) noexcept {
    if (!timer) return false;
    if (*timer <= 0.0f) { *timer = seconds; return true; }
    *timer -= g_state.dt;
    return false;
}

/** 手動タイマーを DeltaTime ぶん 0 に向けて減らし、残り秒を返す。 */
f32 Countdown(f32* timer) noexcept {
    if (!timer) return 0.0f;
    *timer -= g_state.dt;
    if (*timer < 0.0f) *timer = 0.0f;
    return *timer;
}

/** 画面シェイクのトラウマ値を加算する (1 で飽和)。 */
void ScreenShake(f32 strength) noexcept {
    g_state.shake_trauma = Min(1.0f, g_state.shake_trauma + Max(0.0f, strength));
}

/** 全画面フラッシュ演出を開始する。 */
void ScreenFlash(FColor color, f32 seconds) noexcept {
    g_state.flash_color = color;
    g_state.flash_dur   = Max(0.0f, seconds);
    g_state.flash_timer = g_state.flash_dur;
}

/** 型付き catalog から任意の easing を評価する。 */
f32 Ease(f32 t, EEasingType type, f32 fallback) noexcept {
    return acs::game::Easing::Evaluate(type, t, fallback);
}

/** 型付き easing を checked 評価する。 */
FEasingResult TryEase(
    f32 t, EEasingType type, f32& out_value) noexcept {
    return acs::game::Easing::TryEvaluate(type, t, out_value);
}

// 型付き easing を [0,1] 上で等間隔に一括サンプリングする。
FEasingResult TrySampleEasing(
    EEasingType type, f32* out_values, usize sample_count) noexcept {
    return acs::game::Easing::TrySampleCurve(
        type, out_values, sample_count);
}

/** easing type の canonical 名を返す。 */
const char* EasingName(EEasingType type) noexcept {
    return acs::game::Easing::GetName(type);
}

/** easing type の canonical 名を checked 取得する。 */
FEasingResult TryGetEasingName(
    EEasingType type, const char*& out_name) noexcept {
    return acs::game::Easing::TryGetName(type, out_name);
}

/** canonical easing 名を型付き catalog 値へ変換する。 */
bool TryParseEasingName(
    const char* name, EEasingType& out_type) noexcept {
    return acs::game::Easing::TryParseName(name, out_type);
}

/** canonical easing 名を型付き catalog 値へ checked 変換する。 */
FEasingResult TryParseEasingNameChecked(
    const char* name, EEasingType& out_type) noexcept {
    return acs::game::Easing::TryParseNameChecked(name, out_type);
}

/** 加速するイージング。 */
f32 EaseIn(f32 t) noexcept {
    return acs::game::Easing::InQuad(t);
}

/** 減速するイージング。 */
f32 EaseOut(f32 t) noexcept {
    return acs::game::Easing::OutQuad(t);
}

/** 加速→減速するイージング。 */
f32 EaseInOut(f32 t) noexcept {
    return acs::game::Easing::InOutQuad(t);
}

/** 行き過ぎて戻るイージング。 */
f32 EaseOutBack(f32 t) noexcept {
    return acs::game::Easing::OutBack(t);
}

/** 跳ねるイージング。 */
f32 EaseOutBounce(f32 t) noexcept {
    return acs::game::Easing::OutBounce(t);
}

/** ばねのように揺れて収束するイージング。 */
f32 EaseOutElastic(f32 t) noexcept {
    return acs::game::Easing::OutElastic(t);
}

namespace {
/**
 * ウィジェット位置とラベルから安定した非ゼロの id を作る (FNV-1a)。
 *
 * @param x ウィジェットの X 座標。
 * @param y ウィジェットの Y 座標。
 * @param s ラベル文字列 (nullptr 可)。
 * @return 0 でないウィジェット識別子。
 */
u32 UiHash(f32 x, f32 y, const char* s) noexcept {
    u32 h = 2166136261u;
    h = (h ^ static_cast<u32>(static_cast<i32>(x))) * 16777619u;
    h = (h ^ static_cast<u32>(static_cast<i32>(y))) * 16777619u;
    if (s) for (const char* p = s; *p; ++p) h = (h ^ static_cast<u8>(*p)) * 16777619u;
    return h ? h : 1u;
}

/** UI 描画のため view をスクリーン座標 (カメラ無し) に切り替える。 */
void UiBeginScreen() noexcept {
    g_state.batch.SetView(ScreenWidth() * 0.5f, ScreenHeight() * 0.5f, 1.0f);
}

/** UI 描画後に view を現在のカメラ (シェイク込み) へ戻す。 */
void UiEndScreen() noexcept {
    g_state.batch.SetView(g_state.cam_x + g_state.shake_dx,
                          g_state.cam_y + g_state.shake_dy, g_state.cam_zoom);
}

/**
 * 矩形領域の中央にラベルを描く。
 *
 * @param x 領域の左上 X 座標。
 * @param y 領域の左上 Y 座標。
 * @param w 領域の幅。
 * @param h 領域の高さ。
 * @param label 描くラベル (空なら何もしない)。
 * @param col 文字色。
 */
void UiCenteredLabel(f32 x, f32 y, f32 w, f32 h, const char* label, FColor col) noexcept {
    if (!g_state.font_ok || !label || !*label) return;
    const f32 tw = g_state.font.MeasureWidth(label);
    const f32 th = g_state.font.LineHeight();
    DrawTextScaled(x + (w - tw) * 0.5f, y + (h - th) * 0.5f, label, ToVec4(col), 1.0f);
}
} // namespace

/** ボタンを描き、クリック (押して離す) されたフレームなら true を返す。 */
bool Button(f32 x, f32 y, f32 width, f32 height, const char* label) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return false; }
    const u32  id    = UiHash(x, y, label);
    const bool hover = PointInRect(MouseX(), MouseY(), x, y, width, height);
    if (hover && IsMousePressed(EMouseButton::Left)) g_state.ui_active = id;
    bool clicked = false;
    if (IsMouseReleased(EMouseButton::Left)) {
        if (g_state.ui_active == id && hover) clicked = true;
        if (g_state.ui_active == id) g_state.ui_active = 0;
    }
    const bool   pressed = (g_state.ui_active == id) && IsMouseDown(EMouseButton::Left);
    const FColor bg = pressed ? g_state.ui_press : (hover ? g_state.ui_hover : g_state.ui_base);
    UiBeginScreen();
    g_state.batch.DrawRect(x, y, width, height, ToVec4(bg));
    UiCenteredLabel(x, y, width, height, label, g_state.ui_text);
    UiEndScreen();
    return clicked;
}

/** チェックボックスを描き、クリックで *value を反転して変化フレームに true を返す。 */
bool Checkbox(f32 x, f32 y, f32 size, const char* label, bool* value) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return false; }
    if (!value) return false;
    const u32  id    = UiHash(x, y, label);
    const bool hover = PointInRect(MouseX(), MouseY(), x, y, size, size);
    if (hover && IsMousePressed(EMouseButton::Left)) g_state.ui_active = id;
    bool changed = false;
    if (IsMouseReleased(EMouseButton::Left)) {
        if (g_state.ui_active == id && hover) { *value = !*value; changed = true; }
        if (g_state.ui_active == id) g_state.ui_active = 0;
    }
    UiBeginScreen();
    g_state.batch.DrawRect(x, y, size, size, ToVec4(hover ? g_state.ui_hover : g_state.ui_base));
    if (*value) {
        const f32 p = size * 0.25f;
        g_state.batch.DrawRect(x + p, y + p, size - 2.0f * p, size - 2.0f * p, ToVec4(g_state.ui_text));
    }
    if (label && *label && g_state.font_ok) {
        const f32 th = g_state.font.LineHeight();
        DrawTextScaled(x + size + 8.0f, y + (size - th) * 0.5f, label, ToVec4(g_state.ui_text), 1.0f);
    }
    UiEndScreen();
    return changed;
}

/** スライダーを描き、ドラッグで *value を [min,max] で変えて変化フレームに true を返す。 */
bool Slider(f32 x, f32 y, f32 width, f32* value, f32 min, f32 max) noexcept {
    if (!g_state.frame_open) { WarnDrawOutsideFrame(); return false; }
    if (!value || max <= min) return false;
    const f32 height = 18.0f, knob = 14.0f;
    const u32 id     = UiHash(x, y, "slider");
    const bool hover = PointInRect(MouseX(), MouseY(), x, y - 4.0f, width, height + 8.0f);
    if (hover && IsMousePressed(EMouseButton::Left)) g_state.ui_active = id;
    if (g_state.ui_active == id && !IsMouseDown(EMouseButton::Left)) g_state.ui_active = 0;
    bool changed = false;
    if (g_state.ui_active == id) {
        const f32 nv = min + (max - min) * Clamp01((MouseX() - x) / width);
        if (nv != *value) { *value = nv; changed = true; }
    }
    const f32 t  = Clamp01((*value - min) / (max - min));
    const f32 cy = y + height * 0.5f;
    UiBeginScreen();
    g_state.batch.DrawRect(x, cy - 2.0f, width, 4.0f, ToVec4(g_state.ui_base));
    g_state.batch.DrawRect(x, cy - 2.0f, width * t, 4.0f, ToVec4(g_state.ui_hover));
    g_state.batch.DrawRect(x + width * t - knob * 0.5f, cy - knob * 0.5f, knob, knob, ToVec4(g_state.ui_text));
    UiEndScreen();
    return changed;
}

/** 即席 UI の配色を差し替える。 */
void SetUiColors(FColor base, FColor hover, FColor active, FColor text) noexcept {
    g_state.ui_base  = base;
    g_state.ui_hover = hover;
    g_state.ui_press = active;
    g_state.ui_text  = text;
}

/** 整数値をキーに紐づけて保存する。 */
void SaveInt(const char* key, i32 value) noexcept {
    if (!key) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    SetSaveValue(key, buf);
}

/** キーに紐づく整数値を読み込む (無い・パース不可なら default_value)。 */
i32 LoadInt(const char* key, i32 default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    FSaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    if (!s || !*s) return default_value;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s) return default_value;          // パース不可
    return static_cast<i32>(v);
}

/** 小数値をキーに紐づけて保存する。 */
void SaveFloat(const char* key, f32 value) noexcept {
    if (!key) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "%.9g", static_cast<f64>(value));
    SetSaveValue(key, buf);
}

/** キーに紐づく小数値を読み込む (無い・パース不可なら default_value)。 */
f32 LoadFloat(const char* key, f32 default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    FSaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    if (!s || !*s) return default_value;
    char* end = nullptr;
    const double v = strtod(s, &end);
    if (end == s) return default_value;
    return static_cast<f32>(v);
}

/** 文字列値をキーに紐づけて保存する (nullptr は空文字列扱い)。 */
void SaveString(const char* key, const char* value) noexcept {
    if (!key) return;
    SetSaveValue(key, value ? value : "");
}

/** キーに紐づく文字列値を読み込む (無ければ default_value)。 */
const char* LoadString(const char* key, const char* default_value) noexcept {
    if (!key) return default_value;
    EnsureSaveLoaded();
    FSaveEntry* e = FindSave(key);
    if (!e) return default_value;
    const char* s = e->value.Data();
    return s ? s : default_value;
}

/** そのキーが保存済みかを返す。 */
bool HasSaveKey(const char* key) noexcept {
    if (!key) return false;
    EnsureSaveLoaded();
    return FindSave(key) != nullptr;
}

/** 保存内容をすべて消去し save.dat も空にする。 */
void DeleteAllSaves() noexcept {
    g_save.ReleaseStorage();
    g_save_loaded = true;        // 「ロード済み（空）」状態にする
    FILE* f = nullptr;
    if (fopen_s(&f, "save.dat", "wb") == 0 && f) fclose(f);
}

/** 前フレームからの経過秒を返す。 */
f32 DeltaTime()    noexcept { return g_state.dt; }

/** OpenWindow からの累積秒を返す。 */
f32 ElapsedTime()  noexcept { return static_cast<f32>(g_state.timer.TotalSeconds()); }

/** 平滑化した 1 秒あたりフレーム数を返す。 */
i32 Fps()          noexcept { return static_cast<i32>(g_state.timer.SmoothedFPS() + 0.5f); }

/** 画面 (ウィンドウ) の幅を返す。 */
f32 ScreenWidth()  noexcept { return static_cast<f32>(g_state.window.Width()); }

/** 画面 (ウィンドウ) の高さを返す。 */
f32 ScreenHeight() noexcept { return static_cast<f32>(g_state.window.Height()); }

namespace jobdetail {

/** スレッドプールのワーカ起動を保証する (未起動なら自動 Init)。 */
bool Ready() noexcept {
    if (FThreadPool::WorkerCount() >= 1) return true;
    auto r = FThreadPool::Init();           // OpenWindow 未呼び出しでも初回使用で自動起動
    if (r.IsErr()) return false;
    if (!g_state.booted) g_state.jobs_pool_owned = true;   // jobs が起動した = jobs が後始末する
    if (!g_state.jobs_atexit) { std::atexit(&JobsAtexit); g_state.jobs_atexit = true; }
    return FThreadPool::WorkerCount() >= 1;
}

/** ParallelFor の自動チャンク幅 (ワーカ当たり ~4 チャンク) を求める。 */
i32 AutoGrain(i32 begin, i32 end) noexcept {
    const i32 n = end - begin;
    if (n <= 0) return 1;
    u32 w = FThreadPool::WorkerCount(); if (w < 1) w = 1;
    const i32 chunks = static_cast<i32>(w) * 4;            // ワーカ当たり ~4 チャンク
    const i32 g = n / (chunks > 0 ? chunks : 1);
    return g > 0 ? g : 1;
}

/** クロージャを非同期投入し、所属バッチのハンドルを返す (枯渇時は同期実行)。 */
FJobBatch SubmitAsync(FClosure* c, FJobBatch existing) noexcept {
    if (!c) return existing;
    FEasyState::FAsyncBatch* b = nullptr;
    u32 idx = 0;
    if (existing.id != 0) {                                // 既存 batch へ追加
        const u32 ei = (existing.id & 0xFFFFu) - 1u;
        if (ei < FEasyState::kMaxBatches && g_state.async_batches[ei].live &&
            g_state.async_batches[ei].gen == static_cast<u16>(existing.id >> 16)) {
            idx = ei; b = &g_state.async_batches[ei];
        }
    }
    if (!b) {                                              // 新規スロット確保
        for (u32 i = 0; i < FEasyState::kMaxBatches; ++i) {
            if (!g_state.async_batches[i].live) { idx = i; b = &g_state.async_batches[i]; break; }
        }
        if (!b) { RunClosureTask(c, 0); FreeClosure(c); return FJobBatch{}; }   // 枯渇 → 同期
        b->gen = static_cast<u16>(b->gen + 1u); if (b->gen == 0u) b->gen = 1u;
        b->live = true;
        b->closures.ReleaseStorage();
    }
    b->closures.PushBack(c);                               // batch がクロージャを所有
    FTask t{ &RunClosureTask, c, &b->counter };
    auto r = FThreadPool::Submit(t);
    if (r.IsErr()) { RunClosureTask(c, 0); }               // 投入失敗 → 同期実行 (解放は WaitJobs)
    return FJobBatch{ (static_cast<u32>(b->gen) << 16) | (idx + 1u) };
}

/** 構築中の依存グラフにクロージャをノードとして追加する。 */
FJobNode AddNode(FClosure* c) noexcept {
    if (!c) return FJobNode{};
    if (!g_state.pending_graph) {
        g_state.pending_graph  = new FJobGraph();
        g_state.graph_closures.ReleaseStorage();
        g_state.graph_handles.ReleaseStorage();
    }
    const FJobHandle h = g_state.pending_graph->Add(&RunClosureTask, c);
    const u32 id = static_cast<u32>(g_state.graph_handles.Size()) + 1u;
    g_state.graph_handles.PushBack(h);
    g_state.graph_closures.PushBack(c);
    return FJobNode{ id };
}

} // namespace jobdetail

/** バッチの全ジョブ完了を待ち、クロージャを解放してスロットを無効化する。 */
void WaitJobs(FJobBatch batch) noexcept {
    if (batch.id == 0) return;
    const u32 idx = (batch.id & 0xFFFFu) - 1u;
    if (idx >= FEasyState::kMaxBatches) return;
    FEasyState::FAsyncBatch& b = g_state.async_batches[idx];
    if (!b.live || b.gen != static_cast<u16>(batch.id >> 16)) return;   // 既に Wait 済 or 無効
    FThreadPool::Wait(b.counter);
    for (usize k = 0; k < b.closures.Size(); ++k)
        FreeClosure(static_cast<jobdetail::FClosure*>(b.closures[k]));
    b.closures.ReleaseStorage();
    b.live = false;
}

/** バッチの全ジョブが完了したかを待たずに返す (無効ハンドルは完了扱い)。 */
bool JobsDone(FJobBatch batch) noexcept {
    if (batch.id == 0) return true;
    const u32 idx = (batch.id & 0xFFFFu) - 1u;
    if (idx >= FEasyState::kMaxBatches) return true;
    FEasyState::FAsyncBatch& b = g_state.async_batches[idx];
    if (!b.live || b.gen != static_cast<u16>(batch.id >> 16)) return true;
    return b.counter.Finished();
}

/** 2 ノード間に実行順序の依存を張る (before の後に after が走る)。 */
void Then(FJobNode before, FJobNode after) noexcept {
    if (!g_state.pending_graph || before.id == 0 || after.id == 0) return;
    const u32 bi = before.id - 1u, ai = after.id - 1u;
    if (bi >= g_state.graph_handles.Size() || ai >= g_state.graph_handles.Size()) return;
    g_state.graph_handles[ai].DependOn(g_state.graph_handles[bi]);      // after は before の後
}

/** 構築した依存グラフを依存順に実行して全完了まで待ち、グラフを破棄する。 */
void RunJobs() noexcept {
    if (!g_state.pending_graph) return;
    (void)jobdetail::Ready();                              // プールを保証 (未起動なら自動 Init)
    FJobGraph* g = g_state.pending_graph;
    auto r = g->Submit();
    if (r.IsOk()) {
        g->Wait();
    } else {                                               // 循環/エントリ無 → 順次フォールバック
        ACS_LOG_WARN("easy: RunJobs の依存グラフが無効です (循環など)。順次実行にフォールバックします");
        for (usize k = 0; k < g_state.graph_closures.Size(); ++k)
            RunClosureTask(g_state.graph_closures[k], 0);
    }
    for (usize k = 0; k < g_state.graph_closures.Size(); ++k)
        FreeClosure(static_cast<jobdetail::FClosure*>(g_state.graph_closures[k]));
    g_state.graph_closures.ReleaseStorage();
    g_state.graph_handles.ReleaseStorage();
    delete g;
    g_state.pending_graph = nullptr;
}

/** 並列ワーカ数を返す (未起動なら自動起動を試みる)。 */
i32  WorkerCount() noexcept { (void)jobdetail::Ready(); return static_cast<i32>(FThreadPool::WorkerCount()); }

/** 今このコードがワーカスレッド上で動いているかを返す。 */
bool IsWorker()    noexcept { return FThreadPool::CurrentWorkerIndex() != FThreadPool::kNotAWorker; }

} // namespace acs::easy
