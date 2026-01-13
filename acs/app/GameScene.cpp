#include "GameScene.h"

#include "GOC_Fader.h"

namespace
{
	GameObject* faderObj = nullptr;
}

void GameScene::OnAwake()
{
}

void GameScene::OnStart()
{
	faderObj = CreateGameObject<GameObject>();
	auto* fader = faderObj->AddGOC<GOC_Fader>();
	{
		GOC_Fader::SetupParameter param{};
		param.Color = 0x000000;
		param.Duration = 0.2f;
		param.FadeoutCallback = []()
			{
				SceneManager::GetInstance()->ChangeScene("Title");
			};
		fader->Setup(param);
		fader->Fadein();
	}
}

void GameScene::OnPreUpdate(float deltaTime)
{
}

void GameScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(KeyCode::Z))
	{
		auto* fader = faderObj->GetGOC<GOC_Fader>();
		fader->Fadeout();
	}
}

void GameScene::OnPreFixedUpdate(float fixedDeltaTime)
{
}

void GameScene::OnPostFixedUpdate(float fixedDeltaTime)
{
}

void GameScene::OnPreRender(float deltaTime)
{
}

void GameScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "GameScene", 0xff00ff);
}

void GameScene::OnDestroy()
{
	faderObj = nullptr;
}
