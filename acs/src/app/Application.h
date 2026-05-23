// SPDX-License-Identifier: Apache-2.0
// Application 基底（継承して OnStart / OnUpdate / OnRender / OnShutdown を実装）
//
// 使い方:
//   class MyGame : public Application {
//   public:
//       // フックは必ず noexcept override で宣言する
//       // （基底のフックが noexcept のため。noexcept を省くとコンパイルエラー）
//       void OnStart() noexcept override {
//           ACS_LOG_INFO("ゲーム開始");
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           if (Input::IsKeyPressed(EKey::Escape)) Quit();
//       }
//       void OnRender() noexcept override {
//           // 描画コマンド (BeginFrame / EndFrame は基底が呼ぶ)
//       }
//   };
//
//   ACS_DEFINE_MAIN(MyGame)   // エントリポイントを自動生成 (app/EntryPoint.h)
#pragma once

#include "foundation/Types.h"
#include "platform/Window.h"
#include "platform/Time.h"
#include "ecs/World.h"
#include "asset/AssetRegistry.h"
#include "render/Renderer.h"
#include "event/Timer.h"
#include "event/MessageBroker.h"
#include "app/AppConfig.h"

namespace acs {

class Application {
public:
    Application() noexcept = default;
    virtual ~Application() noexcept = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // メインループを実行（成功 = 0、失敗 = 非 0 を返す）
    int Run(const AppConfig& cfg) noexcept;

    // 終了要求（ループを抜ける、OnShutdown が呼ばれる）
    void Quit() noexcept { _running = false; }

    // 背景クリア色を動的に変更する（次フレームの描画から反映される）。
    // 既定値は AppConfig の clear_r/g/b/a（起動時）。
    void SetClearColor(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept {
        _clear_color = ClearColor{ r, g, b, a };
    }

    // 初学者がアクセスしやすいようにエンジンサブシステムを公開
    Window&         GetWindow()        noexcept { return _window; }
    Renderer&       GetRenderer()      noexcept { return _renderer; }
    World&          GetWorld()         noexcept { return _world; }
    AssetRegistry&  GetAssets()        noexcept { return _assets; }
    TimerManager&   GetTimers()        noexcept { return _timers; }
    MessageBroker&  GetEvents()        noexcept { return _events; }
    f32             DeltaTime()  const noexcept { return _dt; }
    u64             FrameCount() const noexcept { return _frame_timer.FrameCount(); }
    f32             FPS()        const noexcept { return _frame_timer.SmoothedFPS(); }

protected:
    // 派生クラスでオーバーライドするフック群（順番にフレームごとに呼ばれる）
    virtual void OnStart() noexcept   {}                  // 起動時 1 回
    virtual void OnUpdate(f32 /*dt*/) noexcept {}          // 毎フレーム更新
    virtual void OnRender() noexcept   {}                  // 毎フレーム描画
    virtual void OnShutdown() noexcept {}                  // 終了時 1 回
    virtual void OnEvent(const Event& /*e*/) noexcept {}   // イベント受信時

    // フレーム描画を完全に差し替えたい場合に true を返してフルカスタム実装する。
    // 戻り値が true のときは Application 側は BeginFrame / OnRender / EndFrame を呼ばず、
    // 派生クラスがコマンドリスト・スワップチェインを直接制御する責任を負う。
    // 用途: HDR + ポストプロセスのパイプラインを組む場合（HelloBloom 等）。
    virtual bool OnCustomFrame() noexcept { return false; }

private:
    // Window のイベントを Input に流しつつ OnEvent も呼ぶブリッジ
    static void EventBridge(void* user, const Event& e) noexcept;

    Window         _window;
    Renderer       _renderer;
    World          _world;
    AssetRegistry  _assets;
    TimerManager   _timers;
    MessageBroker  _events;
    FrameTimer     _frame_timer;
    f32            _dt       = 0.0f;
    bool           _running  = true;
    AppConfig      _cfg;
    ClearColor     _clear_color{};   // BeginFrame に渡す現在のクリア色
};

} // namespace acs
