// SPDX-License-Identifier: Apache-2.0
// HID ゲームパッドの入力レポート解釈テスト。
//
// 実機を繋がずに、各機種の仕様どおり組み立てた合成レポートを解かせて突き合わせる。
// 実機が本当にこの並びで送ってくるかまでは分からないが、ビット位置・12bit のほどき方・
// 軸の符号・IMU の係数といった解釈の中身はここで固定できる。
#include "test/Expect.h"
#include "test/Test.h"

// 解析関数は無名 namespace に閉じてあるので、実装ごと取り込んで直接叩く。
#include "platform/HidGamepad.cpp"

#include <cmath>

using namespace acs;

namespace {

/** ボタンが押されているかを返す。 */
bool IsDown(const FHidGamepadState& state, EGamepadButton button) noexcept
{
    return (state.buttons & (1u << static_cast<u32>(button))) != 0;
}

/** 軸の値を返す。 */
f32 AxisOf(const FHidGamepadState& state, EGamepadAxis axis) noexcept
{
    return state.axes[static_cast<usize>(axis)];
}

/** リトルエンディアンで 16bit を書く。 */
void WriteInt16At(u8* report, u32 offset, i16 value) noexcept
{
    report[offset]     = static_cast<u8>(value & 0xFF);
    report[offset + 1] = static_cast<u8>((value >> 8) & 0xFF);
}

} // namespace


ACS_TEST(HidGamepadReport, DualShock4DecodesButtonsAxesAndMotion)
{
    /** USB のレポート ID 0x01。以降 1 byte ずれる。 */
    u8 report[64] = {};
    report[0] = 0x01u;
    report[1] = 0xFFu;   // LeftX 右いっぱい
    report[2] = 0x00u;   // LeftY 上いっぱい
    report[3] = 0x80u;   // RightX 中央
    report[4] = 0x80u;   // RightY 中央
    report[5] = 0x20u | 0x02u;   // × と hat=2 (右)
    report[6] = 0x01u | 0x20u;   // L1 と Options
    report[7] = 0x01u;           // PS
    report[8] = 0xFFu;           // L2 いっぱい
    report[9] = 0x00u;           // R2 離す
    WriteInt16At(report, 1 + 12, 100);   // gyro.x
    WriteInt16At(report, 1 + 18, 200);   // accel.x

    FHidGamepadState state{};
    EXPECT_TRUE(ParseDualShock4(report, 64, state));

    // 位置ベースの名前へ読み替えられていること (× は Xbox の A と同じ位置)。
    EXPECT_TRUE(IsDown(state, EGamepadButton::South));
    EXPECT_FALSE(IsDown(state, EGamepadButton::East));
    EXPECT_TRUE(IsDown(state, EGamepadButton::Right));
    EXPECT_FALSE(IsDown(state, EGamepadButton::Up));
    EXPECT_TRUE(IsDown(state, EGamepadButton::LeftBumper));
    EXPECT_TRUE(IsDown(state, EGamepadButton::Start));
    EXPECT_TRUE(IsDown(state, EGamepadButton::Guide));

    // 縦軸は上が正 (HID は下が正なので反転している)。
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftX), 1.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftY), 1.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightX), 0.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftTrigger), 1.0f, 0.01f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightTrigger), 0.0f, 0.01f);

    EXPECT_TRUE(state.motion.valid);
    EXPECT_NEAR(state.motion.gyro.x, 100.0f * kSonyGyroScale, 0.001f);
    EXPECT_NEAR(state.motion.accel.x, 200.0f * kSonyAccelScale, 0.001f);
}

ACS_TEST(HidGamepadReport, DualShock4CentersSticksAtMidpoint)
{
    u8 report[64] = {};
    report[0] = 0x01u;
    report[1] = 0x80u;
    report[2] = 0x80u;
    report[3] = 0x80u;
    report[4] = 0x80u;

    FHidGamepadState state{};
    EXPECT_TRUE(ParseDualShock4(report, 64, state));
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftX), 0.0f, 0.01f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftY), 0.0f, 0.01f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightX), 0.0f, 0.01f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightY), 0.0f, 0.01f);
}

ACS_TEST(HidGamepadReport, DualShock4BluetoothMatchesUsb)
{
    /** USB (0x01) と Bluetooth (0x11) は 2 byte ずれるだけで中身は同じ。 */
    u8 usb[64] = {};
    usb[0] = 0x01u;
    usb[1] = 0xFFu; usb[2] = 0x00u; usb[3] = 0x80u; usb[4] = 0x80u;
    usb[5] = 0x20u; usb[6] = 0x01u;

    u8 bluetooth[80] = {};
    bluetooth[0] = 0x11u;
    for (u32 i = 1; i <= 9; ++i) bluetooth[i + 2] = usb[i];

    FHidGamepadState usb_state{};
    FHidGamepadState bt_state{};
    EXPECT_TRUE(ParseDualShock4(usb, 64, usb_state));
    EXPECT_TRUE(ParseDualShock4(bluetooth, 80, bt_state));

    EXPECT_EQ(bt_state.buttons, usb_state.buttons);
    EXPECT_NEAR(AxisOf(bt_state, EGamepadAxis::LeftX), AxisOf(usb_state, EGamepadAxis::LeftX), 0.001f);
    EXPECT_NEAR(AxisOf(bt_state, EGamepadAxis::LeftY), AxisOf(usb_state, EGamepadAxis::LeftY), 0.001f);
}

ACS_TEST(HidGamepadReport, DualShock4RejectsUnknownAndShortReports)
{
    u8 unknown[64] = {};
    unknown[0] = 0x05u;

    u8 valid[64] = {};
    valid[0] = 0x01u;

    FHidGamepadState state{};
    EXPECT_FALSE(ParseDualShock4(unknown, 64, state));
    EXPECT_FALSE(ParseDualShock4(valid, 5, state));
}

ACS_TEST(HidGamepadReport, SwitchDecodesButtonsAndPackedSticks)
{
    /** 標準の入力レポート 0x30。 */
    u8 report[64] = {};
    report[0] = 0x30u;
    report[3] = 0x08u | 0x40u;   // A と R
    report[4] = 0x02u;           // +
    report[5] = 0x02u | 0x80u;   // 十字上 と ZL

    // 12bit を 2 本ずつ 3 byte へ詰める。
    //   x = [0] | (([1] & 0x0F) << 8)      y = ([1] >> 4) | ([2] << 4)
    report[6] = 0x00u; report[7] = 0x08u; report[8] = 0x80u;   // 左は中央 (0x800, 0x800)
    report[9] = 0xFFu; report[10] = 0x0Fu; report[11] = 0x80u; // 右は X いっぱい / Y 中央

    FHidGamepadState state{};
    EXPECT_TRUE(ParseSwitch(report, 64, EHidGamepadKind::SwitchPro, state));

    // Nintendo の A は Xbox の B と同じ位置なので East になる (刻印ではなく位置で持つ)。
    EXPECT_TRUE(IsDown(state, EGamepadButton::East));
    EXPECT_FALSE(IsDown(state, EGamepadButton::South));
    EXPECT_TRUE(IsDown(state, EGamepadButton::RightBumper));
    EXPECT_TRUE(IsDown(state, EGamepadButton::Start));
    EXPECT_TRUE(IsDown(state, EGamepadButton::Up));
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftTrigger), 1.0f, 0.01f);

    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftX), 0.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftY), 0.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightX), 1.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::RightY), 0.0f, 0.02f);
}

ACS_TEST(HidGamepadReport, SwitchUsesLatestImuSample)
{
    /** 1 パケットに 5ms 間隔で 3 サンプル入るので、最新のものだけを使う。 */
    u8 report[64] = {};
    report[0] = 0x30u;
    report[6] = 0x00u; report[7] = 0x08u; report[8] = 0x80u;
    report[9] = 0x00u; report[10] = 0x08u; report[11] = 0x80u;
    WriteInt16At(report, 13, 111);            // 1 サンプル目 (使われない)
    WriteInt16At(report, 13 + 24 + 0, 333);   // 3 サンプル目 accel.x
    WriteInt16At(report, 13 + 24 + 6, 444);   // 3 サンプル目 gyro.x

    FHidGamepadState state{};
    EXPECT_TRUE(ParseSwitch(report, 64, EHidGamepadKind::SwitchPro, state));
    EXPECT_TRUE(state.motion.valid);
    EXPECT_NEAR(state.motion.accel.x, 333.0f * kSwitchAccelScale, 0.001f);
    EXPECT_NEAR(state.motion.gyro.x, 444.0f * kSwitchGyroScale, 0.001f);
}

ACS_TEST(HidGamepadReport, JoyConLeftRotatesStickForSidewaysGrip)
{
    /** Joy-Con 単体は横持ちなので、スティックの向きが本体と 90 度ずれる。 */
    u8 report[64] = {};
    report[0] = 0x30u;
    // left_x = 0x800 (中央) / left_y = 0xFFF (上いっぱい)
    report[6] = 0x00u;
    report[7] = static_cast<u8>(0x08u | (0x0Fu << 4));
    report[8] = 0xFFu;

    FHidGamepadState state{};
    EXPECT_TRUE(ParseSwitch(report, 64, EHidGamepadKind::JoyConLeft, state));

    // 横に倒すと、縦に倒したぶんが横へ回る。
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftX), -1.0f, 0.02f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftY), 0.0f, 0.02f);
}

ACS_TEST(HidGamepadReport, SwitchRejectsUnknownAndShortReports)
{
    u8 unknown[64] = {};
    unknown[0] = 0x3Fu;

    u8 valid[64] = {};
    valid[0] = 0x30u;

    FHidGamepadState state{};
    EXPECT_FALSE(ParseSwitch(unknown, 64, EHidGamepadKind::SwitchPro, state));
    EXPECT_FALSE(ParseSwitch(valid, 8, EHidGamepadKind::SwitchPro, state));
}

ACS_TEST(HidGamepadReport, DualSenseCentersSticksAndRejectsUnknown)
{
    u8 report[64] = {};
    report[0] = 0x01u;
    report[1] = 0x80u; report[2] = 0x80u; report[3] = 0x80u; report[4] = 0x80u;

    FHidGamepadState state{};
    EXPECT_TRUE(ParseDualSense(report, 64, state));
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftX), 0.0f, 0.01f);
    EXPECT_NEAR(AxisOf(state, EGamepadAxis::LeftY), 0.0f, 0.01f);

    u8 unknown[64] = {};
    unknown[0] = 0x09u;
    EXPECT_FALSE(ParseDualSense(unknown, 64, state));
}

// 刻印での別名が位置と同じ値であること。実ボタン数が別名で水増しされていないことも見る。
static_assert(EGamepadButton::A == EGamepadButton::South);
static_assert(EGamepadButton::B == EGamepadButton::East);
static_assert(EGamepadButton::X == EGamepadButton::West);
static_assert(EGamepadButton::Y == EGamepadButton::North);
static_assert(EGamepadButton::Cross == EGamepadButton::South);
static_assert(EGamepadButton::Circle == EGamepadButton::East);
static_assert(EGamepadButton::Square == EGamepadButton::West);
static_assert(EGamepadButton::Triangle == EGamepadButton::North);
static_assert(static_cast<u32>(EGamepadButton::_Count) == 15u);
