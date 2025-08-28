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
		
	/// <summary>
	/// 仮想キー名に物理入力を登録する
	/// </summary>
	/// <param name="key">任意の登録名</param>
	/// <param name="button">0xFF以下ならキーボードVK、0xFFよりも↑なら JoyShock のボタンマスク</param>
	/// <returns></returns>
	bool SubscribeVirtualKey(const std::string& key, int button);

	///<summary>仮想キーの現在状態（押下中）を取得する。</summary>
	bool GetVirtualKey(const std::string& key);

	///<summary>仮想キーの押した瞬間を取得する（前フレーム非押下 → 今フレーム押下）。</summary>
	bool GetVirtualKeyDown(const std::string& key);

	///<summary>仮想キーの離した瞬間を取得する（前フレーム押下 → 今フレーム非押下）。</summary>
	bool GetVirtualKeyUp(const std::string& key);

	// jsonからキー設定を読み込む
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
};

using namespace ACSM_Input;