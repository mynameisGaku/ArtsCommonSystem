#include "Pch.h"
#include "App.h"

#include "TitleScene.h"
#include "GameScene.h"

void App::OnBoot()
{
	// アプリ起動後に入る場所。 DXLIBの初期化前なので、ここで事前オプションの設定を行う
}

void App::OnStart()
{
	ACSM_Input::Initialize();
	ACSU_Time::Init();

	SceneManager::GetInstance()->RegisterScene<TitleScene>("Title");
	SceneManager::GetInstance()->RegisterScene<GameScene>("Game");
	SceneManager::GetInstance()->SetDefault("Title");
}

void App::OnUpdate()
{
	ACSM_Input::Update();
	ACSU_Time::Update();
	SceneManager::GetInstance()->Update();
}

void App::OnDraw()
{
	SceneManager::GetInstance()->Draw();
}

void App::OnDestroy()
{
	SceneManager::GetInstance()->Destroy();
	ACSM_Input::Destroy();
}
