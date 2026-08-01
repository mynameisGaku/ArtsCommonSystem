// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — modelview / AModelMaterialPanel
//
// ModelViewer の中で、現在 load された model の各 submesh / material slot に
// 対して PBR マテリアル (base color / metallic / roughness / normal strength /
// AO strength / emissive 等) を **live 編集** するための ImGui ベースパネル。
// 編集された値は `FMaterialOverride` 構造体に保持され、コールバック経由で
// `FPbrShader` / `FStandardShader` の constant buffer に流し込まれる想定。
//
// 役割分担:
//   ・本パネルは **編集 UI + override 値の保持** のみを担当する。
//     実 shader への反映 (= constant buffer write) は呼び出し側 (ModelViewer
//     本体 or render integrator) が `MaterialChangeCallback` 経由で受け取った
//     `FMaterialOverride` を見て行う。これにより:
//       (1) shader / pipeline の選択 (FPbrShader / FStandardShader / FSkinnedShader
//           / FRefractionShader 等) は ModelViewer 本体が責任を持てる
//       (2) CUndoStack へ「override 1 件分」を atomic に push できる粒度を提供
//     できる。
//   ・slot 数の管理 (= model load 時に呼ばれる `SetMaterialSlotCount`) も
//     ModelViewer 本体が責任を持つ。本パネルは「slot N 個」という事実だけを
//     知って override TArray を resize する。
//
// 設計選択 (ModelViewer):
//   ・**AEditorPanel 基底を継承**: `editor_core::AEditorPanel`
//     のライフサイクル (OnInit / OnShutdown / OnFrameBegin / DrawUI / WantsFocus
//     等) を全て継承する。Workspace への登録 → 自動 dispatch が可能。
//   ・**非コピー / 非ムーブ**: 内部 `TArray<FMaterialOverride>` + callback 状態の
//     所有を曖昧にしない (= AEditorPanel の規約と同形、ACS 規約)。
//   ・**全 noexcept**: ACS 規約。範囲外 index は no-op、nullptr 取得は nullptr
//     return。例外は投げない。
//   ・**STL 不使用 / `<string>` 禁止**: override list は `acs::TArray<FMaterialOverride>`。
//     文字列は ImGui に渡すリテラル / スタック char[] のみ。
//   ・**ImGui ヘッダは .cpp に閉じ込め**: header からは imgui 依存を漏らさず、
//     AInspectorPanel / AParticleEditorPanel と同パターン。
//   ・**MaterialChangeCallback は raw 関数ポインタ + void***: ACS は STL の
//     std::function を使えないため、C スタイル callback 規約に揃える
//     (AInspectorPanel / AParticleEditorPanel と同形)。`override` 全体を 1 つの
//     コピーとして渡すことで、外部 (CUndoStack / 永続化) が
//     「変更前 → 変更後」の 1 件分を atomic に扱える。
//   ・**`is_overridden` フラグ**: 「ユーザーが触ったかどうか」を slot 単位で
//     保持する。false の slot は ModelViewer 側で「元の material そのまま」と
//     解釈する。ResetSlot で false に戻す。
//   ・**ColorEdit3/4 + SliderFloat の値域**:
//       Base Color (ColorEdit4)          : [0,1]^4 (= ImGui 既定)
//       Metallic / Roughness / AO       : [0, 1]
//       Normal Strength                 : [0, 2] (1.0 が neutral、< 1 で flatten、
//                                                > 1 で誇張)
//       Emissive (ColorEdit3)           : [0, 1]^3 (色そのもの)
//       Emissive Intensity              : [0, 10] (HDR / bloom 連動の強度)
//
// ImGui レイアウト (DrawUI):
//   ┌────────── "Material Override" window ──────────────────┐
//   │ Slot count: N                                          │
//   │ [Reset All]                                             │
//   │ ┌─ CollapsingHeader "Slot 0" ────────────────────────┐  │
//   │ │ [x] Override enabled                                │  │
//   │ │ Base Color    [ColorEdit4]                           │  │
//   │ │ Metallic     [SliderFloat]  0..1                    │  │
//   │ │ Roughness    [SliderFloat]  0..1                    │  │
//   │ │ Normal Str.  [SliderFloat]  0..2                    │  │
//   │ │ AO Strength  [SliderFloat]  0..1                    │  │
//   │ │ Emissive     [ColorEdit3]                           │  │
//   │ │ Em. Intens.  [SliderFloat]  0..10                   │  │
//   │ │ [Reset]                                              │  │
//   │ └──────────────────────────────────────────────────────┘  │
//   │ ┌─ CollapsingHeader "Slot 1" ────────────────────────┐  │
//   │ │  ...                                                │  │
//   └──────────────────────────────────────────────────────────┘
//
// 将来拡張余地:
//   ・texture override (BaseColorMap / NormalMap / ORM / Emissive 等の path swap、
//     CAssetBrowser からの drag-drop 連動)。`FMaterialOverride` に
//     `const char* base_color_path` 等を追加し、外部 callback で texture 差替を行う。
//   ・material preset library (= `.acs_matpreset` 1 ファイルに 1 set を保存し、
//     load / save / apply するボタン群)
//   ・shader variant 切替 (FPbrShader / FSkinnedShader / FRefractionShader 等を
//     drop-down で選び、対応する constant buffer に切替える)
//   ・per-slot min/max metadata 受け取り API (= 物理的に意味のある range で
//     SliderFloat を出す。アセットメタデータが充実したら活かす)
//   ・per-slot label 表示 (= 現状は "Slot 0" のような index 表示のみ。アセット
//     由来の material 名を SetSlotName で受け取れるようにする)
//
// 範囲外 (本パネルでは持たない):
//   ・実 shader への constant buffer write (= 外部 callback 受け側)
//   ・texture リソースの load / 解放 (= AssetManager 責務)
//   ・CUndoStack 統合 (= 外部 callback 経由で push してもらう)
//   ・material asset の永続化フォーマット (= MaterialPreset 等)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "math/Vec.h"

namespace acs::game::modelview {

/**
 * 1 つの material slot 分の PBR override データ。
 *
 * @details
 * is_overridden が false の slot は「元の material そのまま」を意味し、各値は
 * default (= 物理的に neutral な値) で初期化される。is_overridden が true になるのは
 * ユーザーが UI 上のフィールドを 1 つでも触った時点で、Reset ボタンで false に戻す。
 * FVec4 / FVec3 / f32 / bool / u32 は全 trivially-constructible なため、
 * TArray<FMaterialOverride> は Resize 時に MemSet ゼロ初期化される経路を安全に通る。
 */
struct FMaterialOverride {
    /**
     * slot 番号 (0..SlotCount()-1)。
     *
     * @details
     * TArray の index と一致する重複情報だが、callback 経由で外部に渡す際にもう一度
     * index を引き直さなくて済むよう構造体内にも持たせる (デバッグ視認性も上がる)。
     */
    u32  slot_index        = 0u;

    /**
     * PBR base color (RGBA)。
     *
     * @details
     * RGB は albedo / F0 tint、A は alpha cutout を含む将来拡張用 (現状は 1.0 で OK)。
     * default = white opaque。
     */
    FVec4 base_color        = FVec4{1.0f, 1.0f, 1.0f, 1.0f};

    /** metallic 係数 (0 = dielectric 非金属、1 = metal)。PBR の Metallic workflow に直結。 */
    f32  metallic          = 0.0f;

    /** roughness 係数 (0 = mirror smooth、1 = perfectly rough)。GGX の roughness にそのまま渡す。 */
    f32  roughness         = 0.5f;

    /**
     * normal map 影響度の倍率。
     *
     * @details
     * 1.0 で neutral (素のテクスチャ通り)、< 1 で flatten、> 1 で誇張。範囲外は
     * SliderFloat の clamp で防ぐ。
     */
    f32  normal_strength   = 1.0f;

    /** ambient occlusion 影響度 (1.0 で AO テクスチャをそのまま使う、0 で AO 無効)。 */
    f32  ao_strength       = 1.0f;

    /**
     * 自己発光色。
     *
     * @details
     * HDR scale 可。ColorEdit3 の値域は [0,1] だが、emissive_intensity との積で >1 に
     * 増幅される設計。default = 黒 (= 発光 OFF)。
     */
    FVec3 emissive          = FVec3{0.0f, 0.0f, 0.0f};

    /**
     * 発光強度倍率。
     *
     * @details
     * bloom / HDR pipeline で >1 の値で「光って見える」効果が出る。default = 1.0 だが、
     * emissive が黒なら結局 0。
     */
    f32  emissive_intensity = 1.0f;

    /**
     * ユーザーがこの slot を 1 度でも編集したか。
     *
     * @details
     * false ならば「元の material そのまま」を意味し、ModelViewer は override を無視して
     * 通常 material を使う。
     */
    bool is_overridden     = false;
};

/**
 * PBR material を slot 単位で live 編集する ImGui パネル (AEditorPanel 派生)。
 *
 * @details
 * 現在 load された model の各 submesh / material slot に対して base color / metallic /
 * roughness / normal strength / AO strength / emissive 等を編集し、値を
 * FMaterialOverride として保持する。実 shader への反映 (constant buffer write) は
 * 行わず、MaterialChangeCallback 経由で呼び出し側に override 1 件分を通知する。
 */
class AModelMaterialPanel : public editor_core::AEditorPanel {
public:
    /**
     * override 1 件分の変更通知 callback の型。
     *
     * @details
     * user は SetOnMaterialChangeCallback の第二引数で渡したポインタがそのまま戻る
     * (closure 代替)。渡される override は本パネルが保持している最新コピーで、callback
     * 受け側で const& として参照後すぐ消費するか、値コピーして CUndoStack に積む。
     * @param user SetOnMaterialChangeCallback で登録した任意ポインタ。
     * @param slot_index 変更があった slot 番号。
     * @param override 変更後の override (最新コピー)。
     */
    using MaterialChangeCallback = void (*)(void* user,
                                            u32 slot_index,
                                            const FMaterialOverride& override) noexcept;

    /** 空のパネルを構築する (slot list は空、callback 未登録)。 */
    AModelMaterialPanel() noexcept = default;

    /** パネルを破棄する (override list は TArray が解放)。 */
    ~AModelMaterialPanel() noexcept override = default;

    /** コピー禁止 (内部 TArray + callback 状態の所有を曖昧にしないため)。 */
    AModelMaterialPanel(const AModelMaterialPanel&)            = delete;

    /** コピー代入も禁止。 */
    AModelMaterialPanel& operator=(const AModelMaterialPanel&) = delete;

    /** ムーブ禁止 (AEditorPanel 基底と同規約)。 */
    AModelMaterialPanel(AModelMaterialPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AModelMaterialPanel& operator=(AModelMaterialPanel&&)      = delete;

    /**
     * パネルを初期化する (Workspace 登録前 / 単独利用時)。
     *
     * @details slot list を空に戻し、callback を維持したまま state をクリアする。多重 Init 可。
     */
    void Init() noexcept;

    /** 後片付けする (slot list を解放し callback を解除)。多重 Shutdown 可。 */
    void Shutdown() noexcept;

    /**
     * material slot 数を設定する (model load 時に呼ぶ)。
     *
     * @details
     * TArray を count 個に Resize し、各 slot の slot_index を 0..count-1 でフィルする。
     * 既存 slot の値は維持されない (= 古い model の override が新 model に流れない) ため
     * default 値で初期化される。count = 0 で全 slot 解除 (= モデルアンロード相当)。
     * @param count material slot 数 (kMaxSlots で clamp)。
     */
    void SetMaterialSlotCount(u32 count) noexcept;

    /**
     * 指定 slot を「元の material に戻す」(is_overridden = false + 各値を default に戻す)。
     *
     * @details 範囲外は no-op。callback も発火する (= 外部の shader CB を default に戻すきっかけ)。
     * @param slot_index リセットする slot 番号。
     */
    void ResetSlot(u32 slot_index) noexcept;

    /** 全 slot を ResetSlot 相当でリセットする。callback は slot 数だけ発火する。 */
    void ResetAll() noexcept;

    /**
     * 指定 slot が override 状態かを返す。
     *
     * @param slot_index 問い合わせる slot 番号。
     * @return override 状態なら true (範囲外は false)。
     */
    bool IsSlotOverridden(u32 slot_index) const noexcept;

    /**
     * 指定 slot の override を返す (const)。
     *
     * @param slot_index 取得する slot 番号。
     * @return slot の override (範囲外 / 未 Init は nullptr)。
     */
    const FMaterialOverride* GetOverride(u32 slot_index) const noexcept;

    /**
     * 指定 slot の override を返す (mutable)。
     *
     * @details
     * 外部から書き換える際は呼び出し側で is_overridden = true を立てるか、値変更後に
     * callback を手動で呼ぶ責務がある (= panel 自身は変更検知しない)。
     * @param slot_index 取得する slot 番号。
     * @return slot の override (範囲外 / 未 Init は nullptr)。
     */
    FMaterialOverride* GetOverrideMutable(u32 slot_index) noexcept;

    /**
     * 現在の slot 数を返す。
     *
     * @return SetMaterialSlotCount の最終値。
     */
    u32 SlotCount() const noexcept;

    /**
     * material 変更通知 callback を登録する (nullptr で解除)。
     *
     * @details
     * 発火タイミングは DrawUI 内で 1 つでも field が変わったとき + ResetSlot / ResetAll 時。
     * 1 度の draw frame で複数 slot が変わると複数回呼ばれる。
     * @param cb 登録する callback (nullptr で解除)。
     * @param user callback に渡す任意ポインタ。
     */
    void SetOnMaterialChangeCallback(MaterialChangeCallback cb, void* user) noexcept;

    /**
     * window タイトルを返す (ImGui ID + Window メニュー表記)。
     *
     * @return 固定リテラル "Material Override"。
     */
    const char* Title() const noexcept override { return "Material Override"; }

    /**
     * メイン ImGui window を描画する。
     *
     * @details ImGui::Begin("Material Override", &m_Visible) で start し、各 slot を CollapsingHeader 内に展開する。
     */
    void DrawUI() noexcept override;

    /** Metallic SliderFloat の下限。 */
    static constexpr f32 kMinMetallic         = 0.0f;

    /** Metallic SliderFloat の上限。 */
    static constexpr f32 kMaxMetallic         = 1.0f;

    /** Roughness SliderFloat の下限。 */
    static constexpr f32 kMinRoughness        = 0.0f;

    /** Roughness SliderFloat の上限。 */
    static constexpr f32 kMaxRoughness        = 1.0f;

    /** Normal Strength SliderFloat の下限。 */
    static constexpr f32 kMinNormalStrength   = 0.0f;

    /** Normal Strength SliderFloat の上限。 */
    static constexpr f32 kMaxNormalStrength   = 2.0f;

    /** AO Strength SliderFloat の下限。 */
    static constexpr f32 kMinAoStrength       = 0.0f;

    /** AO Strength SliderFloat の上限。 */
    static constexpr f32 kMaxAoStrength       = 1.0f;

    /** Emissive Intensity SliderFloat の下限。 */
    static constexpr f32 kMinEmissiveIntensity = 0.0f;

    /** Emissive Intensity SliderFloat の上限。 */
    static constexpr f32 kMaxEmissiveIntensity = 10.0f;

    /**
     * slot 上限 (UI と内部の安全弁)。
     *
     * @details
     * SetMaterialSlotCount に大きすぎる値を渡しても、ここで clamp される
     * (= 不正な model でも UI が暴走しない)。
     */
    static constexpr u32 kMaxSlots = 256u;

private:
    /**
     * 変更通知 callback を発火する (登録されていれば 1 回呼ぶ)。
     *
     * @param slot_index 変更があった slot 番号。
     */
    void FireChangeCallback(u32 slot_index) noexcept;

    /** override list (slot index と 1:1)。SetMaterialSlotCount で Resize。 */
    TArray<FMaterialOverride> m_Overrides {};

    /** 変更通知 callback (nullptr 許容)。 */
    MaterialChangeCallback  m_OnChangeCb   = nullptr;

    /** callback に渡す任意ポインタ。 */
    void*                   m_OnChangeUser = nullptr;
};

using FModelMaterialPanel = AModelMaterialPanel;

} // namespace acs::game::modelview
