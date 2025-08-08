#include "ACSM_Input.h"

HWND ACSM_Input::s_hWnd = nullptr;
BYTE ACSM_Input::m_CurrentKeyState[256] = {};
BYTE ACSM_Input::m_PreviousKeyState[256] = {};

std::array<SHORT, 3> ACSM_Input::m_CurrentMouseState = {};
std::array<SHORT, 3> ACSM_Input::m_PreviousMouseState = {};
POINT ACSM_Input::m_MousePosition = {};

::XINPUT_STATE ACSM_Input::m_CurrentPadState[4] = {};
::XINPUT_STATE ACSM_Input::m_PreviousPadState[4] = {};

bool ACSM_Input::m_CurrentKeyStates[256] = {};
bool ACSM_Input::m_PreviousKeyStates[256] = {};
bool ACSM_Input::m_CurrentMouseButtonStates[3] = {};
bool ACSM_Input::m_PreviousMouseButtonStates[3] = {};

std::unordered_map<std::string, std::vector<ACSM_KeyCode>> ACSM_Input::m_VirtualKeyMap_Key;
std::unordered_map<std::string, std::vector<ACSM_MouseButton>> ACSM_Input::m_VirtualKeyMap_Mouse;
std::unordered_map<std::string, std::vector<ACSM_GamepadButton>> ACSM_Input::m_VirtualKeyMap_Gamepad;
std::unordered_map<std::string, std::vector<ACSM_TriggerCode>> ACSM_Input::m_VirtualKeyMap_Trigger;

std::deque<InputEvent> ACSM_Input::m_InputBuffer;
int ACSM_Input::m_CurrentFrame = 0;

void ACSM_Input::Initialize(HWND hwnd)
{
	s_hWnd = hwnd;
	memset(m_CurrentKeyState, 0, sizeof(m_CurrentKeyState));
	memset(m_PreviousKeyState, 0, sizeof(m_PreviousKeyState));
	GetCursorPos(&m_MousePosition);
	m_CurrentFrame = 0;
}

void ACSM_Input::Update()
{
	m_CurrentFrame++;

	memcpy(m_PreviousKeyState, m_CurrentKeyState, sizeof(m_CurrentKeyState));
	memcpy(m_PreviousMouseState.data(), m_CurrentMouseState.data(), sizeof(m_CurrentMouseState));
	memcpy(m_PreviousPadState, m_CurrentPadState, sizeof(m_CurrentPadState));

	UpdateKeyboard();
	UpdateMouse();
	UpdateGamepad();
	UpdateInputBuffer();
}
bool ACSM_Input::GetGamepadButton(ACSM_GamepadButton button, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return false;
	WORD current = m_CurrentPadState[padIndex].Gamepad.wButtons;
	return (current & (1 << static_cast<int>(button))) != 0;
}

bool ACSM_Input::GetGamepadButtonDown(ACSM_GamepadButton button, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return false;
	WORD current = m_CurrentPadState[padIndex].Gamepad.wButtons;
	WORD previous = m_PreviousPadState[padIndex].Gamepad.wButtons;
	return !(previous & (1 << static_cast<int>(button))) && (current & (1 << static_cast<int>(button)));
}

bool ACSM_Input::GetGamepadButtonUp(ACSM_GamepadButton button, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return false;
	WORD current = m_CurrentPadState[padIndex].Gamepad.wButtons;
	WORD previous = m_PreviousPadState[padIndex].Gamepad.wButtons;
	return (previous & (1 << static_cast<int>(button))) && !(current & (1 << static_cast<int>(button)));
}

float ACSM_Input::GetTrigger(ACSM_TriggerCode trigger, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return 0.0f;
	BYTE value = 0;
	switch (trigger)
	{
	case ACSM_TriggerCode::Left:  value = m_CurrentPadState[padIndex].Gamepad.bLeftTrigger; break;
	case ACSM_TriggerCode::Right: value = m_CurrentPadState[padIndex].Gamepad.bRightTrigger; break;
	default: return 0.0f;
	}
	return static_cast<float>(value) / 255.0f;
}

bool ACSM_Input::GetTriggerDown(ACSM_TriggerCode trigger, int padIndex)
{
	float current = GetTrigger(trigger, padIndex);
	float previous = 0.0f;
	switch (trigger)
	{
	case ACSM_TriggerCode::Left:
		previous = static_cast<float>(m_PreviousPadState[padIndex].Gamepad.bLeftTrigger) / 255.0f;
		break;
	case ACSM_TriggerCode::Right:
		previous = static_cast<float>(m_PreviousPadState[padIndex].Gamepad.bRightTrigger) / 255.0f;
		break;
	default: return false;
	}
	return (previous < ACSM_TRIGGER_THRESHOLD && current >= ACSM_TRIGGER_THRESHOLD);
}

bool ACSM_Input::GetTriggerUp(ACSM_TriggerCode trigger, int padIndex)
{
	float current = GetTrigger(trigger, padIndex);
	float previous = 0.0f;
	switch (trigger)
	{
	case ACSM_TriggerCode::Left:
		previous = static_cast<float>(m_PreviousPadState[padIndex].Gamepad.bLeftTrigger) / 255.0f;
		break;
	case ACSM_TriggerCode::Right:
		previous = static_cast<float>(m_PreviousPadState[padIndex].Gamepad.bRightTrigger) / 255.0f;
		break;
	default: return false;
	}
	return (previous >= ACSM_TRIGGER_THRESHOLD && current < ACSM_TRIGGER_RELEASE_THRESHOLD);
}

float ACSM_Input::GetStickX(bool rightStick, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return 0.0f;
	SHORT val = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRX : m_CurrentPadState[padIndex].Gamepad.sThumbLX;
	return static_cast<float>(val) / 32767.0f;
}

float ACSM_Input::GetStickY(bool rightStick, int padIndex)
{
	if (padIndex < 0 || padIndex >= 4) return 0.0f;
	SHORT val = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRY : m_CurrentPadState[padIndex].Gamepad.sThumbLY;
	return static_cast<float>(val) / 32767.0f;
}

bool ACSM_Input::GetStickDown(bool rightStick, int padIndex)
{
	SHORT currentX = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRX : m_CurrentPadState[padIndex].Gamepad.sThumbLX;
	SHORT currentY = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRY : m_CurrentPadState[padIndex].Gamepad.sThumbLY;
	SHORT prevX = rightStick ? m_PreviousPadState[padIndex].Gamepad.sThumbRX : m_PreviousPadState[padIndex].Gamepad.sThumbLX;
	SHORT prevY = rightStick ? m_PreviousPadState[padIndex].Gamepad.sThumbRY : m_PreviousPadState[padIndex].Gamepad.sThumbLY;

	float currLen = sqrtf((float)currentX * (float)currentX + (float)currentY * (float)currentY);
	float prevLen = sqrtf((float)prevX * (float)prevX + (float)prevY * (float)prevY);

	return (prevLen < ACSM_STICK_THRESHOLD && currLen >= ACSM_STICK_THRESHOLD);
}

bool ACSM_Input::GetStickUp(bool rightStick, int padIndex)
{
	SHORT currentX = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRX : m_CurrentPadState[padIndex].Gamepad.sThumbLX;
	SHORT currentY = rightStick ? m_CurrentPadState[padIndex].Gamepad.sThumbRY : m_CurrentPadState[padIndex].Gamepad.sThumbLY;
	SHORT prevX = rightStick ? m_PreviousPadState[padIndex].Gamepad.sThumbRX : m_PreviousPadState[padIndex].Gamepad.sThumbLX;
	SHORT prevY = rightStick ? m_PreviousPadState[padIndex].Gamepad.sThumbRY : m_PreviousPadState[padIndex].Gamepad.sThumbLY;

	float currLen = sqrtf((float)currentX * (float)currentX + (float)currentY * (float)currentY);
	float prevLen = sqrtf((float)prevX * (float)prevX + (float)prevY * (float)prevY);

	return (prevLen >= ACSM_STICK_THRESHOLD && currLen < ACSM_STICK_RELEASE_THRESHOLD);
}
bool ACSM_Input::IsStickDirection(bool rightStick, const std::string& direction, int padIndex)
{
	float x = GetStickX(rightStick, padIndex);
	float y = GetStickY(rightStick, padIndex);

	if (direction == "Up")         return y > 0.5f;
	if (direction == "Down")       return y < -0.5f;
	if (direction == "Left")       return x < -0.5f;
	if (direction == "Right")      return x > 0.5f;
	if (direction == "UpRight")    return y > 0.5f && x > 0.5f;
	if (direction == "UpLeft")     return y > 0.5f && x < -0.5f;
	if (direction == "DownRight")  return y < -0.5f && x > 0.5f;
	if (direction == "DownLeft")   return y < -0.5f && x < -0.5f;

	return false;
}

void ACSM_Input::UpdateInputBuffer()
{
	// バッファの古いイベントを削除
	while (!m_InputBuffer.empty() && m_CurrentFrame - m_InputBuffer.front().frame > ACSM_INPUT_BUFFER_FRAME)
	{
		m_InputBuffer.pop_front();
	}

	// 仮想キーの各マッピングをチェックしてバッファ追加
	for (const auto& [name, keys] : m_VirtualKeyMap_Key)
	{
		for (auto key : keys)
		{
			if (GetKeyDown(key))
			{
				m_InputBuffer.push_back({ name, true, m_CurrentFrame });
			}
			else if (GetKeyUp(key))
			{
				m_InputBuffer.push_back({ name, false, m_CurrentFrame });
			}
		}
	}

	for (const auto& [name, buttons] : m_VirtualKeyMap_Mouse)
	{
		for (auto button : buttons)
		{
			if (GetMouseButtonDown(button))
			{
				m_InputBuffer.push_back({ name, true, m_CurrentFrame });
			}
			else if (GetMouseButtonUp(button))
			{
				m_InputBuffer.push_back({ name, false, m_CurrentFrame });
			}
		}
	}

	for (const auto& [name, buttons] : m_VirtualKeyMap_Gamepad)
	{
		for (auto button : buttons)
		{
			if (GetGamepadButtonDown(button))
			{
				m_InputBuffer.push_back({ name, true, m_CurrentFrame });
			}
			else if (GetGamepadButtonUp(button))
			{
				m_InputBuffer.push_back({ name, false, m_CurrentFrame });
			}
		}
	}

	for (const auto& [name, triggers] : m_VirtualKeyMap_Trigger)
	{
		for (auto trigger : triggers)
		{
			if (GetTriggerDown(trigger))
			{
				m_InputBuffer.push_back({ name, true, m_CurrentFrame });
			}
			else if (GetTriggerUp(trigger))
			{
				m_InputBuffer.push_back({ name, false, m_CurrentFrame });
			}
		}
	}
}

void ACSM_Input::LoadVirtualKeysFromJson(const std::string& jsonPath)
{
	std::ifstream file(jsonPath);
	if (!file.is_open()) return;

	nlohmann::json json;
	file >> json;

	for (auto& [name, bindings] : json.items())
	{
		for (auto& bind : bindings)
		{
			std::string b = bind.get<std::string>();

			if (b.starts_with("Mouse_"))
			{
				// 例: "Mouse_Left" → "Left"
				std::string key = b.substr(strlen("Mouse_"));
				m_VirtualKeyMap_Mouse[name].push_back(
					ACSM_Enum::ToEnum<ACSM_MouseButton>(key, ACSM_MouseButton::None)
				);
			}
			else if (b.starts_with("Gamepad_"))
			{
				std::string key = b.substr(strlen("Gamepad_"));
				m_VirtualKeyMap_Gamepad[name].push_back(
					ACSM_Enum::ToEnum<ACSM_GamepadButton>(key, ACSM_GamepadButton::None)
				);
			}
			else if (b.starts_with("Trigger_"))
			{
				std::string key = b.substr(strlen("Trigger_"));
				m_VirtualKeyMap_Trigger[name].push_back(
					ACSM_Enum::ToEnum<ACSM_TriggerCode>(key, ACSM_TriggerCode::None)
				);
			}
			else
			{
				// それ以外はキーボードキーとみなす（例: "A", "Space", "LeftArrow"）
				m_VirtualKeyMap_Key[name].push_back(
					ACSM_Enum::ToEnum<ACSM_KeyCode>(b, ACSM_KeyCode::None)
				);
			}
		}
	}
}

bool ACSM_Input::GetVirtualKey(const std::string& name)
{
	for (auto key : m_VirtualKeyMap_Key[name])
	{
		if (GetKey(key)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Mouse[name])
	{
		if (GetMouseButton(btn)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Gamepad[name])
	{
		if (GetGamepadButton(btn)) return true;
	}
	for (auto trg : m_VirtualKeyMap_Trigger[name])
	{
		if (GetTrigger(trg) > ACSM_TRIGGER_THRESHOLD) return true;
	}
	return false;
}

bool ACSM_Input::GetVirtualKeyDown(const std::string& name)
{
	for (auto key : m_VirtualKeyMap_Key[name])
	{
		if (GetKeyDown(key)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Mouse[name])
	{
		if (GetMouseButtonDown(btn)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Gamepad[name])
	{
		if (GetGamepadButtonDown(btn)) return true;
	}
	for (auto trg : m_VirtualKeyMap_Trigger[name])
	{
		if (GetTriggerDown(trg)) return true;
	}
	return false;
}

bool ACSM_Input::GetVirtualKeyUp(const std::string& name)
{
	for (auto key : m_VirtualKeyMap_Key[name])
	{
		if (GetKeyUp(key)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Mouse[name])
	{
		if (GetMouseButtonUp(btn)) return true;
	}
	for (auto btn : m_VirtualKeyMap_Gamepad[name])
	{
		if (GetGamepadButtonUp(btn)) return true;
	}
	for (auto trg : m_VirtualKeyMap_Trigger[name])
	{
		if (GetTriggerUp(trg)) return true;
	}
	return false;
}

bool ACSM_Input::IsCombinationPressed(const std::vector<std::string>& keys)
{
	for (const auto& name : keys)
	{
		bool found = false;
		for (const auto& event : m_InputBuffer)
		{
			if (event.virtualKey == name && event.pressed)
			{
				found = true;
				break;
			}
		}
		if (!found) return false;
	}
	return true;
}
//===============================
// キーボード更新
//===============================
void ACSM_Input::UpdateKeyboard()
{
	memcpy(m_PreviousKeyStates, m_CurrentKeyStates, sizeof(m_CurrentKeyStates));

	for (int i = 0; i < 256; ++i)
	{
		m_CurrentKeyStates[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
	}
}

//===============================
// マウス更新
//===============================
void ACSM_Input::UpdateMouse()
{
	memcpy(m_PreviousMouseButtonStates, m_CurrentMouseButtonStates, sizeof(m_CurrentMouseButtonStates));

	m_CurrentMouseButtonStates[static_cast<int>(ACSM_MouseButton::Left)] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	m_CurrentMouseButtonStates[static_cast<int>(ACSM_MouseButton::Right)] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
	m_CurrentMouseButtonStates[static_cast<int>(ACSM_MouseButton::Middle)] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
}

//===============================
// ゲームパッド更新（XInput）
//===============================
void ACSM_Input::UpdateGamepad()
{
	memcpy(m_PreviousPadState, m_CurrentPadState, sizeof(m_CurrentPadState));

	for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		ZeroMemory(&m_CurrentPadState[i], sizeof(::XINPUT_STATE));
		XInputGetState(i, &m_CurrentPadState[i]);
	}
}

//===============================
// キー取得系
//===============================
bool ACSM_Input::GetKey(ACSM_KeyCode key)
{
	return m_CurrentKeyStates[static_cast<int>(key)];
}

bool ACSM_Input::GetKeyDown(ACSM_KeyCode key)
{
	int index = static_cast<int>(key);
	return !m_PreviousKeyStates[index] && m_CurrentKeyStates[index];
}

bool ACSM_Input::GetKeyUp(ACSM_KeyCode key)
{
	int index = static_cast<int>(key);
	return m_PreviousKeyStates[index] && !m_CurrentKeyStates[index];
}

//===============================
// マウスボタン取得系
//===============================
bool ACSM_Input::GetMouseButton(ACSM_MouseButton button)
{
	return m_CurrentMouseButtonStates[static_cast<int>(button)];
}

bool ACSM_Input::GetMouseButtonDown(ACSM_MouseButton button)
{
	int index = static_cast<int>(button);
	return !m_PreviousMouseButtonStates[index] && m_CurrentMouseButtonStates[index];
}

bool ACSM_Input::GetMouseButtonUp(ACSM_MouseButton button)
{
	int index = static_cast<int>(button);
	return m_PreviousMouseButtonStates[index] && !m_CurrentMouseButtonStates[index];
}

void ACSM_Input::GetMousePosition(int& x, int& y)
{
	POINT pos;
	GetCursorPos(&pos);
	ScreenToClient(s_hWnd, &pos);
	x = pos.x;
	y = pos.y;
}
