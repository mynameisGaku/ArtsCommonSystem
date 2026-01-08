#include "TitleScene.h"

#include "BoxRenderComponent.h"
#include <core/Sky/SkyComponent.h>
#include <core/Sky/SkyMeshComponent.h>

namespace
{
	GameObject* pSky;

	GameObject* pObj_1;
	GameObject* pObj_2;
	BoxRenderComponent* pBox_1;
	BoxRenderComponent* pBox_2;
}

void TitleScene::OnInitialize()
{
	// Scene初期化時
	GameObject* cameraObj = CreateGameObject<GameObject>();

	// CameraComponent は既にある想定
	auto* cam = cameraObj->AddComponent<CameraComponent>();
	cam->SetNearFar(0.1f, 10000000.0f);

	auto* sky = cameraObj->AddComponent<SkyMeshComponent>();

	SkyMeshComponent::SetupParam sp;
	sp.TexW = 256;
	sp.TexH = 128;
	sp.Radius = 5000.0f;
	sp.Slices = 64;
	sp.Stacks = 32;

	sky->Setup(sp);
	pSky = cameraObj;


	// 太陽初期値を Blackboard に入れておく（UpdateSun の中身でもよい）
	cameraObj->GetBlackboard().Set(BlackboardKeys::SunDirection, ACSU_Math::Vec3(0.0f, 1.0f, 0.0f));
	cameraObj->GetBlackboard().Set(BlackboardKeys::SunIntensity, 1.0f);


	// ゲームオブジェクトの追加など
	pObj_1 = CreateGameObject<GameObject>();
	pObj_2 = CreateGameObject<GameObject>();
	pBox_1 = pObj_1->AddComponent<BoxRenderComponent>(BoxRenderComponent({ 100.0f, 100.0f, 0.0f }, 50.0f));
	pBox_2 = pObj_2->AddComponent<BoxRenderComponent>(BoxRenderComponent({ 100.0f, 160.0f, 0.0f }, 50.0f));
	pObj_2->GetComponent<Transform>()->SetLocalPosition(ACSU_Math::Vec3(0.0f, 60.0f, 0.0f));

}

void TitleScene::OnPreUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(KeyCode::G))
	{
		SceneManager::GetInstance()->ChangeScene("Game");
	}
}


ACSU_Math::Vec3 g_SunDir = ACSU_Math::Vec3(0.0f, 1.0f, 0.0f);
float g_SunIntensity = 1.0f;

void TitleScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKey(KeyCode::UP))
	{
		g_SunDir.y += 0.5f * deltaTime;
	}
	if (ACSM_Input::GetKey(KeyCode::DOWN))
	{
		g_SunDir.y -= 0.5f * deltaTime;
	}
	if (ACSM_Input::GetKey(KeyCode::LEFT))
	{
		g_SunDir.x += 0.5f * deltaTime;
	}
	if (ACSM_Input::GetKey(KeyCode::RIGHT))
	{
		g_SunDir.x -= 0.5f * deltaTime;
	}
	if (ACSM_Input::GetKey(KeyCode::U))
	{
		g_SunIntensity += 0.5f * deltaTime;
	}
	if (ACSM_Input::GetKey(KeyCode::J))
	{
		g_SunIntensity -= 0.5f * deltaTime;
	}
	pSky->GetBlackboard().Set(BlackboardKeys::SunDirection, g_SunDir);
	pSky->GetBlackboard().Set(BlackboardKeys::SunIntensity, g_SunIntensity);

	pObj_1->GetComponent<Transform>()->SetLocalPosition(
		ACSU_Math::Vec3(
			pObj_1->GetComponent<Transform>()->GetLocalPosition().x + 50.0f * deltaTime,
			pObj_1->GetComponent<Transform>()->GetLocalPosition().y,
			0.0f));
}

void TitleScene::OnPreFixedUpdate(float fixedDeltaTime)
{
	pObj_2->GetComponent<Transform>()->SetLocalPosition(
		ACSU_Math::Vec3(
			pObj_2->GetComponent<Transform>()->GetLocalPosition().x + 50.0f * fixedDeltaTime,
			pObj_2->GetComponent<Transform>()->GetLocalPosition().y,
			0.0f));
}

void TitleScene::OnPostFixedUpdate(float fixedDeltaTime)
{
}

void TitleScene::OnPreRender(float deltaTime)
{
	DrawString(250, 100, "TitleScene", 0xffffff);
}

void TitleScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "TitleScene", 0xffffff);
	DrawFormatString(100, 50, 0x000000, "sundir: {%.4f, %.4f, %.4f]", g_SunDir.x, g_SunDir.y, g_SunDir.z);
	DrawFormatString(100, 100, 0x000000, "sunintensity: %.4f", g_SunIntensity);
}

void TitleScene::OnDestroy()
{
}
