// SPDX-License-Identifier: Apache-2.0
// CCameraShakePresets
//
// CCamera2D (= IShakeTarget) に対して「爆発 / 地震 / 着弾」等のジャンル別
// shake パラメータを 1 行で流し込む薄い preset ライブラリ。Eiserloh trauma
// 方式 (Camera2D.h を参照) の amplitude / decay_rate / frequency / 適正
// duration を、ゲームコードに magic number を書かずに済むよう名前付きで
// 提供する。
//
// 使い方:
//   class FGameplayScene : public AScene {
//       void OnEnter() noexcept override {
//           // CCamera2D が IShakeTarget を派生していれば直接渡せる:
//           // CCameraShakePresets::ApplyPreset(Services().Camera(),
//           //                                 EShakePreset::ExplosionLarge);
//       }
//   };
//
//   // カスタム preset 登録:
//   CCameraShakePresets presets;
//   presets.RegisterCustomPreset("BossSlam",
//       FShakeParams{0.7f, 1.0f, 0.8f, 18.0f, 1.2f});
//   presets.ApplyCustomByName(target, "BossSlam");
//
// 設計選択:
//   ・**IShakeTarget seam**: CCameraShakePresets は CCamera2D に直接依存しない。
//     trauma を吸う側を `IShakeTarget` 抽象で受けることで、CCamera2D が
//     IShakeTarget を派生していなくても spec として正しい形を維持できる。
//     テスト用の MockShakeTarget でユニットテストも書ける。
//   ・**preset 値は static const 関数で配る**: 単純な定数なので constexpr
//     にしたい所だが、Custom enum case + 拡張 (EShakePreset を増やす)
//     を見越して関数経由に統一。コンパイラは余裕で fold する。
//   ・**Custom preset は名前 + FShakeParams を TArray に保持**: 件数は典型 0〜
//     20 程度なので線形検索。const char* は呼び出し側が保証する static
//     lifetime 想定 (CAchievementManager / CEntitlementRegistry と同設計、STL <string>
//     不使用)。
//   ・**ApplyPreset は trauma を AddShake で「加算」する**: SetShake* で
//     amplitude / decay を上書きしたあと、trauma を AddShake で累積する。
//     これによりトリガーが重なった時 (= 連続ヒット) trauma 蓄積で派手になる
//     Eiserloh 流の挙動を維持する。
//   ・**duration_hint は CCameraShakePresets 自身は使わない**: 単なる "この
//     trauma + decay だと約何秒で減衰しきる" の参考値。caller が UI 演出や
//     SFX と尺合わせするためのヒント。decay の逆算式は trauma / decay_rate
//     (秒)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 preset/manager 系と統一。
//
// 範囲外:
//   ・preset 間 blending (爆発中に地震が来た時、別軸で合成する等)
//   ・directional shake (現状は等方ノイズ、方向ベクトルは CCamera2D 拡張で)
//   ・perlin/curl-noise ベース shake (現状は sin/cos 直流回避 noise)
//   ・preset の JSON 等からの読み込み (現状は C++ 即値ベース)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * shake プリセットの種別。
 *
 * @details
 * trauma 加算 + amp/decay 上書きで「爆発っぽい」「地震っぽい」を一発で出すための
 * プリセット種別。Custom は名前付きカスタムを表す哨兵 (GetPreset では中立値を返す)。
 */
enum class EShakePreset : u8 {
    /** 手榴弾 / 小爆弾。短く激しく。 */
    ExplosionSmall = 0,

    /** 大爆発 / ロケット直撃。長く強く。 */
    ExplosionLarge,

    /** ステージギミック型短震動。 */
    EarthquakeShort,

    /** 環境演出型長震動 (周波数低)。 */
    EarthquakeLong,

    /** 攻撃ヒット / 被弾。ごく短い punch。 */
    HitImpact,

    /** 連続発射型の継続震動 (高周波、低 amp、低 decay)。 */
    RocketLaunch,

    /** 隕石着弾 / ボス登場。最強クラス。 */
    MeteorImpact,

    /** RegisterCustomPreset 経由で登録された名前付き (哨兵)。 */
    Custom,
};

/**
 * shake プリセット 1 個分のパラメータ。
 *
 * @details
 * trauma を AddShake、amplitude を SetShakeAmplitude、decay_rate を SetShakeDecayRate に流す。
 * frequency と duration_hint は CCamera2D 側 API には現状反映されない (frequency は将来の
 * SetShakeFrequency 化のための予約、duration_hint は caller のヒント)。
 */
struct FShakeParams {
    /** AddShake に渡す trauma 量 (0..1 想定、preset では 0.3..0.9)。 */
    f32 trauma         = 0.0f;

    /** SetShakeAmplitude に渡す振幅 (world units max @ trauma=1)。 */
    f32 amplitude      = 0.5f;

    /** SetShakeDecayRate に渡す減衰量 (trauma を 1 秒で 1.0 → 0.0 にする値)。 */
    f32 decay_rate     = 1.0f;

    /** 主要振動周波数 (CCamera2D 拡張予約。現状 API には反映されない)。 */
    f32 frequency      = 25.0f;

    /** 約何秒で減衰しきるかの目安 (= trauma / decay_rate)。caller のヒント。 */
    f32 duration_hint  = 1.0f;
};

/**
 * shake パラメータの流し込み先となる純粋仮想インターフェース。
 *
 * @details
 * CCamera2D がこれを派生する (AddShake / SetShakeAmplitude / SetShakeDecayRate と同シグネチャ)。
 * テスト用 mock も同じ I/F を実装することで CCameraShakePresets を CCamera2D に直接依存させない。
 */
class IShakeTarget {
public:
    /** 空状態で構築する。 */
    IShakeTarget() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IShakeTarget() noexcept = default;

    /** コピー禁止。 */
    IShakeTarget(const IShakeTarget&)            = delete;

    /** コピー代入も禁止。 */
    IShakeTarget& operator=(const IShakeTarget&) = delete;

    /** ムーブ禁止。 */
    IShakeTarget(IShakeTarget&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IShakeTarget& operator=(IShakeTarget&&)      = delete;

    /**
     * trauma を加算する (clamp [0,1] は実装側責務)。
     *
     * @param trauma 加算する trauma 量。
     */
    virtual void AddShake(f32 trauma) noexcept = 0;

    /**
     * 振幅を上書きする。
     *
     * @param a 振幅 (world units max @ trauma=1)。
     */
    virtual void SetShakeAmplitude(f32 a) noexcept = 0;

    /**
     * trauma 減衰レートを上書きする。
     *
     * @param r 減衰量 (trauma を 1 秒で 1.0 → 0.0 にする値)。
     */
    virtual void SetShakeDecayRate(f32 r) noexcept = 0;
};

/**
 * ジャンル別 shake パラメータを IShakeTarget に 1 行で流し込む薄い preset ライブラリ。
 *
 * @details
 * Eiserloh trauma 方式の amplitude / decay_rate / frequency / 適正 duration を名前付きで
 * 提供する。組み込み preset は static 関数で配り、カスタム preset は名前 + FShakeParams を
 * TArray に保持して線形検索する (名前は caller の static lifetime 想定)。非コピー・非ムーブ。
 */
class CCameraShakePresets {
public:
    /** 空のプリセットライブラリを構築する。 */
    CCameraShakePresets()  noexcept = default;

    /** 破棄する。 */
    ~CCameraShakePresets() noexcept = default;

    /** コピー禁止 (他 preset/manager 系と統一)。 */
    CCameraShakePresets(const CCameraShakePresets&)            = delete;

    /** コピー代入も禁止。 */
    CCameraShakePresets& operator=(const CCameraShakePresets&) = delete;

    /** ムーブ禁止。 */
    CCameraShakePresets(CCameraShakePresets&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CCameraShakePresets& operator=(CCameraShakePresets&&)      = delete;

    /**
     * 組み込み preset の FShakeParams を返す。
     *
     * @param preset 取得するプリセット種別。
     * @return 対応する FShakeParams。Custom は中立値 (= 適用しても見た目に影響しない値)。
     */
    static FShakeParams GetPreset(EShakePreset preset) noexcept;

    /**
     * 組み込み preset を target に流し込む。
     *
     * @details SetShakeAmplitude → SetShakeDecayRate → AddShake (加算累積) の順で適用する。
     * @param target 流し込み先の shake ターゲット。
     * @param preset 適用するプリセット種別。Custom は no-op (caller は ApplyCustomByName を使う)。
     */
    static void ApplyPreset(IShakeTarget& target, EShakePreset preset) noexcept;

    /**
     * カスタム preset を登録する。
     *
     * @details 同 name の 2 重登録は黙って上書きする (アセット二重ロード時に直近が勝つ)。
     * @param name プリセット名。caller が保証する static lifetime の文字列 (リテラル想定)。nullptr は no-op。
     * @param params 登録する shake パラメータ。
     */
    void RegisterCustomPreset(const char* name, const FShakeParams& params) noexcept;

    /**
     * 名前で引いたカスタム preset を target に流し込む。
     *
     * @details ApplyPreset と同じ順序 (SetAmp → SetDecay → AddShake) で適用する。
     * @param target 流し込み先の shake ターゲット。
     * @param name 適用するカスタムプリセット名。
     * @return 発見 + 適用できれば true。未登録 / name == nullptr は false。
     */
    bool ApplyCustomByName(IShakeTarget& target, const char* name) noexcept;

    /**
     * 登録済みカスタムプリセット数を返す。
     *
     * @return カスタムプリセットの件数。
     */
    u32 CustomCount() const noexcept;

private:
    /**
     * 登録済みカスタムプリセット 1 件分のエントリ。
     */
    struct FCustomEntry {
        /** プリセット名 (所有しない。caller の static lifetime)。 */
        const char* name = nullptr;

        /** 紐付く shake パラメータ。 */
        FShakeParams params{};
    };

    /**
     * 名前でカスタムプリセットのインデックスを線形検索する (件数は典型 < 20)。
     *
     * @param name 検索するプリセット名。
     * @return m_Customs 内のインデックス (見つからなければ ~0u)。
     */
    u32 FindCustomIndex(const char* name) const noexcept;

    /** 登録済みカスタムプリセットの配列。 */
    TArray<FCustomEntry> m_Customs;
};

/** 旧名を使う既存ソースとの互換alias。 */
using FCameraShakePresets = CCameraShakePresets;

} // namespace acs::game
