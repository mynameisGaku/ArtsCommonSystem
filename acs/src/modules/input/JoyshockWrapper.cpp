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
	, m_StickDeadZone(0.10f)
	, m_TriggerThreshold(0.10f)
{
	m_XInputDevices.resize(XUSER_MAX_COUNT);
	for (auto& d : m_XInputDevices)
	{
		d.connected = false;
		d.currXinputState = ::XINPUT_STATE{};
		d.prevXinputState = ::XINPUT_STATE{};
		d.handle = -1;
	}
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

	const int connected = JslConnectDevices();
	(void)connected;

	RefreshHandleList();

	RefreshXInputHandleList();

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
	JslDisconnectAndDisposeAll();

	// XInput を初期化状態へ
	for (auto& d : m_XInputDevices)
	{
		d.connected = false;
		d.currXinputState = ::XINPUT_STATE{};
		d.prevXinputState = ::XINPUT_STATE{};
		d.handle = -1;
	}

	m_Initialized = false;
}

void JoyshockWrapper::RescanDevices()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (!m_Initialized)
	{
		return;
	}

	JslConnectDevices();
	RefreshHandleList();

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

		dev.state = JslGetSimpleState(dev.handle);
		dev.currButtons = static_cast<uint32_t>(dev.state.buttons);

		float gx = 0.0f, gy = 0.0f, gz = 0.0f;
		JslGetAndFlushAccumulatedGyro(dev.handle, gx, gy, gz);
		dev.lastGyroX = gx;
		dev.lastGyroY = gy;
		dev.lastGyroZ = gz;
	}

	RefreshXInputHandleList();
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
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()))
	{
		return false;
	}
	if (!m_XInputDevices[padNum].connected)
	{
		return false;
	}
	const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
	return (currB & static_cast<DWORD>(button)) != 0;
}

bool JoyshockWrapper::GetXInputButtonDown(int button, int padNum)
{
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()))
	{
		return false;
	}
	if (!m_XInputDevices[padNum].connected)
	{
		return false;
	}
	const DWORD prevB = m_XInputDevices[padNum].prevXinputState.Gamepad.wButtons;
	const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
	return ((~prevB) & currB & static_cast<DWORD>(button)) != 0;
}

bool JoyshockWrapper::GetXInputButtonUp(int button, int padNum)
{
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()))
	{
		return false;
	}
	if (!m_XInputDevices[padNum].connected)
	{
		return false;
	}
	const DWORD prevB = m_XInputDevices[padNum].prevXinputState.Gamepad.wButtons;
	const DWORD currB = m_XInputDevices[padNum].currXinputState.Gamepad.wButtons;
	return ((prevB) & (~currB) & static_cast<DWORD>(button)) != 0;
}

void JoyshockWrapper::GetXInputLeftStick(float& outLX, float& outLY, int padNum) const
{
	outLX = 0.0f;
	outLY = 0.0f;
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()) || !m_XInputDevices[padNum].connected)
	{
		return;
	}
	outLX = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbLX) / 32768.0f;
	outLY = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbLY) / 32768.0f;
	outLX = ApplyDeadZone(outLX, m_StickDeadZone);
	outLY = ApplyDeadZone(outLY, m_StickDeadZone);
}

void JoyshockWrapper::GetXInputRightStick(float& outRX, float& outRY, int padNum) const
{
	outRX = 0.0f;
	outRY = 0.0f;
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()) || !m_XInputDevices[padNum].connected)
	{
		return;
	}
	outRX = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbRX) / 32768.0f;
	outRY = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.sThumbRY) / 32768.0f;
	outRX = ApplyDeadZone(outRX, m_StickDeadZone);
	outRY = ApplyDeadZone(outRY, m_StickDeadZone);
}

void JoyshockWrapper::GetXInputTriggers(float& outL, float& outR, int padNum) const
{
	outL = 0.0f;
	outR = 0.0f;
	if (padNum < 0 || padNum >= static_cast<int>(m_XInputDevices.size()) || !m_XInputDevices[padNum].connected)
	{
		return;
	}
	outL = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.bLeftTrigger) / 255.0f;
	outR = static_cast<float>(m_XInputDevices[padNum].currXinputState.Gamepad.bRightTrigger) / 255.0f;

	outL = (outL >= m_TriggerThreshold) ? outL : 0.0f;
	outR = (outR >= m_TriggerThreshold) ? outR : 0.0f;

	outL = std::clamp(outL, 0.0f, 1.0f);
	outR = std::clamp(outR, 0.0f, 1.0f);
}

void JoyshockWrapper::RefreshHandleList()
{
	std::vector<int> temp;
	temp.resize(64);
	int count = JslGetConnectedDeviceHandles(temp.data(), static_cast<int>(temp.size()));
	if (count > static_cast<int>(temp.size()))
	{
		temp.resize(count);
		count = JslGetConnectedDeviceHandles(temp.data(), static_cast<int>(temp.size()));
	}
	temp.resize(std::max(0, count));

	std::vector<DeviceState> newList;
	newList.reserve(temp.size());

	for (int h : temp)
	{
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
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		::XINPUT_STATE state{};
		const DWORD rv = XInputGetState(i, &state);

		auto& xd = m_XInputDevices[i];

		if (rv == ERROR_SUCCESS)
		{
			xd.connected = true;
			xd.prevXinputState = xd.currXinputState;
			xd.currXinputState = state;
			xd.handle = static_cast<int>(i);
		}
		else
		{
			xd.connected = false;
			xd.prevXinputState = ::XINPUT_STATE{};
			xd.currXinputState = ::XINPUT_STATE{};
			xd.handle = -1;
		}
	}
}
