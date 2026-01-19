#include "TitleScene.h"

#include "GOC_Sprite.h"
#include "GOC_Rotator.h"
#include "GOC_Fader.h"
#include "GOC_EcsWorld.h"
#include "GOC_Mesh.h"

namespace
{
	GameObject* spriteObj = nullptr;
	GameObject* faderObj = nullptr;
	GameObject* ecsWorld = nullptr;
	GameObject* lightObj = nullptr;
	GameObject* meshObj = nullptr;
}

void TitleScene::OnAwake()
{
}

void TitleScene::OnStart()
{
	ACSU_Time::SetTimeScale(1.0f);

	spriteObj = CreateGameObject<GameObject>();
	auto* spr = spriteObj->AddGOC<GOC_Sprite>();
	{
		GOC_Sprite::SetupParam param{};
		param.Path = "assets/sprites/cursor.png";
		param.DivX = 5;
		param.DivY = 1;
		param.CellW = 48;
		param.CellH = 48;
		param.Loop = true;
		param.FrameTime = 0.1;
		spr->Setup(param);
	}
	auto* rotator = spriteObj->AddGOC<GOC_Rotator>();
	{
		GOC_Rotator::SetupParameter param{};
		param.RotateSpeed = 15.0f;
		param.RotateRange = 0.0f;
		rotator->Setup(param);
	}

	faderObj = CreateGameObject<GameObject>();
	auto* fader = faderObj->AddGOC<GOC_Fader>();
	{
		GOC_Fader::SetupParameter param{};
		param.Color = 0x000000;
		param.Duration = 0.2f;
		param.FadeoutCallback = []()
			{
				SceneManager::GetInstance()->ChangeScene("Game");
			};
		fader->Setup(param);
		fader->Fadein();
		faderObj->SetLayer("UI");
	}

	lightObj = CreateGameObject<GameObject>();
	{
		auto* light = lightObj->AddGOC<GOC_Light>();

		light->SetupDirectional(ACSU_Math::Vector3(1.0f, 1.0f, 1.0f), 1.0f);
	}

	meshObj = CreateGameObject<GameObject>();
	{
		auto* mesh = meshObj->AddGOC<GOC_Mesh>();
		mesh->Setup("assets/models/NoFaceGuy_Original.mv1");
		Transform* meshTrs = meshObj->GetGOC<Transform>();
		meshTrs->SetLocalPosition(ACSU_Math::Vector3(1.0f, 0.0f, 0.0f));
		meshTrs->SetLocalScale(ACSU_Math::Vector3(1.0f, 1.0f, 1.0f));
	}

	ecsWorld = CreateGameObject<GameObject>();
	auto* world = ecsWorld->AddGOC<GOC_EcsWorld>();
}

void TitleScene::OnPreUpdate(float deltaTime)
{
	auto mousePos = ACSM_Input::GetMousePoint();
	spriteObj->GetGOC<Transform>()->SetLocalPosition(ACSU_Math::Vector3(mousePos.x, mousePos.y, 0.0f));

	const SceneContext& ctx = GetContext();
	ACSU_Math::Vector3 currentSunDir = ctx.SunDirection;

	if (lightObj)
	{
		if (auto* light = lightObj->GetGOC<GOC_Light>())
		{
			light->SetDirection(currentSunDir);
		}
	}

	auto* mesh = meshObj->GetGOC<GOC_Mesh>();
	mesh->Setup("assets/models/NoFaceGuy_Original.mv1");
	Transform* meshTrs = meshObj->GetGOC<Transform>();
	meshTrs->Move(Vector3(1.0f * deltaTime, 0.0f, 0.0f));
	meshTrs->SetLocalScale(ACSU_Math::Vector3(1.0f, 1.0f, 1.0f));
	meshTrs->RotateEulerLocal(ACSU_Math::Vector3(0.0f, 1.0f, 0.0f));
}

void TitleScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(KeyCode::Z))
	{
		auto* fader = faderObj->GetGOC<GOC_Fader>();
		fader->Fadeout();
	}
}

void TitleScene::OnPreFixedUpdate(float fixedDeltaTime) {}
void TitleScene::OnPostFixedUpdate(float fixedDeltaTime) {}
void TitleScene::OnPreRender(float deltaTime) {}
void TitleScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "TitleScene", 0xff0000);
	DrawFormatString(50, 150, 0xff0000, "fps : %.f", GetFPS());
}
void TitleScene::OnDestroy()
{
}