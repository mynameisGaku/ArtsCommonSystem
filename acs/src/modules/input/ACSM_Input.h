#pragma once
#define ACSM_INPUT
#pragma comment(lib, "JoyShockLibrary.lib")

#include "KeyWrapper.h"

namespace ACSM_Input
{
	void Initialize();
	void Finalize();
	void Update();

	bool GetKey(int key);
	bool GetKeyDown(int key);
	bool GetKeyUp(int key);
	bool GetKey(KeyCode key);
	bool GetKeyDown(KeyCode key);
	bool GetKeyUp(KeyCode key);
	bool GetButton(int button);
	bool GetButtonDown(int button);
	bool GetButtonUp(int button);
	void GetLeftStick(float& outLX, float& outLY);
	void GetRightStick(float& outRX, float& outRY);
	void GetTriggers(float& outL, float& outR);
	void GetAndFlushGyro(float& outX, float& outY, float& outZ); // ジャイロ
};

using namespace ACSM_Input;