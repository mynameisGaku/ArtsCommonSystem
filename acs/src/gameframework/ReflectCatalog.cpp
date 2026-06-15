// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — エンジン同梱型のリフレクションカタログ (本体)
// -----------------------------------------------------------------------------
// 既存のエンジン型を「型ヘッダ非侵襲」で FTypeRegistry へ一括登録する単一の TU。
// 各型のヘッダを include し、ACS_REGISTER*（AcsTypeDescOf 特殊化を生成しない版）を
// 並べるだけ。これにより editor / serializer / script / network が「型を知らずに」
// FindByName / カテゴリ列挙 / factory でエンジン型を横断利用できる。
//
// 方針:
//   ・**型レベル登録が主**。Component/System 等は仮想関数を持ち非 standard-layout で
//     private メンバが多いため offsetof フィールド反射は今は付けない (名前・カテゴリ・
//     サイズ・align・traits・factory を登録)。フィールドは安全に確認しながら後付けする。
//   ・**enum は値反射**（offsetof 不要で安全）。各 enum は定義の全列挙子を登録する
//     (value↔name の双方向解決が全値で成立する。serializer / editor が欠落値で破綻しない)。
//   ・render / asset の型は名前空間 acs 直下なので、マクロが開く acs::game の親探索で
//     短名解決される (using 不要、レジストリ名も短名)。
// =============================================================================
#include "gameframework/ReflectCatalog.h"
#include "gameframework/Reflect.h"

// ----- Component (FComponent2D 派生) -----
#include "gameframework/Sprite2DComponent.h"
#include "gameframework/SpriteAnimComponent.h"
#include "gameframework/TriggerComponent.h"
#include "gameframework/TilemapComponent.h"
#include "gameframework/PhysicsBody2D.h"
#include "gameframework/RigidBody2D.h"          // FRigidBody2D (剛体ボディ・コンポーネント)
#include "gameframework/PrimitiveRenderer2D.h"  // FPrimitiveRenderer2D (形状描画 + コライダー形状)
#include "gameframework/Effects2D.h"   // FWater2D / FFire2D / FTrail2D / FStencilClip2D
#include "gameframework/Light2DComponent.h"  // FLight2DComponent (2D 点光源)
#include "gameframework/Follow2DComponent.h"  // FFollow2DComponent (オブジェクト参照デモ)

// ----- Scene -----
#include "gameframework/Scene2D.h"

// ----- System / Director / Manager -----
#include "gameframework/HealthSystem.h"
#include "gameframework/BuffSystem.h"
#include "gameframework/ScoreSystem.h"
#include "gameframework/WeaponSystem.h"
#include "gameframework/ProjectileSystem.h"
#include "gameframework/PickupSystem.h"
#include "gameframework/InventorySystem.h"
#include "gameframework/CooldownTimer.h"
#include "gameframework/WaveSpawner.h"
#include "gameframework/CombatStateMachine.h"   // FCombatStateMachine + ECombatState
#include "gameframework/AssetBundle.h"           // FStreamingDirector が前方宣言で参照 (完全型化)
#include "gameframework/AchievementManager.h"
#include "gameframework/EffectSystem.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/AmbientDirector.h"
#include "gameframework/AudioDirector.h"
#include "gameframework/MusicDirector.h"
#include "gameframework/CinematicsDirector.h"
#include "gameframework/PauseDirector.h"
#include "gameframework/LocalizationDirector.h"
#include "gameframework/EconomyDirector.h"
#include "gameframework/CheckpointSystem.h"
#include "gameframework/CraftingSystem.h"
#include "gameframework/DeckSystem.h"
#include "gameframework/DialogueSystem.h"
#include "gameframework/HungerSystem.h"
#include "gameframework/PartySystem.h"
#include "gameframework/PrefabSystem.h"
#include "gameframework/PrivacyDirector.h"
#include "gameframework/ReplayDirector.h"
#include "gameframework/StreamingDirector.h"
#include "gameframework/TelemetryDirector.h"
#include "gameframework/TurnManager.h"
#include "gameframework/WeatherSystem.h"
#include "gameframework/Tween.h"   // FTweenManager

// ----- Asset 型 -----
#include "asset/Asset.h"           // EAssetState
#include "asset/ImageAsset.h"      // FImageAsset + EPixelFormat
#include "asset/AudioAsset.h"      // FAudioAsset + ESampleFormat
#include "asset/MeshAsset.h"       // FMeshAsset
#include "asset/TextAsset.h"       // FTextAsset
#include "asset/BinaryAsset.h"     // FBinaryAsset
#include "asset/SkinnedMesh.h"     // FSkinnedMeshAsset

// ----- enum 提供ヘッダ -----
#include "gameframework/BehaviorTree.h"   // EBtStatus / EBtDecoratorOp / EBtVarType / EBtCompareOp
#include "gameframework/SpriteAnimator.h" // EPlayMode
#include "render/IRhiPipeline.h"          // EBlendMode / ECullMode / ECompareFunc
#include "render/RhiTypes.h"              // EPrimitiveTopology / EResourceState

// =============================================================================
// 登録 (グローバルスコープ。各マクロが内部で namespace acs::game を開く)
// =============================================================================

// ----- Component -----
// 編集可能フィールドは ACS_RPROP_*（名前のみ反射、offsetof 不使用）でスキーマ化する。
// Component は多態 + private メンバで offsetof 反射できないため、各型の public setter に
// 対応する「名前・種別・既定値」だけを反射し、値はエディタ側がスキーマ順に保持する。
// 既定値は各コンポーネントのメンバ初期化子に一致させている。
ACS_REGISTER_COMPONENT(FSprite2DComponent,
    ACS_RPROP_V2("size",  1.0f, 1.0f),                 // m_Size{1,1}
    ACS_RPROP_V4("tint",  1.0f, 1.0f, 1.0f, 1.0f),     // m_Tint{1,1,1,1} (RGBA)
    ACS_RPROP_V2("pivot", 0.5f, 0.5f))                 // m_Pivot{0.5,0.5}
ACS_REGISTER_COMPONENT(FSpriteAnimComponent)
ACS_REGISTER_COMPONENT(FTriggerComponent)        // ctor が world& 必須 → Abstract (factory なし)
ACS_REGISTER_COMPONENT(FTilemapComponent)
ACS_REGISTER_COMPONENT(FPhysicsBody2D)           // ctor が world& 必須 → Abstract (kinematic swept)
// プリミティブ描画 (形状を選ぶ。コライダー形状もこの shape に合わせる)。
// shape: 0=Box, 1=Circle, 2=Triangle。
ACS_REGISTER_COMPONENT(FPrimitiveRenderer2D,
    ACS_RPROP_I("shape", 0))                // 0=Box, 1=Circle, 2=Triangle
// 剛体ボディ (現実的な剛体物理の dynamics。形状は FPrimitiveRenderer2D の shape を使う)。
// bodyType: 0=Static, 1=Dynamic。値はエディタが保持し Play で読む。
ACS_REGISTER_COMPONENT(FRigidBody2D,
    ACS_RPROP_I("bodyType",       1),       // 0=Static, 1=Dynamic
    ACS_RPROP_F("restitution",    0.1f),    // 弾性 (反発係数 [0,1])
    ACS_RPROP_F("friction",       0.5f),    // 摩擦係数
    ACS_RPROP_F("mass",           1.0f),    // 質量 (Dynamic のみ。<=0 で静的扱い)
    ACS_RPROP_F("linearDamping",  0.05f),   // 線形減衰
    ACS_RPROP_F("angularDamping", 0.1f))    // 角減衰
ACS_REGISTER_COMPONENT(FWater2DComponent,
    ACS_RPROP_V3("color",     0.12f, 0.35f, 0.60f),    // m_Color
    ACS_RPROP_F ("alpha",     0.72f),                  // m_Alpha
    ACS_RPROP_F ("waveAmp",   0.12f),                  // m_Amp
    ACS_RPROP_F ("waveSpeed", 1.6f))                   // m_Speed
ACS_REGISTER_COMPONENT(FFire2DComponent,
    ACS_RPROP_V2("size",      0.8f, 1.6f),             // m_W, m_H (SetSize)
    ACS_RPROP_F ("intensity", 1.0f))                   // m_Intensity
ACS_REGISTER_COMPONENT(FTrail2DComponent,
    ACS_RPROP_V3("color",  0.3f, 0.8f, 1.0f),          // m_Color
    ACS_RPROP_F ("width",  0.25f),                     // m_Width
    ACS_RPROP_I ("points", 24))                        // m_Max (SetPoints)
ACS_REGISTER_COMPONENT(FStencilClip2DComponent)
// 2D 点光源 (lit スプライト/PBR マテリアルを照らす)。位置は owner ノードの world 位置。
// public メンバ + 単一継承なので ACS_RFIELD_D で offset 反射 → 実体へ値が適用される
// (standalone/Play でも authored 値で光る = エディタと一致)。フィールド順 = radius/color/intensity。
ACS_REGISTER_COMPONENT(FLight2DComponent,
    ACS_RFIELD_D(FLight2DComponent, m_Radius,    EFieldKind::F32,   256.0f, 0.0f, 0.0f, 0.0f),
    ACS_RFIELD_D(FLight2DComponent, m_Color,     EFieldKind::FVec3, 1.0f, 0.95f, 0.85f, 0.0f),
    ACS_RFIELD_D(FLight2DComponent, m_Intensity, EFieldKind::F32,   1.5f, 0.0f, 0.0f, 0.0f),
    ACS_RFIELD_D(FLight2DComponent, m_Type,      EFieldKind::F32,   0.0f, 0.0f, 0.0f, 0.0f),    // 0=Point,1=Spot,2=Directional
    ACS_RFIELD_D(FLight2DComponent, m_Angle,     EFieldKind::F32,   90.0f, 0.0f, 0.0f, 0.0f),   // 方向角(度)
    ACS_RFIELD_D(FLight2DComponent, m_ConeAngle, EFieldKind::F32,   35.0f, 0.0f, 0.0f, 0.0f))   // スポット半角(度)
// 影を落とすタグ (このノードのシルエットが lit スプライトに円ソフト影を落とす)。
ACS_REGISTER_COMPONENT(FShadowCaster2DComponent,
    ACS_RFIELD_D(FShadowCaster2DComponent, m_RadiusScale, EFieldKind::F32, 1.0f, 0.0f, 0.0f, 0.0f),
    ACS_RFIELD_D(FShadowCaster2DComponent, m_Shape,       EFieldKind::F32, 0.0f, 0.0f, 0.0f, 0.0f),  // 0=円,1=箱
    ACS_RFIELD_D(FShadowCaster2DComponent, m_SelfShadow,  EFieldKind::Bool, 0.0f, 0.0f, 0.0f, 0.0f))  // false=自己影スキップ (既定)
// 追従コンポーネント。target は «オブジェクト参照»(他ノードへの参照を渡す UPROPERTY 風)、
// arrived は «読み取り専用»(VisibleAnywhere 風 = FIELD_READONLY)のデモ。
ACS_REGISTER_COMPONENT(FFollow2DComponent,
    ACS_RFIELD_REF(FFollow2DComponent, target),                             // 参照先ノード ID (ピッカーで設定)
    ACS_RFIELD_D(FFollow2DComponent, speed, EFieldKind::F32, 3.0f, 0.0f, 0.0f, 0.0f),
    ACS_RFIELD_DF(FFollow2DComponent, arrived, EFieldKind::Bool, FIELD_READONLY, 0.0f, 0.0f, 0.0f, 0.0f))

// ----- Scene -----
ACS_REGISTER_SCENE(FScene2D)

// ----- Asset -----
ACS_REGISTER_ASSET(FImageAsset)
ACS_REGISTER_ASSET(FAudioAsset)
ACS_REGISTER_ASSET(FMeshAsset)
ACS_REGISTER_ASSET(FTextAsset)
ACS_REGISTER_ASSET(FBinaryAsset)
ACS_REGISTER_ASSET(FSkinnedMeshAsset)

// ----- System / Director / Manager -----
ACS_REGISTER_SYSTEM(FHealthSystem)
ACS_REGISTER_SYSTEM(FBuffSystem)
ACS_REGISTER_SYSTEM(FScoreSystem)
ACS_REGISTER_SYSTEM(FWeaponSystem)
ACS_REGISTER_SYSTEM(FProjectileSystem)
ACS_REGISTER_SYSTEM(FPickupSystem)
ACS_REGISTER_SYSTEM(FInventorySystem)
ACS_REGISTER_SYSTEM(FCooldownTimer)
ACS_REGISTER_SYSTEM(FWaveSpawner)
ACS_REGISTER_SYSTEM(FCombatStateMachine)
ACS_REGISTER_SYSTEM(FAchievementManager)
ACS_REGISTER_SYSTEM(FEffectSystem)
ACS_REGISTER_SYSTEM(FParticleEffectSystem)
ACS_REGISTER_SYSTEM(FAmbientDirector)
ACS_REGISTER_SYSTEM(FAudioDirector)
ACS_REGISTER_SYSTEM(FMusicDirector)
ACS_REGISTER_SYSTEM(FCinematicsDirector)
ACS_REGISTER_SYSTEM(FPauseDirector)
ACS_REGISTER_SYSTEM(FLocalizationDirector)
ACS_REGISTER_SYSTEM(FEconomyDirector)
ACS_REGISTER_SYSTEM(FCheckpointSystem)
ACS_REGISTER_SYSTEM(FCraftingSystem)
ACS_REGISTER_SYSTEM(FDeckSystem)
ACS_REGISTER_SYSTEM(FDialogueSystem)
ACS_REGISTER_SYSTEM(FHungerSystem)
ACS_REGISTER_SYSTEM(FPartySystem)
ACS_REGISTER_SYSTEM(FPrefabSystem)
ACS_REGISTER_SYSTEM(FPrivacyDirector)
ACS_REGISTER_SYSTEM(FReplayDirector)
ACS_REGISTER_SYSTEM(FStreamingDirector)
ACS_REGISTER_SYSTEM(FTelemetryDirector)
ACS_REGISTER_SYSTEM(FTurnManager)
ACS_REGISTER_SYSTEM(FWeatherSystem)
ACS_REGISTER_SYSTEM(FTweenManager)

// ----- enum (gameframework) -----
ACS_REGISTER_ENUM(EBtStatus,
    ACS_EVAL(EBtStatus, Running), ACS_EVAL(EBtStatus, Success), ACS_EVAL(EBtStatus, Failure))
ACS_REGISTER_ENUM(EBtDecoratorOp,
    ACS_EVAL(EBtDecoratorOp, Inverter), ACS_EVAL(EBtDecoratorOp, ForceSuccess),
    ACS_EVAL(EBtDecoratorOp, ForceFailure), ACS_EVAL(EBtDecoratorOp, Repeat))
ACS_REGISTER_ENUM(EBtVarType,
    ACS_EVAL(EBtVarType, Bool), ACS_EVAL(EBtVarType, I32), ACS_EVAL(EBtVarType, F32))
ACS_REGISTER_ENUM(EBtCompareOp,
    ACS_EVAL(EBtCompareOp, Less), ACS_EVAL(EBtCompareOp, LessEq), ACS_EVAL(EBtCompareOp, Equal),
    ACS_EVAL(EBtCompareOp, NotEqual), ACS_EVAL(EBtCompareOp, GreaterEq), ACS_EVAL(EBtCompareOp, Greater))
ACS_REGISTER_ENUM(EPlayMode,
    ACS_EVAL(EPlayMode, Loop), ACS_EVAL(EPlayMode, PingPong), ACS_EVAL(EPlayMode, Once))
ACS_REGISTER_ENUM(ECombatState,
    ACS_EVAL(ECombatState, Peaceful), ACS_EVAL(ECombatState, Alert), ACS_EVAL(ECombatState, Engaged),
    ACS_EVAL(ECombatState, BossFight), ACS_EVAL(ECombatState, Victory), ACS_EVAL(ECombatState, Retreat))

// ----- enum (render, 名前空間 acs) -----
ACS_REGISTER_ENUM(EBlendMode,
    ACS_EVAL(EBlendMode, Opaque), ACS_EVAL(EBlendMode, AlphaBlend), ACS_EVAL(EBlendMode, Additive))
ACS_REGISTER_ENUM(ECullMode,
    ACS_EVAL(ECullMode, None), ACS_EVAL(ECullMode, Front), ACS_EVAL(ECullMode, Back))
ACS_REGISTER_ENUM(ECompareFunc,
    ACS_EVAL(ECompareFunc, Never), ACS_EVAL(ECompareFunc, Less), ACS_EVAL(ECompareFunc, Equal),
    ACS_EVAL(ECompareFunc, LessEqual), ACS_EVAL(ECompareFunc, Greater), ACS_EVAL(ECompareFunc, NotEqual),
    ACS_EVAL(ECompareFunc, GreaterEqual), ACS_EVAL(ECompareFunc, Always))
ACS_REGISTER_ENUM(EPrimitiveTopology,
    ACS_EVAL(EPrimitiveTopology, PointList), ACS_EVAL(EPrimitiveTopology, LineList),
    ACS_EVAL(EPrimitiveTopology, LineStrip), ACS_EVAL(EPrimitiveTopology, TriangleList),
    ACS_EVAL(EPrimitiveTopology, TriangleStrip))
ACS_REGISTER_ENUM(EResourceState,
    ACS_EVAL(EResourceState, Common), ACS_EVAL(EResourceState, RenderTarget),
    ACS_EVAL(EResourceState, Present), ACS_EVAL(EResourceState, CopySrc),
    ACS_EVAL(EResourceState, CopyDst), ACS_EVAL(EResourceState, UnorderedAccess),
    ACS_EVAL(EResourceState, PixelShaderResource), ACS_EVAL(EResourceState, DepthWrite),
    ACS_EVAL(EResourceState, DepthRead))

// ----- enum (asset, 名前空間 acs) -----
ACS_REGISTER_ENUM(EAssetState,
    ACS_EVAL(EAssetState, Unloaded), ACS_EVAL(EAssetState, Loading),
    ACS_EVAL(EAssetState, Ready), ACS_EVAL(EAssetState, Failed))
ACS_REGISTER_ENUM(EPixelFormat,
    ACS_EVAL(EPixelFormat, R8), ACS_EVAL(EPixelFormat, R8G8),
    ACS_EVAL(EPixelFormat, R8G8B8), ACS_EVAL(EPixelFormat, R8G8B8A8),
    ACS_EVAL(EPixelFormat, R32G32B32_F), ACS_EVAL(EPixelFormat, R32G32B32A32_F))
ACS_REGISTER_ENUM(ESampleFormat,
    ACS_EVAL(ESampleFormat, PCM_S16), ACS_EVAL(ESampleFormat, PCM_F32))

// =============================================================================
// エントリ (利用側がリンク引き込み用に 1 度呼ぶ)
// =============================================================================
namespace acs::game {

u32 AcsRegisterEngineTypes() noexcept {
    // この関数を呼ぶことで本 TU がリンクに含まれ、上の ACS_REGISTER* 自動登録子が
    // 静的初期化時に実行される。ここ自体は現在のレジストリ件数を返すだけ。
    return FTypeRegistry::Get().Count();
}

} // namespace acs::game
