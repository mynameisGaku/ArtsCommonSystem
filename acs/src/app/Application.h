// Application 基底（継承して OnStart / OnUpdate / OnRender / OnShutdown を実装）
//
// 使い方:
//   class MyGame : public Application {
//   public:
//       void OnStart() override {
//           Logger::Info("ゲーム開始");
//       }
//       void OnUpdate(f32 dt) override {
//           if (Input::IsKeyPressed(Key::Escape)) Quit();
//       }
//       void OnRender() override {
//           // 描画コマンド (BeginFrame / EndFrame は基底が呼ぶ)
//       }
//   };
//
//   int main() {
//       MyGame g;
//       AppConfig cfg;
//       cfg.title = L"My Game";
//       return g.Run(cfg);
//   }
#pragma once

#include "foundation/Types.h"
#include "platform/Window.h"
#include "platform/Time.h"
#include "ecs/World.h"
#include "asset/AssetRegistry.h"
#include "render/Renderer.h"
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

    // 初学者がアクセスしやすいようにエンジンサブシステムを公開
    Window&         GetWindow()        noexcept { return _window; }
    Renderer&       GetRenderer()      noexcept { return _renderer; }
    World&          GetWorld()         noexcept { return _world; }
    AssetRegistry&  GetAssets()        noexcept { return _assets; }
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

private:
    // Window のイベントを Input に流しつつ OnEvent も呼ぶブリッジ
    static void EventBridge(void* user, const Event& e) noexcept;

    Window         _window;
    Renderer       _renderer;
    World          _world;
    AssetRegistry  _assets;
    FrameTimer     _frame_timer;
    f32            _dt       = 0.0f;
    bool           _running  = true;
    AppConfig      _cfg;
};

} // namespace acs
