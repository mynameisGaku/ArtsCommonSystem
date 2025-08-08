#pragma once
#include "Pch.h"

#define ACSM_INPUT

#include <utl/enum/ACSM_Enum.h>

#define ACSM_TRIGGER_THRESHOLD           0.2f
#define ACSM_TRIGGER_RELEASE_THRESHOLD   0.1f
#define ACSM_STICK_THRESHOLD             16384
#define ACSM_STICK_RELEASE_THRESHOLD     8192
#define ACSM_INPUT_BUFFER_FRAME          3  // 猶予フレーム数

///<summary>
/// キーボードキー定義
///</summary>
///<author>藤本樂</author>
enum class ACSM_KeyCode
{
	None,
	A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F', G = 'G', H = 'H',
	I = 'I', J = 'J', K = 'K', L = 'L', M = 'M', N = 'N', O = 'O', P = 'P',
	Q = 'Q', R = 'R', S = 'S', T = 'T', U = 'U', V = 'V', W = 'W', X = 'X',
	Y = 'Y', Z = 'Z', Num0 = '0', Num1 = '1', Num2 = '2', Num3 = '3', Num4 = '4',
	Num5 = '5', Num6 = '6', Num7 = '7', Num8 = '8', Num9 = '9',
	LeftArrow = VK_LEFT, RightArrow = VK_RIGHT, UpArrow = VK_UP, DownArrow = VK_DOWN,
	LShift = VK_LSHIFT, RShift = VK_RSHIFT, LCtrl = VK_LCONTROL, RCtrl = VK_RCONTROL,
	LAlt = VK_LMENU, RAlt = VK_RMENU, Escape = VK_ESCAPE, Space = VK_SPACE,
	Enter = VK_RETURN, Backspace = VK_BACK, Tab = VK_TAB,
	F1 = VK_F1, F2 = VK_F2, F3 = VK_F3, F4 = VK_F4, F5 = VK_F5, F6 = VK_F6,
	F7 = VK_F7, F8 = VK_F8, F9 = VK_F9, F10 = VK_F10, F11 = VK_F11, F12 = VK_F12,
	Insert = VK_INSERT, Delete = VK_DELETE, Home = VK_HOME, End = VK_END,
	PageUp = VK_PRIOR, PageDown = VK_NEXT,
	Semicolon = VK_OEM_1, Equal = VK_OEM_PLUS, Comma = VK_OEM_COMMA,
	Minus = VK_OEM_MINUS, Period = VK_OEM_PERIOD, Slash = VK_OEM_2, Backquote = VK_OEM_3,
	LBracket = VK_OEM_4, Backslash = VK_OEM_5, RBracket = VK_OEM_6, Quote = VK_OEM_7
};

///<summary>
/// マウスボタン定義
///</summary>
///<author>藤本樂</author>
enum class ACSM_MouseButton { None, Left = 0, Right = 1, Middle = 2 };

///<summary>
/// ゲームパッドボタン定義（XInput 準拠）
///</summary>
///<author>藤本樂</author>
enum class ACSM_GamepadButton
{
	None, A, B, X, Y,
	LB, RB, Back, Start,
	LStick, RStick,
	DPadUp, DPadDown, DPadLeft, DPadRight
};

///<summary>
/// トリガー入力定義（左右）
///</summary>
///<author>藤本樂</author>
enum class ACSM_TriggerCode { None, Left, Right };

///<summary>
/// 入力イベント構造体（バッファ用）
///</summary>
///<author>藤本樂</author>
struct InputEvent
{
	std::string virtualKey;  // 仮想キー名
	bool pressed;            // 押下 or 離されたか
	int frame;               // 押されたフレーム番号
};

///<summary>
/// 入力システムのメインクラス
///</summary>
///<author>藤本樂</author>
class ACSM_Input
{
public:
	///<summary>入力システム初期化</summary>
	static void Initialize(HWND hwnd);

	///<summary>毎フレーム更新</summary>
	static void Update();

	//=========================
	// 実キー操作（Keyboard）
	//=========================
	///<summary>キーが押されているか</summary>
	static bool GetKey(ACSM_KeyCode key);
	///<summary>キーが押された瞬間</summary>
	static bool GetKeyDown(ACSM_KeyCode key);
	///<summary>キーが離された瞬間</summary>
	static bool GetKeyUp(ACSM_KeyCode key);

	//=========================
	// マウス操作
	//=========================
	static bool GetMouseButton(ACSM_MouseButton button);
	static bool GetMouseButtonDown(ACSM_MouseButton button);
	static bool GetMouseButtonUp(ACSM_MouseButton button);
	static void GetMousePosition(int& x, int& y);

	//=========================
	// ゲームパッド操作（XInput）
	//=========================
	static bool GetGamepadButton(ACSM_GamepadButton button, int padIndex = 0);
	static bool GetGamepadButtonDown(ACSM_GamepadButton button, int padIndex = 0);
	static bool GetGamepadButtonUp(ACSM_GamepadButton button, int padIndex = 0);
	static float GetTrigger(ACSM_TriggerCode trigger, int padIndex = 0);
	static bool GetTriggerDown(ACSM_TriggerCode trigger, int padIndex = 0);
	static bool GetTriggerUp(ACSM_TriggerCode trigger, int padIndex = 0);
	static float GetStickX(bool rightStick, int padIndex = 0);
	static float GetStickY(bool rightStick, int padIndex = 0);
	static bool GetStickDown(bool rightStick, int padIndex = 0);
	static bool GetStickUp(bool rightStick, int padIndex = 0);
	static bool IsStickDirection(bool rightStick, const std::string& direction, int padIndex = 0);

	//=========================
	// 仮想キー・同時押し対応
	//=========================
	static void LoadVirtualKeysFromJson(const std::string& jsonPath);
	static bool GetVirtualKey(const std::string& virtualKeyName);
	static bool GetVirtualKeyDown(const std::string& virtualKeyName);
	static bool GetVirtualKeyUp(const std::string& virtualKeyName);
	static bool IsCombinationPressed(const std::vector<std::string>& keys);

private:
	static HWND s_hWnd;
	static BYTE m_CurrentKeyState[256];
	static BYTE m_PreviousKeyState[256];

	static std::array<SHORT, 3> m_CurrentMouseState;
	static std::array<SHORT, 3> m_PreviousMouseState;
	static POINT m_MousePosition;

	static ::XINPUT_STATE m_CurrentPadState[4];
	static ::XINPUT_STATE m_PreviousPadState[4];

	static bool m_CurrentKeyStates[256];
	static bool m_PreviousKeyStates[256];
	static bool m_CurrentMouseButtonStates[3];
	static bool m_PreviousMouseButtonStates[3];

	// 仮想キーと実キーのマッピング（複数対応）
	static std::unordered_map<std::string, std::vector<ACSM_KeyCode>> m_VirtualKeyMap_Key;
	static std::unordered_map<std::string, std::vector<ACSM_MouseButton>> m_VirtualKeyMap_Mouse;
	static std::unordered_map<std::string, std::vector<ACSM_GamepadButton>> m_VirtualKeyMap_Gamepad;
	static std::unordered_map<std::string, std::vector<ACSM_TriggerCode>> m_VirtualKeyMap_Trigger;

	// 入力イベントバッファ（同時押し猶予用）
	static std::deque<InputEvent> m_InputBuffer;
	static int m_CurrentFrame;

	static void UpdateKeyboard();
	static void UpdateMouse();
	static void UpdateGamepad();
	static void UpdateInputBuffer();
};
