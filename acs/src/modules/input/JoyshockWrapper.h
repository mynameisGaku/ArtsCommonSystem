#pragma once
#include "Pch.h"
#include <extension\Joyshock\JoyShockLibrary.h>

namespace Pad
{
	enum PadCode
	{
		PAD_S = JSMASK_S,
		PAD_W = JSMASK_W,
		PAD_E = JSMASK_E,
		PAD_N = JSMASK_N,
		PAD_ZL = JSMASK_ZL,
		PAD_ZR = JSMASK_ZR,
		PAD_L = JSMASK_L,
		PAD_R = JSMASK_R,
		PAD_MINUS = JSMASK_MINUS,
		PAD_PLUS = JSMASK_PLUS,
		PAD_LCLICK = JSMASK_LCLICK,
		PAD_RCLICK = JSMASK_RCLICK,
		PAD_HOME = JSMASK_HOME,
		PAD_CAPTURE = JSMASK_CAPTURE,
		PAD_UNKNOWN = 0,
	};

	enum XPadCode
	{
		XPAD_A = XINPUT_GAMEPAD_A,
		XPAD_B = XINPUT_GAMEPAD_B,
		XPAD_X = XINPUT_GAMEPAD_X,
		XPAD_Y = XINPUT_GAMEPAD_Y,
		XPAD_LB = XINPUT_GAMEPAD_LEFT_SHOULDER,
		XPAD_RB = XINPUT_GAMEPAD_RIGHT_SHOULDER,
		XPAD_BACK = XINPUT_GAMEPAD_BACK,
		XPAD_START = XINPUT_GAMEPAD_START,
		XPAD_LS = XINPUT_GAMEPAD_LEFT_THUMB,
		XPAD_RS = XINPUT_GAMEPAD_RIGHT_THUMB,
		XPAD_DPAD_UP = XINPUT_GAMEPAD_DPAD_UP,
		XPAD_DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN,
		XPAD_DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT,
		XPAD_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT,
		XPAD_UNKNOWN = 0,
	};
}
using namespace Pad;

class JoyshockWrapper
{
public:
	static JoyshockWrapper& Instance();


	bool Initialize();


	void Destroy();


	void Uninitialize();


	void RescanDevices();


	void Update();

	int GetDeviceCount() const;

	int GetHandleByIndex(int index) const;

	bool IsConnected(int index) const;

	bool GetRawButton(int index, uint32_t buttonMask) const;
	bool GetRawButtonDown(int index, uint32_t buttonMask) const;
	bool GetRawButtonUp(int index, uint32_t buttonMask) const;

	void GetLeftStick(int index, float& outLX, float& outLY) const;
	void GetRightStick(int index, float& outRX, float& outRY) const;
	void GetTriggers(int index, float& outL, float& outR) const;
	void GetAndFlushGyro(int index, float& outX, float& outY, float& outZ);

	void SetAutomaticCalibration(int index, bool enable);
	void SetGyroSpace(int index, int space);
	void SetDeadZones(float stickDeadZone, float triggerThreshold);

	bool GetRawButton(uint32_t buttonMask) const;
	bool GetRawButtonDown(uint32_t buttonMask) const;
	bool GetRawButtonUp(uint32_t buttonMask) const;
	void GetLeftStick(float& outLX, float& outLY) const;
	void GetRightStick(float& outRX, float& outRY) const;
	void GetTriggers(float& outL, float& outR) const;
	void GetAndFlushGyro(float& outX, float& outY, float& outZ);

	bool GetXInputButton(int button, int padNum = 0);
	bool GetXInputButtonDown(int button, int padNum = 0);
	bool GetXInputButtonUp(int button, int padNum = 0);
	void GetXInputLeftStick(float& outLX, float& outLY, int padNum = 0) const;
	void GetXInputRightStick(float& outRX, float& outRY, int padNum = 0) const;
	void GetXInputTriggers(float& outL, float& outR, int padNum = 0) const;

private:
	JoyshockWrapper();
	~JoyshockWrapper();
	JoyshockWrapper(const JoyshockWrapper&) = delete;
	JoyshockWrapper& operator=(const JoyshockWrapper&) = delete;

	static JoyshockWrapper* m_pInstance;

	struct DeviceState
	{
		int handle;
		bool connected;
		uint32_t prevButtons;
		uint32_t currButtons;
		JOY_SHOCK_STATE state;
		float lastGyroX;
		float lastGyroY;
		float lastGyroZ;
	};

	struct XInputDeviceState
	{
		bool connected;
		::XINPUT_STATE currXinputState;
		::XINPUT_STATE prevXinputState;
		int handle; // 0..3 / -1
	};

	void RefreshHandleList();
	bool IsValidIndex(int index) const;

	void RefreshXInputHandleList();

private:
	bool							m_Initialized;
	std::vector<DeviceState>		m_Devices;
	std::vector<XInputDeviceState>	m_XInputDevices;
	float							m_StickDeadZone;
	float							m_TriggerThreshold;
	mutable std::mutex				m_Mutex;
};
