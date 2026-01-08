#include "ACSM_Input.h"

#include "JoyshockWrapper.h"
#include "KeyWrapper.h"
#include <Xinput.h> // ★ 追加

namespace
{
	bool TryParsePadName(const std::string& name, uint32_t& outMask)
	{
		std::unordered_map<std::string, uint32_t>* kPadMap =
			new std::unordered_map<std::string, uint32_t>{
				{ "Instance", JSMASK_S }, { "PAD_S", JSMASK_S },
				{ "W", JSMASK_W }, { "PAD_W", JSMASK_W },
				{ "E", JSMASK_E }, { "PAD_E", JSMASK_E },
				{ "N", JSMASK_N }, { "PAD_N", JSMASK_N },
				{ "ZL", JSMASK_ZL }, { "PAD_ZL", JSMASK_ZL },
				{ "ZR", JSMASK_ZR }, { "PAD_ZR", JSMASK_ZR },
				{ "L", JSMASK_L }, { "PAD_L", JSMASK_L },
				{ "R", JSMASK_R }, { "PAD_R", JSMASK_R },
				{ "MINUS", JSMASK_MINUS }, { "PAD_MINUS", JSMASK_MINUS },
				{ "PLUS", JSMASK_PLUS }, { "PAD_PLUS", JSMASK_PLUS },
				{ "LCLICK", JSMASK_LCLICK }, { "PAD_LCLICK", JSMASK_LCLICK },
				{ "RCLICK", JSMASK_RCLICK }, { "PAD_RCLICK", JSMASK_RCLICK },
				{ "HOME", JSMASK_HOME }, { "PAD_HOME", JSMASK_HOME },
				{ "CAPTURE", JSMASK_CAPTURE }, { "PAD_CAPTURE", JSMASK_CAPTURE },
		};

		auto it = kPadMap->find(name);
		if (it == kPadMap->end())
		{
			delete kPadMap;
			return false;
		}
		outMask = it->second;

		delete kPadMap;
		return true;
	}

	bool TryParseXInputName(const std::string& name, int& outXBtn)
	{
		static const std::unordered_map<std::string, int> kXMap
		{
			{ "A",			XINPUT_GAMEPAD_A },
			{ "B",			XINPUT_GAMEPAD_B },
			{ "X",			XINPUT_GAMEPAD_X },
			{ "Y",			XINPUT_GAMEPAD_Y },
			{ "LB",			XINPUT_GAMEPAD_LEFT_SHOULDER },
			{ "RB",			XINPUT_GAMEPAD_RIGHT_SHOULDER },
			{ "BACK",		XINPUT_GAMEPAD_BACK },
			{ "START",		XINPUT_GAMEPAD_START },
			{ "LS",			XINPUT_GAMEPAD_LEFT_THUMB },
			{ "RS",			XINPUT_GAMEPAD_RIGHT_THUMB },
			{ "DPAD_UP",	XINPUT_GAMEPAD_DPAD_UP },
			{ "DPAD_DOWN",	XINPUT_GAMEPAD_DPAD_DOWN },
			{ "DPAD_LEFT",	XINPUT_GAMEPAD_DPAD_LEFT },
			{ "DPAD_RIGHT",	XINPUT_GAMEPAD_DPAD_RIGHT },
		};
		auto it = kXMap.find(name);
		if (it == kXMap.end())
		{
			return false;
		}
		outXBtn = it->second;
		return true;
	}
}

#ifdef ACSM_DXLIB
namespace ACS_Mouse
{
	std::unordered_map<MouseButton, bool>* g_pButtons = {};
	std::unordered_map<MouseButton, bool>* g_pButtonsPrev = {};

	void Initialize()
	{
		g_pButtons = new std::unordered_map<MouseButton, bool>;
		g_pButtonsPrev = new std::unordered_map<MouseButton, bool>;
	}

	void Destroy()
	{
		delete g_pButtons;
		delete g_pButtonsPrev;
	}

	void Update()
	{
		*g_pButtonsPrev = *g_pButtons;
		int mouseState = GetMouseInput();
		(*g_pButtons)[ACSM_Input::MouseButton::Left] = (mouseState & MOUSE_INPUT_LEFT) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::Right] = (mouseState & MOUSE_INPUT_RIGHT) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::Middle] = (mouseState & MOUSE_INPUT_MIDDLE) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::X1] = (mouseState & MOUSE_INPUT_4) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::X2] = (mouseState & MOUSE_INPUT_5) != 0;
	}
}
#endif

namespace ASC_VirtualKey
{
	enum class DeviceType
	{
		None,
		Keyboard,
		Pad,   // JoyShockLibrary
		XPad   // XInput
	};

	struct Binding
	{
		DeviceType type;
		unsigned int code;   // Keyboard: VK(0x00..0xFF), Pad: JSMASK_*, XPad: XINPUT_GAMEPAD_*
		int deviceIndex;     // パッド index
	};

	static std::unordered_map<std::string, std::vector<Binding>>* s_pBindings;
	static std::unordered_map<std::string, bool>* s_pCurr;
	static std::unordered_map<std::string, bool>* s_pPrev;

	void Initialize()
	{
		s_pBindings = new std::unordered_map<std::string, std::vector<Binding>>;
		s_pCurr = new std::unordered_map<std::string, bool>;
		s_pPrev = new std::unordered_map<std::string, bool>;
	}

	void Destroy()
	{
		delete s_pBindings;
		delete s_pCurr;
		delete s_pPrev;
	}

	static bool PollBinding(const Binding& b)
	{
		if (b.type == DeviceType::Keyboard)
		{
			return KeyWrapper::Instance().GetKey(static_cast<unsigned char>(b.code));
		}
		else if (b.type == DeviceType::Pad)
		{
			return JoyshockWrapper::Instance().GetRawButton(b.deviceIndex, b.code);
		}
		else if (b.type == DeviceType::XPad)
		{
			return JoyshockWrapper::Instance().GetXInputButton(static_cast<int>(b.code), b.deviceIndex);
		}
		return false;
	}

	void Update()
	{
		*s_pPrev = *s_pCurr;

		for (auto& kv : (*s_pBindings))
		{
			const std::string& name = kv.first;
			const auto& vec = kv.second;

			bool pressed = false;
			for (const auto& b : vec)
			{
				if (PollBinding(b))
				{
					pressed = true;
					break;
				}
			}
			(*s_pCurr)[name] = pressed;
		}
	}

	bool Subscribe(const std::string& name, const Binding& b)
	{
		if (name.empty())
		{
			return false;
		}
		if (b.type == DeviceType::Keyboard)
		{
			if (b.code > 0xFF) { return false; }
		}
		else
		{
			if (b.code == 0) { return false; }
		}

		auto& vec = (*s_pBindings)[name];
		auto it = std::find_if(vec.begin(), vec.end(),
			[&](const Binding& x)
			{
				return x.type == b.type && x.code == b.code && x.deviceIndex == b.deviceIndex;
			});
		if (it == vec.end())
		{
			vec.push_back(b);
		}

		s_pCurr->emplace(name, false);
		s_pPrev->emplace(name, false);
		return true;
	}

	bool Get(const std::string& name)
	{
		auto it = s_pCurr->find(name);
		return (it != s_pCurr->end()) ? it->second : false;
	}

	bool GetDown(const std::string& name)
	{
		const bool now = Get(name);
		const bool prev = (s_pPrev->find(name) != s_pPrev->end()) ? (*s_pPrev)[name] : false;
		return now && !prev;
	}

	bool GetUp(const std::string& name)
	{
		const bool now = Get(name);
		const bool prev = (s_pPrev->find(name) != s_pPrev->end()) ? (*s_pPrev)[name] : false;
		return (!now) && prev;
	}
}

void ACSM_Input::Initialize()
{
	JoyshockWrapper::Instance().Initialize();
	ACS_Mouse::Initialize();
	ASC_VirtualKey::Initialize();
}
void ACSM_Input::Destroy()
{
	ACS_Mouse::Destroy();
	ASC_VirtualKey::Destroy();
	JoyshockWrapper::Instance().Destroy();
	KeyWrapper::Instance().Destroy();
}
void ACSM_Input::Update()
{
	JoyshockWrapper::Instance().Update();
	KeyWrapper::Instance().Update();
	ACS_Mouse::Update();
	ASC_VirtualKey::Update();
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

bool ACSM_Input::GetButton(int button, int padNum)
{
	return JoyshockWrapper::Instance().GetRawButton(padNum, button);
}
bool ACSM_Input::GetButtonDown(int button, int padNum)
{
	return JoyshockWrapper::Instance().GetRawButtonDown(padNum, button);
}
bool ACSM_Input::GetButtonUp(int button, int padNum)
{
	return JoyshockWrapper::Instance().GetRawButtonUp(padNum, button);
}
void ACSM_Input::GetLeftStick(float& outLX, float& outLY, int padNum)
{
	JoyshockWrapper::Instance().GetLeftStick(padNum, outLX, outLY);
}
void ACSM_Input::GetRightStick(float& outRX, float& outRY, int padNum)
{
	JoyshockWrapper::Instance().GetRightStick(padNum, outRX, outRY);
}
void ACSM_Input::GetTriggers(float& outL, float& outR, int padNum)
{
	JoyshockWrapper::Instance().GetTriggers(padNum, outL, outR);
}

bool ACSM_Input::GetXInputButton(int xinputButton, int padNum)
{
	return JoyshockWrapper::Instance().GetXInputButton(xinputButton, padNum);
}
bool ACSM_Input::GetXInputButtonDown(int xinputButton, int padNum)
{
	return JoyshockWrapper::Instance().GetXInputButtonDown(xinputButton, padNum);
}
bool ACSM_Input::GetXInputButtonUp(int xinputButton, int padNum)
{
	return JoyshockWrapper::Instance().GetXInputButtonUp(xinputButton, padNum);
}
void ACSM_Input::GetXInputLeftStick(float& outLX, float& outLY, int padNum)
{
	JoyshockWrapper::Instance().GetXInputLeftStick(outLX, outLY, padNum);
}
void ACSM_Input::GetXInputRightStick(float& outRX, float& outRY, int padNum)
{
	JoyshockWrapper::Instance().GetXInputRightStick(outRX, outRY, padNum);
}
void ACSM_Input::GetXInputTriggers(float& outL, float& outR, int padNum)
{
	JoyshockWrapper::Instance().GetXInputTriggers(outL, outR, padNum);
}

bool ACSM_Input::SubscribeVirtualKey(const std::string& key, int button)
{
	using namespace ASC_VirtualKey;

	if (button <= 0xFF)
	{
		Binding b{};
		b.type = DeviceType::Keyboard;
		b.code = static_cast<unsigned int>(button);
		b.deviceIndex = 0;
		return Subscribe(key, b);
	}
	else
	{
		// ここは JSL の JSMASK_* / もしくは XINPUT_GAMEPAD_* の両方が来うる
		// 利用側（JSON購読）でデバイス種別を分けるため、直接ここでは Pad に寄せない。
		// 既定は JSL（Pad）として扱うユーティリティ。XPad は JSON 側で明示する。
		Binding b{};
		b.type = DeviceType::Pad; // 既定（JSL）
		b.code = static_cast<unsigned int>(button);
		b.deviceIndex = 0;
		return Subscribe(key, b);
	}
}

bool ACSM_Input::GetVirtualKey(const std::string& key)
{
	return ASC_VirtualKey::Get(key);
}
bool ACSM_Input::GetVirtualKeyDown(const std::string& key)
{
	return ASC_VirtualKey::GetDown(key);
}
bool ACSM_Input::GetVirtualKeyUp(const std::string& key)
{
	return ASC_VirtualKey::GetUp(key);
}

void ACSM_Input::SubscribeVirtualKeyFromJson(const std::string& path)
{
	using namespace ASC_VirtualKey;
	using JSON = nlohmann::json;

	JSON json;
	std::ifstream file(path);
	file >> json;

	auto keys = json["Keys"];

	for (auto& key : keys)
	{
		std::string virtualKeyName = key["VirtualKeyName"];
		auto values = key["Values"];
		for (auto& value : values)
		{
			std::string deviceName = value["Device"];
			std::string keyName = value["KeyName"];

			// 既存の ToEnum を使いつつ、未対応なら文字列で XPad を拾う
			DeviceType device = ACSU_Enum::ToEnum<DeviceType>(deviceName, DeviceType::None);
			if (device == DeviceType::None)
			{
				if (deviceName == "XPad" || deviceName == "xpad" || deviceName == "XINPUT")
				{
					device = DeviceType::XPad;
				}
				else if (deviceName == "Pad" || deviceName == "pad")
				{
					device = DeviceType::Pad;
				}
				else if (deviceName == "Keyboard" || deviceName == "keyboard")
				{
					device = DeviceType::Keyboard;
				}
			}

			switch (device)
			{
			case DeviceType::Keyboard:
			{
				KeyCode keyCode = ACSU_Enum::ToEnum(keyName, KeyCode::NONE);
				if (keyCode != KeyCode::NONE)
				{
					Binding b{};
					b.type = DeviceType::Keyboard;
					b.code = static_cast<unsigned int>(keyCode);
					b.deviceIndex = 0;
					Subscribe(virtualKeyName, b);
				}
			}
			break;

			case DeviceType::Pad: // JoyShock
			{
				uint32_t mask = 0;
				if (TryParsePadName(keyName, mask))
				{
					Binding b{};
					b.type = DeviceType::Pad;
					b.code = mask;
					b.deviceIndex = value.value("PadIndex", 0);
					Subscribe(virtualKeyName, b);
				}
			}
			break;

			case DeviceType::XPad: // XInput
			{
				int xbtn = 0;
				if (TryParseXInputName(keyName, xbtn))
				{
					Binding b{};
					b.type = DeviceType::XPad;
					b.code = static_cast<unsigned int>(xbtn);
					b.deviceIndex = value.value("PadIndex", 0);
					Subscribe(virtualKeyName, b);
				}
			}
			break;

			default:
				break;
			}
		}
	}
}

#ifdef ACSM_DXLIB
POINT ACSM_Input::GetMousePoint()
{
	POINT pos{};
	int tmpX, tmpY;
	DxLib::GetMousePoint(&tmpX, &tmpY);
	pos = { (LONG)tmpX, (LONG)tmpY };
	return pos;
}
bool ACSM_Input::GetMouseButton(MouseButton button)
{
	return (*ACS_Mouse::g_pButtons)[button];
}
bool ACSM_Input::GetMouseButtonDown(MouseButton button)
{
	return (*ACS_Mouse::g_pButtons)[button] && !(*ACS_Mouse::g_pButtonsPrev)[button];
}
bool ACSM_Input::GetMouseButtonUp(MouseButton button)
{
	return !(*ACS_Mouse::g_pButtons)[button] && (*ACS_Mouse::g_pButtonsPrev)[button];
}
void ACSM_Input::SetMousePosition(int x, int y)
{
	SetMousePoint(x, y);
}
void ACSM_Input::SetMouseVisible(bool visible)
{
	SetMouseDispFlag(visible ? TRUE : FALSE);
}
#endif
