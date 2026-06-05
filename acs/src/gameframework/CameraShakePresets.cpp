// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E — FCameraShakePresets 実装
//
// 設計上のポイント:
//   ・組み込み preset の数値は「small/large + duration」感が一聴で伝わる
//     値域に揃えてある。trauma は 0.3 〜 0.9、amplitude は 0.4 〜 1.5、
//     decay_rate は 1.0 〜 2.5 (= 約 0.3 〜 1.0 秒で減衰しきる)。
//     duration_hint は trauma / decay_rate の概算秒数を入れている (caller
//     の UI / SFX 尺合わせ用)。
//   ・name 比較は const char* per-byte (FAchievementManager と同設計)。
//     STL <string> / <cstring> 不使用。
//   ・Custom 登録は同 name で「上書き」。FAchievementManager は重複を黙って
//     弾くが、preset は「最新値が勝つ」方が DCC ツール再ロード時のフローに
//     合うため上書き挙動を選ぶ (FDamageFeedback / FTween の感覚と同じ)。
#include "gameframework/CameraShakePresets.h"

namespace acs::game {

namespace {

/**
 * const char* を per-byte で安全比較する。
 *
 * @details FAchievementManager / FEntitlement と同設計の helper (STL 不使用)。
 * @param a 比較する文字列 A。
 * @param b 比較する文字列 B。
 * @return 内容が一致すれば true。どちらかが nullptr なら false。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/** 「未発見」を表す哨兵値。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

/**
 * 組み込み preset の実値を switch で返す。
 *
 * @details
 * amplitude / decay の単位は FCamera2D.h の trauma 方式を参照。frequency は将来の
 * SetShakeFrequency 用の希望値、duration_hint は trauma / decay_rate の概算秒数。
 * static const テーブルは thread-local 初期化が絡むため関数 switch で素直に書く。
 * @param p 取得するプリセット種別。
 * @return 対応する ShakeParams。Custom / 未知値は中立値を返す。
 */
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

/** 組み込み preset の ShakeParams を返す。 */
ShakeParams FCameraShakePresets::GetPreset(EShakePreset preset) noexcept {
    return BuiltinParams(preset);
}

/** 組み込み preset を SetAmp → SetDecay → AddShake の順で target に流し込む。 */
void FCameraShakePresets::ApplyPreset(IShakeTarget& target,
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

/** 名前でカスタムプリセットのインデックスを線形検索する (未検出は kNotFound)。 */
u32 FCameraShakePresets::FindCustomIndex(const char* name) const noexcept {
    if (name == nullptr) return kNotFound;
    const usize n = m_Customs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Customs[i].name, name)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** カスタム preset を登録する (同 name は上書き、nullptr は no-op)。 */
void FCameraShakePresets::RegisterCustomPreset(const char*        name,
                                              const ShakeParams& params) noexcept {
    if (name == nullptr) return;

    // 既存 name は上書き (DCC ツール再ロードで最新値が勝つ運用)。
    const u32 idx = FindCustomIndex(name);
    if (idx != kNotFound) {
        m_Customs[idx].params = params;
        return;
    }

    CustomEntry e{};
    e.name   = name;     // static lifetime を caller が保証する
    e.params = params;
    m_Customs.PushBack(e);
}

/** 名前で引いたカスタム preset を ApplyPreset と同じ順序で target に流し込む。 */
bool FCameraShakePresets::ApplyCustomByName(IShakeTarget& target,
                                           const char*   name) noexcept {
    const u32 idx = FindCustomIndex(name);
    if (idx == kNotFound) return false;

    const ShakeParams& p = m_Customs[idx].params;
    // 組み込み preset と同じ順序で適用 (SetAmp → SetDecay → AddShake)。
    target.SetShakeAmplitude(p.amplitude);
    target.SetShakeDecayRate(p.decay_rate);
    target.AddShake(p.trauma);
    return true;
}

/** 登録済みカスタムプリセット数を返す。 */
u32 FCameraShakePresets::CustomCount() const noexcept {
    return static_cast<u32>(m_Customs.Size());
}

} // namespace acs::game
