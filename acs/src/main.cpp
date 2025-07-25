#include "Pch.h"
#include <application/ecs_test/ECSTestApp.h>

#if ECSTEST
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT)
{
    SetOutApplicationLogValidFlag(FALSE);
    SetWindowSize(800, 600);
    ChangeWindowMode(TRUE);
    if (DxLib_Init() == -1)
    {
        return -1;
    }

    SetDrawScreen(DX_SCREEN_BACK);

    ECSTestApp app;
    app.OnStart();

    const int targetFPS = 60;
    const int frameDelay = 1000 / targetFPS;

    while (ProcessMessage() == 0 && CheckHitKey(KEY_INPUT_ESCAPE) == 0)
    {
        int startTime = GetNowCount();

        app.Update();
        app.Draw();
        ScreenFlip();

        int elapsedTime = GetNowCount() - startTime;
        if (elapsedTime < frameDelay)
        {
            WaitTimer(frameDelay - elapsedTime);
        }
    }

    app.OnDestroy();

    DxLib_End();

    return 0;
}
#else
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT)
{
    return 0;
}
#endif
