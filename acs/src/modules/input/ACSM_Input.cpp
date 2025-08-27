#include "ACSM_Input.h"

#include "JoyshockWrapper.h"
#include "KeyWrapper.h"

void ACSM_Input::Initialize()
{
	JoyshockWrapper::Instance().Initialize();
}
void ACSM_Input::Finalize()
{
	JoyshockWrapper::Instance().Finalize();
}
void ACSM_Input::Update()
{
	JoyshockWrapper::Instance().Update();
	KeyWrapper::Instance().Update();
}
bool ACSM_Input::GetKey(int key)
{
	return KeyWrapper::Instance().GetKey(key);
}
bool ACSM_Input::GetKeyDown(int key)
{
	return KeyWrapper::Instance().GetKeyDown(key);
}
bool ACSM_Input::GetKeyUp(int key)
{
	return KeyWrapper::Instance().GetKeyUp(key);
}
bool ACSM_Input::GetKey(KeyCode key)
{
	return KeyWrapper::Instance().GetKey(key);
}
bool ACSM_Input::GetKeyDown(KeyCode key)
{
	return KeyWrapper::Instance().GetKeyDown(key);
}
bool ACSM_Input::GetKeyUp(KeyCode key)
{
	return KeyWrapper::Instance().GetKeyUp(key);
}
bool ACSM_Input::GetButton(int button)
{
	return JoyshockWrapper::Instance().GetRawButton(button);
}
bool ACSM_Input::GetButtonDown(int button)
{
	return JoyshockWrapper::Instance().GetRawButtonDown(button);
}
bool ACSM_Input::GetButtonUp(int button)
{
	return JoyshockWrapper::Instance().GetRawButtonUp(button);
}
void ACSM_Input::GetLeftStick(float& outLX, float& outLY)
{
	JoyshockWrapper::Instance().GetLeftStick(outLX, outLY);
}
void ACSM_Input::GetRightStick(float& outRX, float& outRY)
{
	JoyshockWrapper::Instance().GetRightStick(outRX, outRY);
}
void ACSM_Input::GetTriggers(float& outL, float& outR)
{
	JoyshockWrapper::Instance().GetTriggers(outL, outR);
}
void ACSM_Input::GetAndFlushGyro(float& outX, float& outY, float& outZ)
{
	JoyshockWrapper::Instance().GetAndFlushGyro(outX, outY, outZ);
}