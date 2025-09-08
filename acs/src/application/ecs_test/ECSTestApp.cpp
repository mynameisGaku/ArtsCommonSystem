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
		DrawString(300, 0, "A Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 0, "A Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_B, 0))
	{
		DrawString(300, 30, "B Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 30, "B Button Pressed.", 0x111111);


	if (ACSM_Input::GetXInputButton(XPAD_X, 0))
	{
		DrawString(300, 60, "X Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 60, "X Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_Y, 0))
	{
		DrawString(300, 90, "Y Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 90, "Y Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_BACK, 0))
	{
		DrawString(300, 120, "Back Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 90, "Y Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_START, 0))
	{
		DrawString(300, 150, "Start Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 150, "Start Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_DPAD_DOWN, 0))
	{
		DrawString(300, 180, "DPad Down Pressed.", 0xffffff);
	}
	else
		DrawString(300, 180, "DPad Down Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_DPAD_UP, 0))
	{
		DrawString(300, 210, "DPad Up Pressed.", 0xffffff);
	}
	else
		DrawString(300, 210, "DPad Up Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_DPAD_LEFT, 0))
	{
		DrawString(300, 240, "DPad Left Pressed.", 0xffffff);
	}
	else
		DrawString(300, 240, "DPad Left Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_DPAD_RIGHT, 0))
	{
		DrawString(300, 270, "DPad Right Pressed.", 0xffffff);
	}
	else
		DrawString(300, 270, "DPad Right Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_LB, 0))
	{
		DrawString(300, 300, "Left Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 300, "Left Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_RB, 0))
	{
		DrawString(300, 330, "Right Button Pressed.", 0xffffff);
	}
	else
		DrawString(300, 330, "Right Button Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_LS, 0))
	{
		DrawString(300, 360, "Left Stick Pressed.", 0xffffff);
	}
	else
		DrawString(300, 360, "Left Stick Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_RS, 0))
	{
		DrawString(300, 390, "Right Stick Pressed.", 0xffffff);
	}
	else
		DrawString(300, 390, "Right Stick Pressed.", 0x111111);

	if (ACSM_Input::GetXInputButton(XPAD_START, 0))
	{
		DrawString(300, 420, "Start Pressed.", 0xffffff);
	}
	else
		DrawString(300, 420, "Start Pressed.", 0x111111);




	float lx, ly;
	ACSM_Input::GetXInputLeftStick(lx, ly, 0);
	DrawFormatString(300, 420, 0xffffff, "Left Stick: (%.02f, %.02f)", lx, ly);
	float rx, ry;
	ACSM_Input::GetXInputRightStick(rx, ry, 0);
	DrawFormatString(300, 450, 0xffffff, "Right Stick: (%.02f, %.02f)", rx, ry);
	float lt, rt;
	ACSM_Input::GetXInputTriggers(lt, rt, 0);
	DrawFormatString(300, 480, 0xffffff, "Triggers: (L: %.02f, R: %.02f)", lt, rt);
}

void ECSTestApp::OnDestroy()
{
	ACSM_Input::Finalize();
	delete m_pECS;
}