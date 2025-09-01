#include <algorithm>
#include <cmath>
#include "JoyshockWrapper.h"
#include "ACSM_Input.h"

namespace
{
	static inline float ApplyDeadZone(float v, float dz)
	{
		if (std::fabs(v) < dz)
		{
			return 0.0f;
		}

		const float sign = (v >= 0.0f) ? 1.0f : -1.0f;
		const float t = (std::fabs(v) - dz) / (1.0f - dz);
		return std::clamp(sign * t, -1.0f, 1.0f);
	}
}

JoyshockWrapper* JoyshockWrapper::m_pInstance = nullptr;

JoyshockWrapper& JoyshockWrapper::Instance()
{
	if (not m_pInstance)
	{
		m_pInstance = new JoyshockWrapper;
	}
	return *m_pInstance;
}

void JoyshockWrapper::Destroy()
{
	if (m_pInstance)
	{
		delete m_pInstance;
	}
	m_pInstance = nullptr;
}

JoyshockWrapper::JoyshockWrapper()
	: m_Initialized(false)
	, m_StickDeadZone(0.10f)     // 初期値：10%
	, m_TriggerThreshold(0.10f)  // 初期値：10%
{
}

JoyshockWrapper::~JoyshockWrapper()
{
	Finalize();
}

bool JoyshockWrapper::Initialize()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (m_Initialized)
	{
		return true;
	}

	const int connected = JslConnectDevices();      // 現在接続されているデバイスを検出
	(void)connected;                                 // 戻り値はすぐには使用しない（RefreshHandleList で正確に反映）

	RefreshHandleList();
	m_Initialized = true;
	return true;
}

void JoyshockWrapper::Finalize()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (!m_Initialized)
	{
		return;
	}

	m_Devices.clear();
	JslDisconnectAndDisposeAll();                    // すべてのデバイス切断
	m_Initialized = false;
}

void JoyshockWrapper::RescanDevices()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (!m_Initialized)
	{
		return;
	}

	// 既存接続を維持したまま再検出（JSL v3 以降は安全）
	JslConnectDevices();
	RefreshHandleList();

	// XInput更新
	RefreshXInputHandleList();
}

void JoyshockWrapper::Update()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (!m_Initialized)
	{
		return;
	}

	for (auto& dev : m_Devices)
	{
		if (!dev.connected)
		{
			continue;
		}

		if (!JslStillConnected(dev.handle))
		{
			dev.connected = false;
			continue;
		}

		dev.prevButtons = dev.currButtons;

		// アナログ含む簡易状態取得
		dev.state = JslGetSimpleState(dev.handle);
		dev.currButtons = static_cast<uint32_t>(dev.state.buttons);

		// ジャイロは必要に応じて逐次取得（ここでは直近値を保持）
		float gx = 0.0f, gy = 0.0f, gz = 0.0f;
		JslGetAndFlushAccumulatedGyro(dev.handle, gx, gy, gz);
		dev.lastGyroX = gx;
		dev.lastGyroY = gy;
		dev.lastGyroZ = gz;
	}
}

int JoyshockWrapper::GetDeviceCount() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return static_cast<int>(m_Devices.size());
}

int JoyshockWrapper::GetHandleByIndex(int index) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index))
	{
		return 0;
	}
	return m_Devices[index].handle;
}

bool JoyshockWrapper::IsConnected(int index) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index))
	{
		return false;
	}
	return m_Devices[index].connected;
}

bool JoyshockWrapper::GetRawButton(int index, uint32_t buttonMask) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return false;
	}
	return (m_Devices[index].currButtons & buttonMask) != 0;
}

bool JoyshockWrapper::GetRawButtonDown(int index, uint32_t buttonMask) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return false;
	}
	const uint32_t prevB = m_Devices[index].prevButtons;
	const uint32_t currB = m_Devices[index].currButtons;
	return ((~prevB) & currB & buttonMask) != 0;
}

bool JoyshockWrapper::GetRawButtonUp(int index, uint32_t buttonMask) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return false;
	}
	const uint32_t prevB = m_Devices[index].prevButtons;
	const uint32_t currB = m_Devices[index].currButtons;
	return ((prevB) & (~currB) & buttonMask) != 0;
}

void JoyshockWrapper::GetLeftStick(int index, float& outLX, float& outLY) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	outLX = 0.0f;
	outLY = 0.0f;

	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}

	float x = m_Devices[index].state.stickLX;
	float y = m_Devices[index].state.stickLY;
	x = ApplyDeadZone(x, m_StickDeadZone);
	y = ApplyDeadZone(y, m_StickDeadZone);
	outLX = x;
	outLY = y;
}

void JoyshockWrapper::GetRightStick(int index, float& outRX, float& outRY) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	outRX = 0.0f;
	outRY = 0.0f;

	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}

	float x = m_Devices[index].state.stickRX;
	float y = m_Devices[index].state.stickRY;
	x = ApplyDeadZone(x, m_StickDeadZone);
	y = ApplyDeadZone(y, m_StickDeadZone);
	outRX = x;
	outRY = y;
}

void JoyshockWrapper::GetTriggers(int index, float& outL, float& outR) const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	outL = 0.0f;
	outR = 0.0f;

	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}

	float l = m_Devices[index].state.lTrigger;
	float r = m_Devices[index].state.rTrigger;

	// Nintendo 系は 0/1 の場合あり。しきい値を超えたら 1 とみなす。
	l = (l >= m_TriggerThreshold) ? l : 0.0f;
	r = (r >= m_TriggerThreshold) ? r : 0.0f;

	outL = std::clamp(l, 0.0f, 1.0f);
	outR = std::clamp(r, 0.0f, 1.0f);
}

void JoyshockWrapper::GetAndFlushGyro(int index, float& outX, float& outY, float& outZ)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	outX = outY = outZ = 0.0f;

	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}

	// Update 内でも一度読み出しているが、ここで再度読み出して最新を返す。
	float gx = 0.0f, gy = 0.0f, gz = 0.0f;
	JslGetAndFlushAccumulatedGyro(m_Devices[index].handle, gx, gy, gz);
	m_Devices[index].lastGyroX = gx;
	m_Devices[index].lastGyroY = gy;
	m_Devices[index].lastGyroZ = gz;

	outX = gx;
	outY = gy;
	outZ = gz;
}

void JoyshockWrapper::SetAutomaticCalibration(int index, bool enable)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}
	JslSetAutomaticCalibration(m_Devices[index].handle, enable);
}

void JoyshockWrapper::SetGyroSpace(int index, int space)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!IsValidIndex(index) || !m_Devices[index].connected)
	{
		return;
	}
	JslSetGyroSpace(m_Devices[index].handle, space);
}

void JoyshockWrapper::SetDeadZones(float stickDeadZone, float triggerThreshold)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_StickDeadZone = std::clamp(stickDeadZone, 0.0f, 0.99f);
	m_TriggerThreshold = std::clamp(triggerThreshold, 0.0f, 1.0f);
}

bool JoyshockWrapper::GetRawButton(uint32_t buttonMask) const
{
	return GetRawButton(0, buttonMask);
}

bool JoyshockWrapper::GetRawButtonDown(uint32_t buttonMask) const
{
	return GetRawButtonDown(0, buttonMask);
}

bool JoyshockWrapper::GetRawButtonUp(uint32_t buttonMask) const
{
	return GetRawButtonUp(0, buttonMask);
}

void JoyshockWrapper::GetLeftStick(float& outLX, float& outLY) const
{
	GetLeftStick(0, outLX, outLY);
}

void JoyshockWrapper::GetRightStick(float& outRX, float& outRY) const
{
	GetRightStick(0, outRX, outRY);
}

void JoyshockWrapper::GetTriggers(float& outL, float& outR) const
{
	GetTriggers(0, outL, outR);
}

void JoyshockWrapper::GetAndFlushGyro(float& outX, float& outY, float& outZ)
{
	GetAndFlushGyro(0, outX, outY, outZ);
}

bool JoyshockWrapper::GetXInputButton(int button, int padNum)
{
	if (m_XInputDevices[padNum].connected)
	{
		int _button = 0;

		switch (button)
		{
		case 0x1000: _button = JSMASK_S; break;			// XINPUT_GAMEPAD_A
		case 0x2000: _button = JSMASK_W; break;			// XINPUT_GAMEPAD_B
		case 0x4000: _button = JSMASK_E; break;			// XINPUT_GAMEPAD_X
		case 0x8000: _button = JSMASK_N; break;			// XINPUT_GAMEPAD_Y
		case 0x0100: _button = JSMASK_L; break;			// XINPUT_GAMEPAD_LEFT_SHOULDER
		case 0x0200: _button = JSMASK_R; break;			// XINPUT_GAMEPAD_RIGHT_SHOULDER
		case 0x0010: _button = JSMASK_MINUS; break;		// XINPUT_GAMEPAD_BACK
		case 0x0020: _button = JSMASK_PLUS; break;		// XINPUT_GAMEPAD_START
		case 0x0040: _button = JSMASK_LCLICK; break;	// XINPUT_GAMEPAD_LEFT_THUMB
		case 0x0080: _button = JSMASK_RCLICK; break;	// XINPUT_GAMEPAD_RIGHT_THUMB
		case 0x0001: _button = JSMASK_UP; break;		// XINPUT_GAMEPAD_DPAD_UP
		case 0x0002: _button = JSMASK_DOWN; break;		// XINPUT_GAMEPAD_DPAD_DOWN
		case 0x0004: _button = JSMASK_LEFT; break;		// XINPUT_GAMEPAD_DPAD_LEFT
		case 0x0008: _button = JSMASK_RIGHT; break;		// XINPUT_GAMEPAD_DPAD_RIGHT
		default: return false; // 未対応
		}
		const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
		return (currB & button) != 0;
	}
	return false;
}

bool JoyshockWrapper::GetXInputButtonDown(int button, int padNum)
{
	if (m_XInputDevices[padNum].connected)
	{
		int _button = 0;

		switch (button)
		{
		case 0x1000: _button = JSMASK_S; break;			// XINPUT_GAMEPAD_A
		case 0x2000: _button = JSMASK_W; break;			// XINPUT_GAMEPAD_B
		case 0x4000: _button = JSMASK_E; break;			// XINPUT_GAMEPAD_X
		case 0x8000: _button = JSMASK_N; break;			// XINPUT_GAMEPAD_Y
		case 0x0100: _button = JSMASK_L; break;			// XINPUT_GAMEPAD_LEFT_SHOULDER
		case 0x0200: _button = JSMASK_R; break;			// XINPUT_GAMEPAD_RIGHT_SHOULDER
		case 0x0010: _button = JSMASK_MINUS; break;		// XINPUT_GAMEPAD_BACK
		case 0x0020: _button = JSMASK_PLUS; break;		// XINPUT_GAMEPAD_START
		case 0x0040: _button = JSMASK_LCLICK; break;	// XINPUT_GAMEPAD_LEFT_THUMB
		case 0x0080: _button = JSMASK_RCLICK; break;	// XINPUT_GAMEPAD_RIGHT_THUMB
		case 0x0001: _button = JSMASK_UP; break;		// XINPUT_GAMEPAD_DPAD_UP
		case 0x0002: _button = JSMASK_DOWN; break;		// XINPUT_GAMEPAD_DPAD_DOWN
		case 0x0004: _button = JSMASK_LEFT; break;		// XINPUT_GAMEPAD_DPAD_LEFT
		case 0x0008: _button = JSMASK_RIGHT; break;		// XINPUT_GAMEPAD_DPAD_RIGHT
		default: return false; // 未対応
		}
		const DWORD prevB = m_XInputDevices[padNum].prevXinputState.Gamepad.wButtons;
		const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
		return ((~prevB) & currB & button) != 0;
	}
	return false;
}

bool JoyshockWrapper::GetXInputButtonUp(int button, int padNum)
{
	if (m_XInputDevices[padNum].connected)
	{
		int _button = 0;

		switch (button)
		{
		case 0x1000: _button = JSMASK_S; break;			// XINPUT_GAMEPAD_A
		case 0x2000: _button = JSMASK_W; break;			// XINPUT_GAMEPAD_B
		case 0x4000: _button = JSMASK_E; break;			// XINPUT_GAMEPAD_X
		case 0x8000: _button = JSMASK_N; break;			// XINPUT_GAMEPAD_Y
		case 0x0100: _button = JSMASK_L; break;			// XINPUT_GAMEPAD_LEFT_SHOULDER
		case 0x0200: _button = JSMASK_R; break;			// XINPUT_GAMEPAD_RIGHT_SHOULDER
		case 0x0010: _button = JSMASK_MINUS; break;		// XINPUT_GAMEPAD_BACK
		case 0x0020: _button = JSMASK_PLUS; break;		// XINPUT_GAMEPAD_START
		case 0x0040: _button = JSMASK_LCLICK; break;	// XINPUT_GAMEPAD_LEFT_THUMB
		case 0x0080: _button = JSMASK_RCLICK; break;	// XINPUT_GAMEPAD_RIGHT_THUMB
		case 0x0001: _button = JSMASK_UP; break;		// XINPUT_GAMEPAD_DPAD_UP
		case 0x0002: _button = JSMASK_DOWN; break;		// XINPUT_GAMEPAD_DPAD_DOWN
		case 0x0004: _button = JSMASK_LEFT; break;		// XINPUT_GAMEPAD_DPAD_LEFT
		case 0x0008: _button = JSMASK_RIGHT; break;		// XINPUT_GAMEPAD_DPAD_RIGHT
		default: return false; // 未対応
		}
		const DWORD prevB = m_XInputDevices[padNum].prevXinputState.Gamepad.wButtons;
		const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
		return ((prevB) & (~currB) & button) != 0;
	}
	return false;
}

void JoyshockWrapper::GetXInputLeftStick(float& outLX, float& outLY, int padNum) const
{
	outLX = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbLX) / 32768.0f
		: 0.0f;
	outLY = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbLY) / 32768.0f
		: 0.0f;
	outLX = ApplyDeadZone(outLX, m_StickDeadZone);
	outLY = ApplyDeadZone(outLY, m_StickDeadZone);
}

void JoyshockWrapper::GetXInputRightStick(float& outRX, float& outRY, int padNum) const
{
	outRX = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbRX) / 32768.0f
		: 0.0f;
	outRY = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbRY) / 32768.0f
		: 0.0f;
	outRX = ApplyDeadZone(outRX, m_StickDeadZone);
	outRY = ApplyDeadZone(outRY, m_StickDeadZone);
}

void JoyshockWrapper::GetXInputTriggers(float& outL, float& outR, int padNum) const
{
	outL = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.bLeftTrigger) / 255.0f
		: 0.0f;
	outR = m_XInputDevices[padNum].connected
		? static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.bRightTrigger) / 255.0f
		: 0.0f;
	// Xbox 系は 0/1 の場合あり。しきい値を超えたら 1 とみなす。
	outL = (outL >= m_TriggerThreshold) ? outL : 0.0f;
	outR = (outR >= m_TriggerThreshold) ? outR : 0.0f;
	outL = std::clamp(outL, 0.0f, 1.0f);
	outR = std::clamp(outR, 0.0f, 1.0f);
}

void JoyshockWrapper::RefreshHandleList()
{
	// 既存配列を維持しつつ、JSL から最新ハンドル列を取得して反映する
	std::vector<int> temp;
	temp.resize(64); // 一旦十分なサイズを確保
	int count = JslGetConnectedDeviceHandles(temp.data(), static_cast<int>(temp.size()));
	if (count > static_cast<int>(temp.size()))
	{
		// 必要に応じて再確保
		temp.resize(count);
		count = JslGetConnectedDeviceHandles(temp.data(), static_cast<int>(temp.size()));
	}
	temp.resize(std::max(0, count));

	// 新リストに合わせて m_Devices を再構成
	std::vector<DeviceState> newList;
	newList.reserve(temp.size());

	for (int h : temp)
	{
		// 既存に同ハンドルがあれば状態を引き継ぐ
		auto it = std::find_if(m_Devices.begin(), m_Devices.end(),
			[h](const DeviceState& d)
			{
				return d.handle == h;
			});

		if (it != m_Devices.end())
		{
			newList.push_back(*it);
		}
		else
		{
			DeviceState d{};
			d.handle = h;
			d.connected = true;
			d.prevButtons = 0;
			d.currButtons = 0;
			d.state = JOY_SHOCK_STATE{};
			d.lastGyroX = d.lastGyroY = d.lastGyroZ = 0.0f;

			// 初回サンプルを取得しておく
			d.state = JslGetSimpleState(h);
			d.currButtons = static_cast<uint32_t>(d.state.buttons);

			newList.push_back(d);
		}
	}

	m_Devices.swap(newList);
}

bool JoyshockWrapper::IsValidIndex(int index) const
{
	return (index >= 0) && (index < static_cast<int>(m_Devices.size()));
}

void JoyshockWrapper::RefreshXInputHandleList()
{
	DWORD dwResult;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		::XINPUT_STATE state;
		ZeroMemory(&state, sizeof(::XINPUT_STATE));

		// Simply get the state of the controller from XInput.
		dwResult = XInputGetState(i, &state);

		if (dwResult == ERROR_SUCCESS)
		{
			m_XInputDevices[i].connected = true;
			m_XInputDevices[i].prevXinputState = m_XInputDevices[i].currXinputState;
			m_XInputDevices[i].currXinputState = state;
			m_XInputDevices[i].handle = i;
		}
		else
		{
			m_XInputDevices[i].connected = false;
			m_XInputDevices[i].currXinputState = ::XINPUT_STATE{};
			m_XInputDevices[i].prevXinputState = ::XINPUT_STATE{};
			m_XInputDevices[i].handle = -1;
		}
	}
}
