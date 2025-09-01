#pragma once
#define ACSM_INPUT
#pragma comment(lib, "JoyShockLibrary.lib")

#include "Pch.h"
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
	void GetAndFlushGyro(float& outX, float& outY, float& outZ);

	bool GetButton(int button, int padNum);
	bool GetButtonDown(int button, int padNum);
	bool GetButtonUp(int button, int padNum);
	void GetLeftStick(float& outLX, float& outLY, int padNum);
	void GetRightStick(float& outRX, float& outRY, int padNum);
	void GetTriggers(float& outL, float& outR, int padNum);

	/// <summary>
	/// 仮想キー名に物理入力を登録する
	/// 0xFF以下 = Keyboard VK / それ以外 = Pad（JSL or XInput）
	/// </summary>
	bool SubscribeVirtualKey(const std::string& key, int button);

	///<summary>仮想キー（押下中/Down/Up）</summary>
	bool GetVirtualKey(const std::string& key);
	bool GetVirtualKeyDown(const std::string& key);
	bool GetVirtualKeyUp(const std::string& key);

	// jsonからキー設定を読み込む（Keyboard / Pad(JSL) / XPad(XInput)）
	void SubscribeVirtualKeyFromJson(const std::string& path);

#ifdef ACSM_DXLIB
	POINT GetMousePoint();
	enum class MouseButton
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		X1 = 3,
		X2 = 4,
		Max = 5
	};
	bool GetMouseButton(MouseButton button);
	bool GetMouseButtonDown(MouseButton button);
	bool GetMouseButtonUp(MouseButton button);
	void SetMousePosition(int x, int y);
	void SetMouseVisible(bool visible);
#endif

	bool GetXInputButton(int xinputButton, int padNum = 0);
	bool GetXInputButtonDown(int xinputButton, int padNum = 0);
	bool GetXInputButtonUp(int xinputButton, int padNum = 0);
	void GetXInputLeftStick(float& outLX, float& outLY, int padNum = 0);
	void GetXInputRightStick(float& outRX, float& outRY, int padNum = 0);
	void GetXInputTriggers(float& outL, float& outR, int padNum = 0);
};

using namespace ACSM_Input;
