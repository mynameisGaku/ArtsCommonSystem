#pragma once
#include "Pch.h"
#include <extension\Joyshock\JoyShockLibrary.h>


namespace Pad
{
	enum PadCode
	{
		S = JSMASK_S,
		W = JSMASK_W,
		E = JSMASK_E,
		N = JSMASK_N,
		ZL = JSMASK_ZL,
		ZR = JSMASK_ZR,
		L = JSMASK_L,
		R = JSMASK_R,
		MINUS = JSMASK_MINUS,
		PLUS = JSMASK_PLUS,
		LCLICK = JSMASK_LCLICK,
		RCLICK = JSMASK_RCLICK,
		HOME = JSMASK_HOME,
		CAPTURE = JSMASK_CAPTURE,
		UNKNOWN = 0,
	};
}
using namespace Pad;
class JoyshockWrapper
{
public:
	static JoyshockWrapper& Instance();
	static void Destroy();

	///<summary>
	/// 初期化。接続済みデバイスを検出して内部状態を構築する。
	///</summary>
	/// <returns>成功なら true</returns>
	bool Initialize();

	///<summary>
	/// 終了処理。全デバイス切断と内部リソースの解放を行う。
	///</summary>
	void Finalize();

	///<summary>
	/// デバイスの再検出（ホットプラグ対応）。既存接続を維持したまま新規デバイスを追加検出する。
	///</summary>
	void RescanDevices();

	///<summary>
	/// 1フレーム更新。各デバイスの状態を取得し、押下遷移（Down/Up）を算出する。
	///</summary>
	void Update();

	///<summary>
	/// 接続中デバイス数を取得する。
	///</summary>
	/// <returns>接続台数</returns>
	int GetDeviceCount() const;

	///<summary>
	/// インデックスから JSL のデバイスハンドルを取得する。
	///</summary>
	/// <param name="index">0 〜 GetDeviceCount()-1</param>
	/// <returns>JSL ハンドル（int）。不正時は 0</returns>
	int GetHandleByIndex(int index) const;

	///<summary>
	/// 指定デバイスが接続中かを取得する。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <returns>接続中なら true</returns>
	bool IsConnected(int index) const;

	///<summary>
	/// 生ボタンビットの現在状態を取得する（JSL の `JOY_SHOCK_STATE.buttons` 用マスクを渡す）。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="buttonMask">ボタンビットマスク（JSL 定義）</param>
	/// <returns>押下中なら true</returns>
	bool GetRawButton(int index, uint32_t buttonMask) const;

	///<summary>
	/// 生ボタンビットの「押した瞬間」を取得する（前フレーム非押下→今フレーム押下）。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="buttonMask">ボタンビットマスク（JSL 定義）</param>
	/// <returns>押した瞬間なら true</returns>
	bool GetRawButtonDown(int index, uint32_t buttonMask) const;

	///<summary>
	/// 生ボタンビットの「離した瞬間」を取得する（前フレーム押下→今フレーム非押下）。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="buttonMask">ボタンビットマスク（JSL 定義）</param>
	/// <returns>離した瞬間なら true</returns>
	bool GetRawButtonUp(int index, uint32_t buttonMask) const;

	///<summary>
	/// 左スティック（-1..1）を取得する。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="outLX">X 値（-1..1）</param>
	/// <param name="outLY">Y 値（-1..1）</param>
	void GetLeftStick(int index, float& outLX, float& outLY) const;

	///<summary>
	/// 右スティック（-1..1）を取得する。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="outRX">X 値（-1..1）</param>
	/// <param name="outRY">Y 値（-1..1）</param>
	void GetRightStick(int index, float& outRX, float& outRY) const;

	///<summary>
	/// 左右トリガ値を取得する（DS 系は 0..1 のアナログ、Nintendo 系は 0/1）。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="outL">左トリガ</param>
	/// <param name="outR">右トリガ</param>
	void GetTriggers(int index, float& outL, float& outR) const;

	///<summary>
	/// ジャイロの累積（前回取得からの平均角速度）を取得し、内部累積をクリアする。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="outX">角速度 X</param>
	/// <param name="outY">角速度 Y</param>
	/// <param name="outZ">角速度 Z</param>
	void GetAndFlushGyro(int index, float& outX, float& outY, float& outZ);

	///<summary>
	/// 自動ジャイロキャリブレーションの有効/無効を設定する。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="enable">有効なら true</param>
	void SetAutomaticCalibration(int index, bool enable);

	///<summary>
	/// ジャイロの座標空間を設定する（0=Local, 1=World, 2=Player）。
	///</summary>
	/// <param name="index">デバイスインデックス</param>
	/// <param name="space">0/1/2</param>
	void SetGyroSpace(int index, int space);

	///<summary>
	/// デッドゾーンとしきい値を設定する（スティック／トリガ用のクランプ・フィルタ）。
	///</summary>
	/// <param name="stickDeadZone">スティックのデッドゾーン（0..1）</param>
	/// <param name="triggerThreshold">トリガの押下判定しきい値（0..1）</param>
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
		int handle;								// JSL ハンドル
		bool connected;							// 接続状態
		uint32_t prevButtons;					// 前フレームのボタン
		uint32_t currButtons;					// 今フレームのボタン
		JOY_SHOCK_STATE state;					// 今フレームのアナログ値
		float lastGyroX;						// 直近取得したジャイロX
		float lastGyroY;						// 直近取得したジャイロY
		float lastGyroZ;						// 直近取得したジャイロZ
	};

	struct XInputDeviceState
	{
		bool connected;							// 接続状態
		::XINPUT_STATE	currXinputState;			// XInput 状態
		::XINPUT_STATE	prevXinputState;			// XInput 状態
		int handle;								// XBOXコントローラー ハンドル
	};

	void RefreshHandleList();					// 内部：ハンドル一覧を再取得する
	bool IsValidIndex(int index) const;			// 内部：インデックス範囲チェック
	void RefreshXInputHandleList();				// 内部：XInputハンドル一覧を再取得する

private:
	bool							m_Initialized;		// 初期化済みフラグ
	std::vector<DeviceState>		m_Devices;			// デバイス配列
	std::vector<XInputDeviceState>	m_XInputDevices;	// デバイス配列
	float							m_StickDeadZone;	// スティックのデッドゾーン 0..1
	float							m_TriggerThreshold;	// トリガ押下のしきい値 0..1
	mutable std::mutex				m_Mutex;			// スレッドセーフ用ミューテックス
};