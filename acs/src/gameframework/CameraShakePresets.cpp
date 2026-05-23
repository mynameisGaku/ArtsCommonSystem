// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E Phase 3 — CameraShakePresets 実装
//
// 設計上のポイント:
//   ・組み込み preset の数値は「small/large + duration」感が一聴で伝わる
//     値域に揃えてある。trauma は 0.3 〜 0.9、amplitude は 0.4 〜 1.5、
//     decay_rate は 1.0 〜 2.5 (= 約 0.3 〜 1.0 秒で減衰しきる)。
//     duration_hint は trauma / decay_rate の概算秒数を入れている (caller
//     の UI / SFX 尺合わせ用)。
//   ・name 比較は const char* per-byte (AchievementManager と同設計)。
//     STL <string> / <cstring> 不使用。
//   ・Custom 登録は同 name で「上書き」。AchievementManager は重複を黙って
//     弾くが、preset は「最新値が勝つ」方が DCC ツール再ロード時のフローに
//     合うため上書き挙動を選ぶ (DamageFeedback / Tween の感覚と同じ)。
#include "gameframework/CameraShakePresets.h"

namespace acs::game {

namespace {

// const char* per-byte 安全比較。どちらかが nullptr なら false。
// AchievementManager / Entitlement と同設計の helper を anonymous namespace に
// 再掲する (ピラー単位で独立しているほうが読みやすい)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「未発見」哨兵値。
constexpr u32 kNotFound = ~static_cast<u32>(0);

// 組み込み preset の実値。amplitude / decay の単位は Camera2D.h の trauma 方式
// を参照。frequency は Camera2D 実装が将来 SetShakeFrequency を持ったときに
// 流す前提の「希望値」。duration_hint は trauma / decay_rate の概算秒数。
//
// 数値設計:
//   ExplosionSmall  : 短い punch。trauma 中、amp 中、decay 速め。
//   ExplosionLarge  : 大爆発。trauma 強、amp 大、decay 標準。
//   EarthquakeShort : 短地震。trauma 中弱、amp 中、decay 遅め (= 揺れが残る)。
//   EarthquakeLong  : 長地震。trauma 中、amp 中強、decay 非常に遅い、freq 低い。
//   HitImpact       : 着弾。trauma 弱、amp 弱、decay 高速、freq 高め (キレ重視)。
//   RocketLaunch    : 連射継続。trauma 中、amp 小、decay 中、freq 高め。
//   MeteorImpact    : 隕石。trauma 最強、amp 最大、decay 遅め、freq 低め。
//   Custom          : 哨兵 — ZeroParams を返す。
//
// `static const` でテーブル化すると thread-local 初期化が絡むので、関数 switch
// で素直に書く (コンパイラは余裕でテーブル化する)。
ShakeParams BuiltinParams(EShakePreset p) noexcept {
    switch (p) {
        case EShakePreset::ExplosionSmall:
            return ShakeParams{0.4f, 0.6f, 1.5f, 25.0f, 0.27f};
        case EShakePreset::ExplosionLarge:
            return ShakeParams{0.8f, 1.2f, 1.0f, 20.0f, 0.80f};
        case EShakePreset::EarthquakeShort:
            return ShakeParams{0.5f, 0.7f, 0.8f, 12.0f, 0.62f};
        case EShakePreset::EarthquakeLong:
            return ShakeParams{0.6f, 0.9f, 0.3f, 8.0f, 2.00f};
        case EShakePreset::HitImpact:
            return ShakeParams{0.3f, 0.4f, 2.0f, 30.0f, 0.15f};
        case EShakePreset::RocketLaunch:
            return ShakeParams{0.5f, 0.35f, 1.2f, 28.0f, 0.42f};
        case EShakePreset::MeteorImpact:
            return ShakeParams{0.9f, 1.5f, 0.7f, 16.0f, 1.28f};
        case EShakePreset::Custom:
        default:
            // Custom 哨兵 / 未知値 — 中立 (= 適用しても見た目には影響しない値)。
            return ShakeParams{0.0f, 0.0f, 1.0f, 25.0f, 0.0f};
    }
}

} // namespace

// =============================================================================
// 組み込み preset
// =============================================================================

ShakeParams CameraShakePresets::GetPreset(EShakePreset preset) noexcept {
    return BuiltinParams(preset);
}

void CameraShakePresets::ApplyPreset(IShakeTarget& target,
                                     EShakePreset   preset) noexcept {
    // Custom は名前経由 (ApplyCustomByName) 専用 — 即値経由では no-op。
    if (preset == EShakePreset::Custom) return;

    const ShakeParams p = BuiltinParams(preset);
    // 順序: 静的パラメータを先に上書きしてから trauma を累積。
    // 連続トリガー時に AddShake の clamp で 1.0 に張り付くのが Eiserloh 流。
    target.SetShakeAmplitude(p.amplitude);
    target.SetShakeDecayRate(p.decay_rate);
    target.AddShake(p.trauma);
}

// =============================================================================
// カスタム preset
// =============================================================================

u32 CameraShakePresets::FindCustomIndex(const char* name) const noexcept {
    if (name == nullptr) return kNotFound;
    const usize n = _customs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_customs[i].name, name)) return static_cast<u32>(i);
    }
    return kNotFound;
}

void CameraShakePresets::RegisterCustomPreset(const char*        name,
                                              const ShakeParams& params) noexcept {
    if (name == nullptr) return;

    // 既存 name は上書き (DCC ツール再ロードで最新値が勝つ運用)。
    const u32 idx = FindCustomIndex(name);
    if (idx != kNotFound) {
        _customs[idx].params = params;
        return;
    }

    CustomEntry e{};
    e.name   = name;     // static lifetime を caller が保証する
    e.params = params;
    _customs.PushBack(e);
}

bool CameraShakePresets::ApplyCustomByName(IShakeTarget& target,
                                           const char*   name) noexcept {
    const u32 idx = FindCustomIndex(name);
    if (idx == kNotFound) return false;

    const ShakeParams& p = _customs[idx].params;
    // 組み込み preset と同じ順序で適用 (SetAmp → SetDecay → AddShake)。
    target.SetShakeAmplitude(p.amplitude);
    target.SetShakeDecayRate(p.decay_rate);
    target.AddShake(p.trauma);
    return true;
}

u32 CameraShakePresets::CustomCount() const noexcept {
    return static_cast<u32>(_customs.Size());
}

} // namespace acs::game
