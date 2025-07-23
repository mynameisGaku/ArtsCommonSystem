#include "DxLib.h"
#include <application/ecs_test/ECSTestApp.h>

#if ECSTEST
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT)
{
    SetWindowSize(800, 600);
    ChangeWindowMode(TRUE);
    if (DxLib_Init() == -1)
    {
        return -1;
    }

    SetDrawScreen(DX_SCREEN_BACK);  // バックバッファ描画を有効にする（推奨）

    ECSTestApp app;
    app.Start();

    const int targetFPS = 60;
    const int frameDelay = 1000 / targetFPS;

    while (ProcessMessage() == 0 && CheckHitKey(KEY_INPUT_ESCAPE) == 0)
    {
        // フレーム開始時刻
        int startTime = GetNowCount();

        // 1フレームの処理
        app.Update();
        app.Draw();
        ScreenFlip();  // 裏画面を表に反映

        // フレーム時間の調整（60fpsになるように）
        int elapsedTime = GetNowCount() - startTime;
        if (elapsedTime < frameDelay)
        {
            WaitTimer(frameDelay - elapsedTime);
        }
    }

    app.Destroy();

    DxLib_End();

    return 0;
}
#endif
