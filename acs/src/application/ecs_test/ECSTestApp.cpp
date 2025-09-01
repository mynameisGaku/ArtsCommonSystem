#include "pch.h"
#include "ECSTestApp.h"

ECSTestApp::ECSTestApp()
{
	m_pECS = nullptr;
}

ECSTestApp::~ECSTestApp()
{
}

void ECSTestApp::OnStart()
{
	ACSM_Input::Initialize();
	m_pECS = new seecs::ECS;
}

void ECSTestApp::Update()
{
	ACSM_Input::Update();
}

void ECSTestApp::Draw()
{
	if (ACSM_Input::GetXInputButton(XPAD_A, 0))
	{
		DrawString(300, 300, "A Button Pressed.", 0xffffff);
	}
	if (ACSM_Input::GetXInputButton(XPAD_B, 0))
	{
		DrawString(300, 330, "B Button Pressed.", 0xffffff);
	}
	if (ACSM_Input::GetXInputButton(XPAD_X, 0))
	{
		DrawString(300, 360, "X Button Pressed.", 0xffffff);
	}
	if (ACSM_Input::GetXInputButton(XPAD_Y, 0))
	{
		DrawString(300, 390, "Y Button Pressed.", 0xffffff);
	}
}

void ECSTestApp::OnDestroy()
{
	ACSM_Input::Finalize();
	delete m_pECS;
}