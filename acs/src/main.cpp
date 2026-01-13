#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#ifdef _DEBUG
#define DBG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__);
#else
#define DBG_NEW new
#endif

#include "Pch.h"

void Shutdown()
{
	ImGui_ImplDXlib_Shutdown();
	ImGui::DestroyContext();
	DxLib_End();
#ifdef _CRTDBG_MAP_ALLOC
#ifdef _DEBUG
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
	_CrtDumpMemoryLeaks();
#endif
#endif
}

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ INT)
{
	//_CrtSetBreakAlloc(242);

	ApplicationManager* appManager = ApplicationManager::GetInstance();
	IApplication* app = appManager->GetCurrentApplication();

	app->OnBoot();

	SetOutApplicationLogValidFlag(FALSE);
	SetWindowSize(1280, 720);
	SetGraphMode(1280, 720, 32);
	ChangeWindowMode(TRUE);
	if (DxLib_Init() == -1)
	{
		return -1;
	}

	SetDrawScreen(DX_SCREEN_BACK);
	SetHookWinProc([](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT /*CALLBACK*/
		{
			// DxLibとImGuiのウィンドウプロシージャを両立させる
			SetUseHookWinProcReturnValue(FALSE);
			return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
		});

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	//io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\meiryo.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
	ImGui_ImplDXlib_Init();

	SetChangeScreenModeGraphicsSystemResetFlag(FALSE);

	if (app == nullptr)
	{
		appManager->Destroy();
		Shutdown();
		return 0;
	}

	SetDrawScreen(DX_SCREEN_BACK);
	SetAlwaysRunFlag(TRUE);
	SetUseZBuffer3D(TRUE);
	SetWriteZBuffer3D(TRUE);

	app->OnStart();

	const int targetFPS = 240;
	const int frameDelay = 1000 / targetFPS;

	while (ProcessMessage() == 0 && CheckHitKey(KEY_INPUT_ESCAPE) == 0)
	{
		ImGui_ImplDXlib_NewFrame();
		ImGui::NewFrame();

		int startTime = GetNowCount();

		app->OnUpdate();
		app->OnDraw();

		ScreenFlip();
		ClearDrawScreen();

		int elapsedTime = GetNowCount() - startTime;
		if (elapsedTime < frameDelay)
		{
			WaitTimer(frameDelay - elapsedTime);
		}

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDXlib_RenderDrawData();

		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	app->OnDestroy();
	appManager->Destroy();

	Shutdown();

	return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
		return true;

	return false;
}