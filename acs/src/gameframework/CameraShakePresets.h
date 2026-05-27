// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar E Phase 3 — FCameraShakePresets
//
// FCamera2D (= IShakeTarget) に対して「爆発 / 地震 / 着弾」等のジャンル別
// shake パラメータを 1 行で流し込む薄い preset ライブラリ。Eiserloh trauma
// 方式 (FCamera2D.h を参照) の amplitude / decay_rate / frequency / 適正
// duration を、ゲームコードに magic number を書かずに済むよう名前付きで
// 提供する。
//
// 使い方:
//   class GameplayScene : public Scene {
//       void OnEnter() noexcept override {
//           // (Phase 3+ で FCamera2D が IShakeTarget を派生したら直接渡せる)
//           // FCameraShakePresets::ApplyPreset(Services().Camera(),
//           //                                 EShakePreset::ExplosionLarge);
//       }
//   };
//
//   // カスタム preset 登録:
//   FCameraShakePresets presets;
//   presets.RegisterCustomPreset("BossSlam",
//       ShakeParams{0.7f, 1.0f, 0.8f, 18.0f, 1.2f});
//   presets.ApplyCustomByName(target, "BossSlam");
//
// 設計選択 (Phase 3):
//   ・**IShakeTarget seam**: FCameraShakePresets は FCamera2D に直接依存しない。
//     trauma を吸う側を `IShakeTarget` 抽象で受けることで、Phase 3+ で
//     FCamera2D が IShakeTarget を派生するまでの間も spec として正しい
//     形を維持できる。テスト用の MockShakeTarget でユニットテストも書ける。
//   ・**preset 値は static const 関数で配る**: 単純な定数なので constexpr
//     にしたい所だが、Custom enum case + 拡張 (将来 EShakePreset を増やす)
//     を見越して関数経由に統一。コンパイラは余裕で fold する。
//   ・**Custom preset は名前 + ShakeParams を TArray に保持**: 件数は典型 0〜
//     20 程度なので線形検索。const char* は呼び出し側が保証する static
//     lifetime 想定 (FAchievementManager / FEntitlement と同設計、STL <string>
//     不使用)。
//   ・**ApplyPreset は trauma を AddShake で「加算」する**: SetShake* で
//     amplitude / decay を上書きしたあと、trauma を AddShake で累積する。
//     これによりトリガーが重なった時 (= 連続ヒット) trauma 蓄積で派手になる
//     Eiserloh 流の挙動を維持する。
//   ・**duration_hint は FCameraShakePresets 自身は使わない**: 単なる "この
//     trauma + decay だと約何秒で減衰しきる" の参考値。caller が UI 演出や
//     SFX と尺合わせするためのヒント。decay の逆算式は trauma / decay_rate
//     (秒)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 preset/manager 系と統一。
//
// 範囲外 (Phase 4+ で):
//   ・preset 間 blending (爆発中に地震が来た時、別軸で合成する等)
//   ・directional shake (現状は等方ノイズ、方向ベクトルは FCamera2D 拡張で)
//   ・perlin/curl-noise ベース shake (現状は sin/cos 直流回避 noise)
//   ・preset の JSON 等からの読み込み (Phase 3 は C++ 即値ベースで十分)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- EShakePreset enum -----------------------------------------------------
// trauma 加算 + amp/decay 上書きで「爆発っぽい」「地震っぽい」を一発で出す
// ためのプリセット種別。Custom は名前付きカスタムを表す哨兵 (GetPreset では
// 中立値を返す)。
enum class EShakePreset : u8 {
    ExplosionSmall = 0,   // 手榴弾 / 小爆弾。短く激しく。
    ExplosionLarge,       // 大爆発 / ロケット直撃。長く強く。
    EarthquakeShort,      // ステージギミック型短震動。
    EarthquakeLong,       // 環境演出型長震動 (周波数低)。
    HitImpact,            // 攻撃ヒット / 被弾。ごく短い punch。
    RocketLaunch,         // 連続発射型の継続震動 (高周波、低 amp、低 decay)。
    MeteorImpact,         // 隕石着弾 / ボス登場。最強クラス。
    Custom,               // RegisterCustomPreset 経由で登録された名前付き。
};

// ---- ShakeParams ----------------------------------------------------------
// preset 1 個分の shake パラメータ。trauma を AddShake、amplitude を
// SetShakeAmplitude、decay_rate を SetShakeDecayRate に流す。frequency と
// duration_hint は FCamera2D 側 API には現状反映されない (frequency は
// FCamera2D 実装 (Tick 内 25.0f hard-coded) を将来 SetShakeFrequency 化する
// ための予約、duration_hint は caller のヒント)。
struct ShakeParams {
    f32 trauma         = 0.0f;   // AddShake(amount) — 0..1 想定。preset では 0.3..0.9。
    f32 amplitude      = 0.5f;   // SetShakeAmplitude — world units max @ trauma=1。
    f32 decay_rate     = 1.0f;   // SetShakeDecayRate — trauma を 1 秒で 1.0 → 0.0 にする値。
    f32 frequency      = 25.0f;  // 主要振動周波数 (FCamera2D 拡張予約)。
    f32 duration_hint  = 1.0f;   // 約何秒で減衰しきるかの目安 = trauma / decay_rate。
};

// ---- IShakeTarget seam ----------------------------------------------------
// FCameraShakePresets が trauma 値を流し込む先の純粋仮想インターフェース。
// FCamera2D は Phase 3+ でこれを派生する (現状の AddShake / SetShakeAmplitude
// / SetShakeDecayRate と同シグネチャ)。テスト用 mock も同じ I/F を実装する。
class IShakeTarget {
public:
    IShakeTarget() noexcept = default;
    virtual ~IShakeTarget() noexcept = default;

    IShakeTarget(const IShakeTarget&)            = delete;
    IShakeTarget& operator=(const IShakeTarget&) = delete;
    IShakeTarget(IShakeTarget&&)                 = delete;
    IShakeTarget& operator=(IShakeTarget&&)      = delete;

    // trauma を amount だけ累積 (clamp [0,1] は実装側責務)。
    virtual void AddShake(f32 trauma) noexcept = 0;

    // amplitude を上書き (world units max @ trauma=1)。
    virtual void SetShakeAmplitude(f32 a) noexcept = 0;

    // decay_rate を上書き (trauma を 1 秒で 1.0 → 0.0 にする値)。
    virtual void SetShakeDecayRate(f32 r) noexcept = 0;
};

// ---- FCameraShakePresets ---------------------------------------------------
class FCameraShakePresets {
public:
    FCameraShakePresets()  noexcept = default;
    ~FCameraShakePresets() noexcept = default;

    FCameraShakePresets(const FCameraShakePresets&)            = delete;
    FCameraShakePresets& operator=(const FCameraShakePresets&) = delete;
    FCameraShakePresets(FCameraShakePresets&&)                 = delete;
    FCameraShakePresets& operator=(FCameraShakePresets&&)      = delete;

    // ---- 組み込み preset 取得 -------------------------------------------
    // preset 用の trauma/amplitude/decay/freq 値を返す。Custom が渡された
    // 場合は中立値 (= ZeroParams 相当) を返す (Custom は名前経由で
    // ApplyCustomByName 専用)。
    static ShakeParams GetPreset(EShakePreset preset) noexcept;

    // ---- 組み込み preset 適用 -------------------------------------------
    // preset を target に流し込む。順序:
    //   1) SetShakeAmplitude(params.amplitude)
    //   2) SetShakeDecayRate(params.decay_rate)
    //   3) AddShake(params.trauma)  ← 加算 (累積)
    // Custom が渡された場合は no-op (caller は ApplyCustomByName を使う)。
    static void ApplyPreset(IShakeTarget& target, EShakePreset preset) noexcept;

    // ---- カスタム preset 登録 -------------------------------------------
    // name は呼び出し側が保証する static lifetime の文字列 (リテラル想定)。
    // 同 name の 2 重登録は黙って上書き (アセット二重ロード時に直近が勝つ)。
    // name == nullptr は no-op (defensive)。
    void RegisterCustomPreset(const char* name, const ShakeParams& params) noexcept;

    // 名前で引いた ShakeParams を target に流し込む。同 ApplyPreset と
    // 同じ順序 (SetAmp → SetDecay → AddShake) で適用する。
    // 戻り値: true = 発見 + 適用、false = 未登録 / name == nullptr。
    bool ApplyCustomByName(IShakeTarget& target, const char* name) noexcept;

    // 登録済みカスタムプリセット数。
    u32 CustomCount() const noexcept;

private:
    struct CustomEntry {
        const char* name = nullptr;   // 所有しない (caller の static lifetime)
        ShakeParams params{};
    };

    // 名前で線形検索 (件数は典型 < 20)。見つからなければ ~0u を返す。
    u32 FindCustomIndex(const char* name) const noexcept;

    TArray<CustomEntry> _customs;
};

} // namespace acs::game
