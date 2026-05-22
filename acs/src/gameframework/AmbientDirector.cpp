// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — AmbientDirector 実装
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

// stop 表は hour 昇順。隣接 stop 間で線形補間する。
// 0:00 と 22:00 が同じ夜色なので 22:00→24:00 (= 0:00) 区間も自然にループ。
const AmbientDirector::TimeStop AmbientDirector::_stops[6] = {
    //   hour  sky (RGB linear-ish 0..1)        ambient (RGB)
    {  0.0f,  Vec3{0.02f, 0.03f, 0.10f},  Vec3{0.03f, 0.04f, 0.10f} }, // 真夜中 紺
    {  4.0f,  Vec3{0.08f, 0.06f, 0.18f},  Vec3{0.10f, 0.07f, 0.12f} }, // 夜明け前 紫紺
    {  6.0f,  Vec3{0.95f, 0.55f, 0.25f},  Vec3{0.55f, 0.30f, 0.20f} }, // 朝焼け 橙 / 赤茶
    { 12.0f,  Vec3{0.40f, 0.65f, 0.95f},  Vec3{0.95f, 0.95f, 0.95f} }, // 昼 青空 / 白
    { 18.0f,  Vec3{0.65f, 0.25f, 0.45f},  Vec3{0.90f, 0.55f, 0.25f} }, // 夕焼け 紫赤 / 橙
    { 22.0f,  Vec3{0.02f, 0.03f, 0.10f},  Vec3{0.03f, 0.04f, 0.10f} }, // 夜 紺
};

// ----- 時刻ユーティリティ ---------------------------------------------------

// [0, 24) に正規化。負値も fmod の符号挙動を回避して正に。
static f32 WrapHours(f32 h) noexcept {
    // 大きすぎる値も Mod で 24 周期に。
    h = Mod(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    // 念のため境界処理: 24.0f が来ると [0,24) を外れる。
    if (h >= 24.0f) h = 0.0f;
    return h;
}

void AmbientDirector::SetTimeOfDay(f32 hours) noexcept {
    _hours = WrapHours(hours);
}

void AmbientDirector::AdvanceTime(f32 dt_hours) noexcept {
    if (dt_hours < 0.0f) dt_hours = 0.0f;  // 時間は戻さない
    _hours = WrapHours(_hours + dt_hours);
}

// ----- 補間ヘルパ -----------------------------------------------------------

// 現在時刻に対し、(prev_stop, next_stop, t) を返す。
// t = 0 で prev、t = 1 で next。next は 22:00→24:00 ラップ時に _stops[0] (= 0:00) を採用。
struct StopPair {
    const AmbientDirector::TimeStop* a;
    const AmbientDirector::TimeStop* b;
    f32 t;
};

static StopPair FindPair(const AmbientDirector::TimeStop (&stops)[6], f32 hours) noexcept {
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

// ----- パブリック getter ----------------------------------------------------

Vec3 AmbientDirector::SkyColor() const noexcept {
    const StopPair p = FindPair(_stops, _hours);
    return Lerp(p.a->sky, p.b->sky, p.t);
}

Vec3 AmbientDirector::AmbientColor() const noexcept {
    const StopPair p = FindPair(_stops, _hours);
    return Lerp(p.a->ambient, p.b->ambient, p.t);
}

Vec3 AmbientDirector::SunDirection() const noexcept {
    // hour_angle = (hour - 6) / 12 * π:
    //   06:00 →  0     : 東地平線 (x=+1, y= 0)
    //   12:00 →  π/2   : 天頂   (x= 0, y=+1)
    //   18:00 →  π     : 西地平線 (x=-1, y= 0)
    //   00:00 → -π/2   : 真下   (x= 0, y=-1)
    // 太陽は「東→南 (= 天頂)→西」の弧を描く。Z 軸 (方位ずれ) は Phase 2 で。
    const f32 angle = (_hours - 6.0f) / 12.0f * kPi;
    const f32 cx = Cos(angle);
    const f32 sy = Sin(angle);
    // 既に単位長 (cos²+sin²=1) なので Normalize 不要。z=0 は方位固定 (真南北軌道)。
    return Vec3{ cx, sy, 0.0f };
}

} // namespace acs::game
