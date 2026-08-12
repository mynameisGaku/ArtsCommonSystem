// SPDX-License-Identifier: Apache-2.0
// 時刻進行とtime-of-day配色の補間を実装する。
//
// キーフレーム間を線形補間する time-of-day ドライバの実装。
// stop 表は雰囲気重視の「ゲームらしい」配色 (写実色温度ではない):
//   00:00 夜    紺 / 暗青
//   04:00 夜明け前 紺寄り紫 / 暗赤紫
//   06:00 朝焼け 橙 / 赤茶
//   12:00 昼    青 / 白
//   18:00 夕焼け 紫赤 / 橙
//   22:00 夜    紺 / 暗青  (24:00 ≡ 00:00 でループ)
#include "gameframework/AmbientDirector.h"

namespace acs::game {

/**
 * time-of-day 補間用の固定キーフレーム表。
 *
 * @details hour 昇順で隣接 stop 間を線形補間する。0:00 と 22:00 が同じ夜色なので
 * 22:00→24:00 (= 0:00) 区間も自然にループする。色は写実色温度ではなく雰囲気重視の配色。
 */
const CAmbientDirector::FTimeStop CAmbientDirector::m_Stops[6] = {
    //   hour  sky (RGB linear-ish 0..1)        ambient (RGB)
    {  0.0f,  FVec3{0.02f, 0.03f, 0.10f},  FVec3{0.03f, 0.04f, 0.10f} }, // 真夜中 紺
    {  4.0f,  FVec3{0.08f, 0.06f, 0.18f},  FVec3{0.10f, 0.07f, 0.12f} }, // 夜明け前 紫紺
    {  6.0f,  FVec3{0.95f, 0.55f, 0.25f},  FVec3{0.55f, 0.30f, 0.20f} }, // 朝焼け 橙 / 赤茶
    { 12.0f,  FVec3{0.40f, 0.65f, 0.95f},  FVec3{0.95f, 0.95f, 0.95f} }, // 昼 青空 / 白
    { 18.0f,  FVec3{0.65f, 0.25f, 0.45f},  FVec3{0.90f, 0.55f, 0.25f} }, // 夕焼け 紫赤 / 橙
    { 22.0f,  FVec3{0.02f, 0.03f, 0.10f},  FVec3{0.03f, 0.04f, 0.10f} }, // 夜 紺
};

/**
 * 時刻を [0, 24) に正規化する。
 *
 * @details Mod で 24 周期に畳み、負値も正に補正し、境界の 24.0f は 0 に丸める。
 * @param h 正規化する時刻 (時)。
 * @return [0, 24) に収めた時刻。
 */
static f32 WrapHours(f32 h) noexcept {
    // 大きすぎる値も Mod で 24 周期に。
    h = Mod(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    // 念のため境界処理: 24.0f が来ると [0,24) を外れる。
    if (h >= 24.0f) h = 0.0f;
    return h;
}

/** 時刻を正規化して現在時刻に設定する。 */
void CAmbientDirector::SetTimeOfDay(f32 hours) noexcept {
    m_Hours = WrapHours(hours);
}

/** ゲーム時間を進める (負値は 0 にクランプし、結果を [0, 24) にラップ)。 */
void CAmbientDirector::AdvanceTime(f32 dt_hours) noexcept {
    if (dt_hours < 0.0f) dt_hours = 0.0f;  // 時間は戻さない
    m_Hours = WrapHours(m_Hours + dt_hours);
}

/**
 * FindPair が返す補間対象の stop ペアと補間係数。
 *
 * @details t = 0 で a、t = 1 で b。22:00→24:00 ラップ時は b に m_Stops[0] (= 0:00) を採る。
 */
struct FStopPair {
    /** 補間始点の stop。 */
    const CAmbientDirector::FTimeStop* a;

    /** 補間終点の stop。 */
    const CAmbientDirector::FTimeStop* b;

    /** a→b の補間係数 [0, 1]。 */
    f32 t;
};

/**
 * 現在時刻を挟む stop ペアと補間係数を求める。
 *
 * @details 22:00 以降は (stops[5], stops[0]) を span=2h でラップ補間し、それ以外は
 * 該当区間 i..i+1 を返す。span=0 はゼロ除算ガードで t=0 にする。
 * @param stops 補間に使うキーフレーム表。
 * @param hours 現在時刻 [0, 24)。
 * @return 補間対象の stop ペアと係数。
 */
static FStopPair FindPair(const CAmbientDirector::FTimeStop (&stops)[6], f32 hours) noexcept {
    // hours は [0, 24)。stops[0].hour == 0、stops[5].hour == 22。
    // ケース 1: 22.0 <= hours < 24.0 → (stops[5], stops[0]+24, span=2h)
    if (hours >= stops[5].hour) {
        const f32 span = 24.0f - stops[5].hour;        // = 2.0f
        const f32 t    = (hours - stops[5].hour) / span;
        return { &stops[5], &stops[0], t };
    }
    // ケース 2: 通常区間 i..i+1
    for (u32 i = 0; i < 5; ++i) {
        if (hours < stops[i + 1].hour) {
            const f32 span = stops[i + 1].hour - stops[i].hour;
            // span は表の構造上 0 にならないが、念のためゼロ除算ガード。
            const f32 t = span > 0.0f ? (hours - stops[i].hour) / span : 0.0f;
            return { &stops[i], &stops[i + 1], t };
        }
    }
    // ここには来ない (stops[5].hour 以上は ケース 1 で吸収) が、フォールバック。
    return { &stops[5], &stops[0], 0.0f };
}

/** 現在時刻の空の色を stop 間補間で返す。 */
FVec3 CAmbientDirector::SkyColor() const noexcept {
    const FStopPair p = FindPair(m_Stops, m_Hours);
    return Lerp(p.a->sky, p.b->sky, p.t);
}

/** 現在時刻の環境光の色を stop 間補間で返す。 */
FVec3 CAmbientDirector::AmbientColor() const noexcept {
    const FStopPair p = FindPair(m_Stops, m_Hours);
    return Lerp(p.a->ambient, p.b->ambient, p.t);
}

/** 現在時刻の太陽方向を hour_angle から算出して返す。 */
FVec3 CAmbientDirector::SunDirection() const noexcept {
    // hour_angle = (hour - 6) / 12 * π:
    //   06:00 →  0     : 東地平線 (x=+1, y= 0)
    //   12:00 →  π/2   : 天頂   (x= 0, y=+1)
    //   18:00 →  π     : 西地平線 (x=-1, y= 0)
    //   00:00 → -π/2   : 真下   (x= 0, y=-1)
    // 太陽は「東→南 (= 天頂)→西」の弧を描く。Z 軸 (方位ずれ) は将来対応。
    const f32 angle = (m_Hours - 6.0f) / 12.0f * kPi;
    const f32 cx = Cos(angle);
    const f32 sy = Sin(angle);
    // 既に単位長 (cos²+sin²=1) なので Normalize 不要。z=0 は方位固定 (真南北軌道)。
    return FVec3{ cx, sy, 0.0f };
}

} // namespace acs::game
