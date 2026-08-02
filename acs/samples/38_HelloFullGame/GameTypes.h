// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — 共通型定数。FHighScore POD と全シーンが参照する
// constexpr 定数 (色 / 物理パラメータ / wave 設定) を集約。
//
// すべて `inline constexpr` で header 内に置くため、複数 TU に include
// しても 1 つのストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hellofg {

// 永続データ (TSaveSlot で .acssave に保存される POD)
struct FHighScore {
    acs::u64 best_score = 0;   // これまでで最高のスコア
    acs::u64 timestamp  = 0;   // 達成時刻 (Clock::Ticks())、参考表示用
};
static_assert(__is_trivially_copyable(FHighScore),
              "FHighScore must be trivially copyable for TSaveSlot");

// world / camera
inline constexpr acs::f32 kWorldUnit  = 32.0f;   // 1 world unit = 32 px (CCamera2D zoom)
inline constexpr acs::f32 kWorldHalfW = 16.0f;   // [-16, +16]
inline constexpr acs::f32 kWorldHalfH = 9.0f;    // [-9,  +9]

// プレイヤー
inline constexpr acs::f32 kPlayerHp     = 3.0f;
inline constexpr acs::f32 kPlayerSpeed  = 6.0f;   // world units / sec
inline constexpr acs::f32 kPlayerRadius = 0.5f;
inline constexpr acs::f32 kPlayerInvuln = 1.0f;   // 被弾後の無敵秒数
inline constexpr acs::f32 kFireCooldown = 0.18f;  // 弾連射の最小間隔 (sec)

// 弾
inline constexpr acs::f32 kBulletSpeed    = 16.0f;
inline constexpr acs::f32 kBulletLifetime = 1.5f;
inline constexpr acs::f32 kBulletRadius   = 0.15f;
inline constexpr acs::f32 kBulletDamage   = 1.0f;

// 敵
inline constexpr acs::f32 kEnemyHp      = 2.0f;
inline constexpr acs::f32 kEnemySpeed   = 2.2f;
inline constexpr acs::f32 kEnemyRadius  = 0.5f;
inline constexpr acs::u32 kMaxEnemies   = 64;
inline constexpr acs::u32 kMaxBullets   = 256;
inline constexpr acs::u32 kMaxParticles = 1024;

// wave
inline constexpr acs::u32 kTotalWaves       = 5;
inline constexpr acs::f32 kIntermissionSec  = 2.0f;
inline constexpr acs::f32 kSpawnIntervalSec = 0.6f;

// 弾種 ID (文字列リテラルなので strcmp / アドレス比較で安全)
inline constexpr const char* kBulletDefId = "player_bullet";

// TSaveSlot のファイル名 (相対パス wide string)。
inline constexpr const wchar_t kSaveFile[] = L"hello_full_game_highscore.acssave";

// HSL を rgb に変換せず、ベタの色定数だけ使う。
inline constexpr acs::FVec4 kColorPlayer      {0.95f, 0.20f, 0.20f, 1.0f};
inline constexpr acs::FVec4 kColorPlayerHurt  {1.0f,  0.6f,  0.6f,  1.0f};
inline constexpr acs::FVec4 kColorEnemy       {0.30f, 0.45f, 0.95f, 1.0f};
inline constexpr acs::FVec4 kColorBullet      {1.0f,  0.95f, 0.20f, 1.0f};
inline constexpr acs::FVec4 kColorTileLight   {0.18f, 0.20f, 0.22f, 1.0f};
inline constexpr acs::FVec4 kColorTileDark    {0.12f, 0.14f, 0.16f, 1.0f};

inline constexpr acs::FVec3 kParticleStart{1.0f, 0.65f, 0.15f};
inline constexpr acs::FVec3 kParticleEnd  {0.6f, 0.05f, 0.05f};

} // namespace hellofg
