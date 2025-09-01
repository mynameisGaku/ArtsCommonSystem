#pragma once
#include "Pch.h"

// Windows仮想キーを 0x00 ～ 0xFF の256個すべて列挙。
// 既知のキーは公式に準拠した名称、未割当・予約は UNASSIGNED_xx / OEM_SPECIFIC_xx などで穴埋め。
// 値は unsigned char に収まります（KeyWrapper の引数に static_cast<unsigned char> で渡せます）。
enum KeyCode : unsigned char
{
	NONE = 0x00,  // Unassigned/Reserved
	MLBUTTON = 0x01,  // Left mouse button
	MRBUTTON = 0x02,  // Right mouse button
	CANCEL = 0x03,  // Control-break processing
	MMBUTTON = 0x04,  // Middle mouse button
	MXBUTTON1 = 0x05,  // X1 mouse button
	MXBUTTON2 = 0x06,  // X2 mouse button
	UNASSIGNED_07 = 0x07,  // Unassigned/Reserved
	BACK = 0x08,  // Backspace
	TAB = 0x09,  // Tab
	UNASSIGNED_0A = 0x0A,  // Unassigned/Reserved
	UNASSIGNED_0B = 0x0B,  // Unassigned/Reserved
	CLEAR = 0x0C,  // Clear
	RETURN = 0x0D,  // Enter
	UNASSIGNED_0E = 0x0E,  // Unassigned/Reserved
	UNASSIGNED_0F = 0x0F,  // Unassigned/Reserved
	SHIFT = 0x10,  // Shift
	CONTROL = 0x11,  // Ctrl
	MENU = 0x12,  // Alt
	PAUSE = 0x13,  // Pause
	CAPITAL = 0x14,  // Caps Lock
	KANA = 0x15,  // Kana/Hangul mode
	IME_ON = 0x16,  // IME On
	JUNJA = 0x17,  // Junja
	FINAL = 0x18,  // Final
	HANJA = 0x19,  // Hanja/Kanji
	IME_OFF = 0x1A,  // IME Off
	ESCAPE = 0x1B,  // Escape
	CONVERT = 0x1C,  // Convert
	NONCONVERT = 0x1D,  // NonConvert
	ACCEPT = 0x1E,  // Accept
	MODECHANGE = 0x1F,  // Mode Change
	SPACE = 0x20,  // Space
	PRIOR = 0x21,  // Page Up
	NEXT = 0x22,  // Page Down
	END = 0x23,  // End
	HOME = 0x24,  // Home
	LEFT = 0x25,  // Left Arrow
	UP = 0x26,  // Up Arrow
	RIGHT = 0x27,  // Right Arrow
	DOWN = 0x28,  // Down Arrow
	SELECT = 0x29,  // Select
	PRINT = 0x2A,  // Print
	EXECUTE = 0x2B,  // Execute
	SNAPSHOT = 0x2C,  // Print Screen
	INSERT = 0x2D,  // Insert
	DELETE_KEY = 0x2E,  // Delete
	HELP = 0x2F,  // Help
	D0 = 0x30,  // '0'
	D1 = 0x31,  // '1'
	D2 = 0x32,  // '2'
	D3 = 0x33,  // '3'
	D4 = 0x34,  // '4'
	D5 = 0x35,  // '5'
	D6 = 0x36,  // '6'
	D7 = 0x37,  // '7'
	D8 = 0x38,  // '8'
	D9 = 0x39,  // '9'
	UNASSIGNED_3A = 0x3A,  // Unassigned/Reserved
	UNASSIGNED_3B = 0x3B,  // Unassigned/Reserved
	UNASSIGNED_3C = 0x3C,  // Unassigned/Reserved
	UNASSIGNED_3D = 0x3D,  // Unassigned/Reserved
	UNASSIGNED_3E = 0x3E,  // Unassigned/Reserved
	UNASSIGNED_3F = 0x3F,  // Unassigned/Reserved
	UNASSIGNED_40 = 0x40,  // Unassigned/Reserved
	A = 0x41,  // 'A'
	B = 0x42,  // 'B'
	C = 0x43,  // 'C'
	D = 0x44,  // 'D'
	E = 0x45,  // 'E'
	F = 0x46,  // 'F'
	G = 0x47,  // 'G'
	H = 0x48,  // 'H'
	I = 0x49,  // 'I'
	J = 0x4A,  // 'J'
	K = 0x4B,  // 'K'
	L = 0x4C,  // 'L'
	M = 0x4D,  // 'M'
	N = 0x4E,  // 'N'
	O = 0x4F,  // 'O'
	P = 0x50,  // 'P'
	Q = 0x51,  // 'Q'
	R = 0x52,  // 'R'
	S = 0x53,  // 'S'
	T = 0x54,  // 'T'
	U = 0x55,  // 'U'
	V = 0x56,  // 'V'
	W = 0x57,  // 'W'
	X = 0x58,  // 'X'
	Y = 0x59,  // 'Y'
	Z = 0x5A,  // 'Z'
	LWIN = 0x5B,  // Left Windows
	RWIN = 0x5C,  // Right Windows
	APPS = 0x5D,  // Application/Menu
	UNASSIGNED_5E = 0x5E,  // Unassigned/Reserved
	SLEEP = 0x5F,  // Sleep
	NUMPAD0 = 0x60,  // Numpad 0
	NUMPAD1 = 0x61,  // Numpad 1
	NUMPAD2 = 0x62,  // Numpad 2
	NUMPAD3 = 0x63,  // Numpad 3
	NUMPAD4 = 0x64,  // Numpad 4
	NUMPAD5 = 0x65,  // Numpad 5
	NUMPAD6 = 0x66,  // Numpad 6
	NUMPAD7 = 0x67,  // Numpad 7
	NUMPAD8 = 0x68,  // Numpad 8
	NUMPAD9 = 0x69,  // Numpad 9
	NUM_MULTIPLY = 0x6A,  // Numpad *
	NUM_ADD = 0x6B,  // Numpad +
	NUM_SEPARATOR = 0x6C,  // Numpad Separator
	NUM_SUBTRACT = 0x6D,  // Numpad -
	NUM_DECIMAL = 0x6E,  // Numpad .
	NUM_DIVIDE = 0x6F,  // Numpad /
	F1 = 0x70,  // F1
	F2 = 0x71,  // F2
	F3 = 0x72,  // F3
	F4 = 0x73,  // F4
	F5 = 0x74,  // F5
	F6 = 0x75,  // F6
	F7 = 0x76,  // F7
	F8 = 0x77,  // F8
	F9 = 0x78,  // F9
	F10 = 0x79,  // F10
	F11 = 0x7A,  // F11
	F12 = 0x7B,  // F12
	F13 = 0x7C,  // F13
	F14 = 0x7D,  // F14
	F15 = 0x7E,  // F15
	F16 = 0x7F,  // F16
	F17 = 0x80,  // F17
	F18 = 0x81,  // F18
	F19 = 0x82,  // F19
	F20 = 0x83,  // F20
	F21 = 0x84,  // F21
	F22 = 0x85,  // F22
	F23 = 0x86,  // F23
	F24 = 0x87,  // F24
	UNASSIGNED_88 = 0x88,  // Unassigned/Reserved
	UNASSIGNED_89 = 0x89,  // Unassigned/Reserved
	UNASSIGNED_8A = 0x8A,  // Unassigned/Reserved
	UNASSIGNED_8B = 0x8B,  // Unassigned/Reserved
	UNASSIGNED_8C = 0x8C,  // Unassigned/Reserved
	UNASSIGNED_8D = 0x8D,  // Unassigned/Reserved
	UNASSIGNED_8E = 0x8E,  // Unassigned/Reserved
	UNASSIGNED_8F = 0x8F,  // Unassigned/Reserved
	NUMLOCK = 0x90,  // Num Lock
	SCROLL = 0x91,  // Scroll Lock
	UNASSIGNED_92 = 0x92,  // Unassigned/Reserved
	UNASSIGNED_93 = 0x93,  // Unassigned/Reserved
	UNASSIGNED_94 = 0x94,  // Unassigned/Reserved
	UNASSIGNED_95 = 0x95,  // Unassigned/Reserved
	UNASSIGNED_96 = 0x96,  // Unassigned/Reserved
	UNASSIGNED_97 = 0x97,  // Unassigned/Reserved
	UNASSIGNED_98 = 0x98,  // Unassigned/Reserved
	UNASSIGNED_99 = 0x99,  // Unassigned/Reserved
	UNASSIGNED_9A = 0x9A,  // Unassigned/Reserved
	UNASSIGNED_9B = 0x9B,  // Unassigned/Reserved
	UNASSIGNED_9C = 0x9C,  // Unassigned/Reserved
	UNASSIGNED_9D = 0x9D,  // Unassigned/Reserved
	UNASSIGNED_9E = 0x9E,  // Unassigned/Reserved
	UNASSIGNED_9F = 0x9F,  // Unassigned/Reserved
	LSHIFT = 0xA0,  // Left Shift
	RSHIFT = 0xA1,  // Right Shift
	LCONTROL = 0xA2,  // Left Ctrl
	RCONTROL = 0xA3,  // Right Ctrl
	LMENU = 0xA4,  // Left Alt
	RMENU = 0xA5,  // Right Alt
	BROWSER_BACK = 0xA6,  // Browser Back
	BROWSER_FORWARD = 0xA7,  // Browser Forward
	BROWSER_REFRESH = 0xA8,  // Browser Refresh
	BROWSER_STOP = 0xA9,  // Browser Stop
	BROWSER_SEARCH = 0xAA,  // Browser Search
	BROWSER_FAVORITES = 0xAB,  // Browser Favorites
	BROWSER_HOME = 0xAC,  // Browser Home
	VOLUME_MUTE = 0xAD,  // Volume Mute
	VOLUME_DOWN = 0xAE,  // Volume Down
	VOLUME_UP = 0xAF,  // Volume Up
	MEDIA_NEXT_TRACK = 0xB0,  // Media Next
	MEDIA_PREV_TRACK = 0xB1,  // Media Prev
	MEDIA_STOP = 0xB2,  // Media Stop
	MEDIA_PLAY_PAUSE = 0xB3,  // Media Play/Pause
	LAUNCH_MAIL = 0xB4,  // Launch Mail
	LAUNCH_MEDIA_SELECT = 0xB5,  // Launch Media Select
	LAUNCH_APP1 = 0xB6,  // Launch App1
	LAUNCH_APP2 = 0xB7,  // Launch App2
	UNASSIGNED_B8 = 0xB8,  // Unassigned/Reserved
	UNASSIGNED_B9 = 0xB9,  // Unassigned/Reserved
	OEM_1 = 0xBA,  // OEM 1 (;:)
	OEM_PLUS = 0xBB,  // OEM Plus (+)
	OEM_COMMA = 0xBC,  // OEM Comma (,)
	OEM_MINUS = 0xBD,  // OEM Minus (-)
	OEM_PERIOD = 0xBE,  // OEM Period (.)
	OEM_2 = 0xBF,  // OEM 2 (/? )
	OEM_3 = 0xC0,  // OEM 3 (`~)
	UNASSIGNED_C1 = 0xC1,  // Unassigned/Reserved
	UNASSIGNED_C2 = 0xC2,  // Unassigned/Reserved
	UNASSIGNED_C3 = 0xC3,  // Unassigned/Reserved
	UNASSIGNED_C4 = 0xC4,  // Unassigned/Reserved
	UNASSIGNED_C5 = 0xC5,  // Unassigned/Reserved
	UNASSIGNED_C6 = 0xC6,  // Unassigned/Reserved
	UNASSIGNED_C7 = 0xC7,  // Unassigned/Reserved
	UNASSIGNED_C8 = 0xC8,  // Unassigned/Reserved
	UNASSIGNED_C9 = 0xC9,  // Unassigned/Reserved
	UNASSIGNED_CA = 0xCA,  // Unassigned/Reserved
	UNASSIGNED_CB = 0xCB,  // Unassigned/Reserved
	UNASSIGNED_CC = 0xCC,  // Unassigned/Reserved
	UNASSIGNED_CD = 0xCD,  // Unassigned/Reserved
	UNASSIGNED_CE = 0xCE,  // Unassigned/Reserved
	UNASSIGNED_CF = 0xCF,  // Unassigned/Reserved
	UNASSIGNED_D0 = 0xD0,  // Unassigned/Reserved
	UNASSIGNED_D1 = 0xD1,  // Unassigned/Reserved
	UNASSIGNED_D2 = 0xD2,  // Unassigned/Reserved
	UNASSIGNED_D3 = 0xD3,  // Unassigned/Reserved
	UNASSIGNED_D4 = 0xD4,  // Unassigned/Reserved
	UNASSIGNED_D5 = 0xD5,  // Unassigned/Reserved
	UNASSIGNED_D6 = 0xD6,  // Unassigned/Reserved
	UNASSIGNED_D7 = 0xD7,  // Unassigned/Reserved
	UNASSIGNED_D8 = 0xD8,  // Unassigned/Reserved
	UNASSIGNED_D9 = 0xD9,  // Unassigned/Reserved
	UNASSIGNED_DA = 0xDA,  // Unassigned/Reserved
	OEM_4 = 0xDB,  // OEM 4 ([{)
	OEM_5 = 0xDC,  // OEM 5 (\|)
	OEM_6 = 0xDD,  // OEM 6 (]})
	OEM_7 = 0xDE,  // OEM 7 ('")
	OEM_8 = 0xDF,  // OEM 8
	UNASSIGNED_E0 = 0xE0,  // Unassigned/Reserved
	OEM_SPECIFIC_E1 = 0xE1,  // Unassigned/Reserved
	OEM_102 = 0xE2,  // OEM 102 (<>|)
	OEM_SPECIFIC_E3 = 0xE3,  // Unassigned/Reserved
	OEM_SPECIFIC_E4 = 0xE4,  // Unassigned/Reserved
	PROCESSKEY = 0xE5,  // Process Key
	OEM_SPECIFIC_E6 = 0xE6,  // Unassigned/Reserved
	PACKET = 0xE7,  // Packet
	UNASSIGNED_E8 = 0xE8,  // Unassigned/Reserved
	OEM_SPECIFIC_E9 = 0xE9,  // Unassigned/Reserved
	OEM_SPECIFIC_EA = 0xEA,  // Unassigned/Reserved
	OEM_SPECIFIC_EB = 0xEB,  // Unassigned/Reserved
	OEM_SPECIFIC_EC = 0xEC,  // Unassigned/Reserved
	OEM_SPECIFIC_ED = 0xED,  // Unassigned/Reserved
	OEM_SPECIFIC_EE = 0xEE,  // Unassigned/Reserved
	OEM_SPECIFIC_EF = 0xEF,  // Unassigned/Reserved
	OEM_SPECIFIC_F0 = 0xF0,  // Unassigned/Reserved
	OEM_SPECIFIC_F1 = 0xF1,  // Unassigned/Reserved
	OEM_SPECIFIC_F2 = 0xF2,  // Unassigned/Reserved
	OEM_SPECIFIC_F3 = 0xF3,  // Unassigned/Reserved
	OEM_SPECIFIC_F4 = 0xF4,  // Unassigned/Reserved
	OEM_SPECIFIC_F5 = 0xF5,  // Unassigned/Reserved
	ATTN = 0xF6,  // Attn
	CRSEL = 0xF7,  // CrSel
	EXSEL = 0xF8,  // ExSel
	EREOF = 0xF9,  // Erase EOF
	PLAY = 0xFA,  // Play
	ZOOM = 0xFB,  // Zoom
	NONAME = 0xFC,  // NoName
	PA1 = 0xFD,  // PA1
	OEM_CLEAR = 0xFE,  // OEM Clear
	UNASSIGNED_FF = 0xFF,  // Unassigned/Reserved
};

class KeyWrapper
{
public:

	static KeyWrapper& Instance();
	static void Destroy();

	void Finalize();

	void Update();

	bool GetKey(KeyCode code);
	bool GetKeyDown(KeyCode code);
	bool GetKeyUp(KeyCode code);

	bool GetKey(unsigned char mask);
	bool GetKeyDown(unsigned char mask);
	bool GetKeyUp(unsigned char mask);


private:
	KeyWrapper();
	~KeyWrapper();

	static KeyWrapper* m_pInstance;

	unsigned char m_KeyState[256]{};
	unsigned char m_KeyStatePrev[256]{};

};
