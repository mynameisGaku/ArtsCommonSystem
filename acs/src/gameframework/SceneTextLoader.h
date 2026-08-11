// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "gameframework/RigidWorld2D.h"

namespace acs { class IRhiDevice; class IRhiTexture; }

namespace acs::game {

class ANode;
class IAssetPackReader;

/** ARigidBody2D を持つノードの剛体パラメータ (シーン側で CRigidWorld2D に積む)。 */
struct FRigidBodyRequest {
    ANode* node        = nullptr;
    int      bodyType    = 1;       // 0=Static, 1=Dynamic
    f32      restitution = 0.1f;
    f32      friction    = 0.5f;
    f32      mass        = 1.0f;
    f32      linDamp     = 0.05f;
    f32      angDamp     = 0.1f;
    int      shape       = 0;       // 0=Box, 1=Circle, 3=Polygon (APrimitiveRenderer2D の shape)
    f32      base        = 48.0f;   // ノードの base (= ピクセルサイズ)
    FVec2    poly[kMaxPolyVerts]{};  // shape==3 のローカル頂点
    u32      polyCount   = 0u;
};

/** SPRT 行で要求されたスプライト (テクスチャは GPU upload が要るので後段で解決)。 */
struct FSpriteRequest {
    /** スプライトを付ける対象ノード。 */
    ANode* node = nullptr;

    /** 画像ファイルパス (UTF-8、通常は絶対パス)。 */
    char     path[260] = {};

    /** スプライトサイズ (node の base、world=pixel)。 */
    FVec2    size{ 48.0f, 48.0f };
};

/** PBR (Lit) マテリアルの法線マップ要求 (GPU upload が要るので後段で解決)。 */
struct FMaterialTexRequest {
    /** 法線マップを付ける対象ノード。 */
    ANode* node = nullptr;

    /** 法線マップ画像パス (UTF-8)。 */
    char     normalPath[260] = {};
};

/** 読み込んだノード群の world 位置の境界 (カメラのフレーミング用)。 */
struct FSceneBounds {
    /** 最小 (左上)。 */
    FVec2 min{ 0.0f, 0.0f };

    /** 最大 (右下)。 */
    FVec2 max{ 0.0f, 0.0f };

    /** 1 つ以上ノードがあり境界が有効か。 */
    bool  valid = false;

    /** 境界中心。 */
    FVec2 Center() const noexcept { return FVec2{ (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f }; }

    /** 境界サイズ (幅, 高さ)。 */
    FVec2 Size() const noexcept { return FVec2{ max.x - min.x, max.y - min.y }; }
};

/** 検証付き ACSCENE text/file 読み込みの上限。 */
inline constexpr usize kSceneTextMaxBytes = 8u * 1024u * 1024u;
inline constexpr u32 kSceneTextMaxLineBytes = 2047u;
inline constexpr u32 kSceneTextMaxNodes = 4096u;
inline constexpr u32 kSceneTextMaxDirectiveRecords = 262144u;
inline constexpr u32 kSceneTextMaxComponentsPerNode = 1024u;
inline constexpr u32 kSceneTextMaxPathBytes = 259u;

/** 検証付き ACSCENE loader が返す詳細な失敗理由。 */
enum class ESceneTextLoadError : u8 {
    /** 読み込みが成功した。 */
    None = 0,
    /** 入力 text が null だった。 */
    NullText,
    /** ファイル入力の path が null だった。 */
    NullPath,
    /** 入力 text が許容バイト数を超えた。 */
    TextTooLarge,
    /** 1 行のバイト数が上限を超えた。 */
    LineTooLong,
    /** ACSCENE header の形式が不正だった。 */
    InvalidHeader,
    /** node 件数の宣言がなかった。 */
    MissingNodeCount,
    /** node 件数が安全上限を超えた。 */
    NodeLimitExceeded,
    /** 宣言件数より前に node list が終端した。 */
    TruncatedNodeList,
    /** node record の構文または値が不正だった。 */
    InvalidNodeRecord,
    /** 同じ node id が複数回現れた。 */
    DuplicateNodeId,
    /** node が存在しない node id を参照した。 */
    InvalidNodeReference,
    /** node の親子関係に循環があった。 */
    HierarchyCycle,
    /** node tree の深さが安全上限を超えた。 */
    TreeDepthLimitExceeded,
    /** directive 件数が安全上限を超えた。 */
    DirectiveLimitExceeded,
    /** 1 node の component 件数が上限を超えた。 */
    ComponentLimitExceeded,
    /** directive の種類または引数が不正だった。 */
    InvalidDirective,
    /** 入力 path のバイト数が上限を超えた。 */
    PathTooLong,
    /** 書き込み先 node が破棄待ちだった。 */
    TargetPendingDestroy,
    /** 読み込み用メモリを確保できなかった。 */
    AllocationFailure,
    /** 検証済み scene を出力先へ確定できなかった。 */
    CommitFailed,
    /** scene file を開けなかった。 */
    FileOpenFailed,
    /** scene file の読み取り位置を移動できなかった。 */
    FileSeekFailed,
    /** scene file のサイズが安全上限を超えた。 */
    FileSizeLimitExceeded,
    /** scene file の全内容を読み取れなかった。 */
    FileReadFailed,
    /** text 内に埋め込み NUL があった。 */
    EmbeddedNul,
};

/**
 * 検証付き ACSCENE 読み込みの結果。
 *
 * 失敗時は、出力先 tree と request array の内容を変更しない。
 * `line` は text の構文・意味エラーでは 1 始まり、file error では 0。
 */
struct FSceneTextLoadResult {
    ESceneTextLoadError Error = ESceneTextLoadError::None;
    FSceneBounds Bounds{};
    u32 Line = 0u;
    u32 NodesLoaded = 0u;
    u32 DirectivesRead = 0u;

    bool Succeeded() const noexcept { return Error == ESceneTextLoadError::None; }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ESceneTextLoadError に対応する安定した診断名。 */
const char* SceneTextLoadErrorName(ESceneTextLoadError error) noexcept;

/**
 * NUL 終端 ACSCENE text を検証し、transactional に読み込む。
 *
 * 呼び出し側が所有する state を変更する前に、document 全体と parent graph を検証する。
 * forward compatibility のため未知の directive 名は引き続き無視し、既知 directive の
 * 不正形式は拒否する。
 */
FSceneTextLoadResult TryLoadAcsceneText(
    const char* text, ANode& root,
    TArray<FSpriteRequest>* out_sprites = nullptr,
    TArray<FRigidBodyRequest>* out_bodies = nullptr,
    TArray<FMaterialTexRequest>* out_mat_tex = nullptr,
    ANode** out_root = nullptr) noexcept;

/** TryLoadAcsceneText に対応する検証付き file 読み込み。 */
FSceneTextLoadResult TryLoadAcsceneFile(
    const char* path, ANode& root,
    TArray<FSpriteRequest>* out_sprites = nullptr,
    TArray<FRigidBodyRequest>* out_bodies = nullptr,
    TArray<FMaterialTexRequest>* out_mat_tex = nullptr,
    ANode** out_root = nullptr) noexcept;

/**
 * `.acpak` 内の ACSCENE text を検証して transactional に読み込む。
 *
 * @details scene の MAT と、そこから得た画像要求も同じ pack の仮想パスを保持する。
 * pack entry の不足・CRC不一致・不正textは失敗として返し、loose fileへfallbackしない。
 */
FSceneTextLoadResult TryLoadAcsceneAssetPack(
    IAssetPackReader& pack, const char* virtual_path, ANode& root,
    TArray<FSpriteRequest>* out_sprites = nullptr,
    TArray<FRigidBodyRequest>* out_bodies = nullptr,
    TArray<FMaterialTexRequest>* out_mat_tex = nullptr,
    ANode** out_root = nullptr) noexcept;

/**
 * ACSCENE テキストを root 配下へ復元する。
 *
 * @details
 * ノードを root 直下に平坦生成してから親 id で付け替える (順序非依存)。COMP は
 * CreateComponentByName で実体化し、CPROP は反射フィールド (offset 付き=ユーザー型) へ適用、
 * APrimitiveRenderer2D は shape/color/size を typed setter で設定する (ノードの color/base を使用)。
 * @param text NUL 終端の ACSCENE テキスト。
 * @param root 子を追加する先の root ノード。
 * @param out_sprites 非 null なら SPRT 行を集める (テクスチャ解決は LoadSceneSprites で後段)。
 * @return 読み込んだノードの world 境界 (空シーンは valid=false)。
 */
FSceneBounds LoadAcsceneText(const char* text, ANode& root,
                             TArray<FSpriteRequest>* out_sprites = nullptr,
                             TArray<FRigidBodyRequest>* out_bodies = nullptr,
                             TArray<FMaterialTexRequest>* out_mat_tex = nullptr,
                             ANode** out_root = nullptr) noexcept;

/**
 * ACSCENE ファイルを読み込んで root 配下へ復元する。
 *
 * @param path 読み込む .acscene ファイルパス。
 * @param root 子を追加する先の root ノード。
 * @param out_sprites 非 null なら SPRT 行を集める。
 * @return 読み込んだノードの world 境界 (読込失敗/空は valid=false)。
 */
FSceneBounds LoadAcsceneFile(const char* path, ANode& root,
                             TArray<FSpriteRequest>* out_sprites = nullptr,
                             TArray<FRigidBodyRequest>* out_bodies = nullptr,
                             TArray<FMaterialTexRequest>* out_mat_tex = nullptr) noexcept;

/** TryLoadAcsceneAssetPack の互換 wrapper。失敗時は空 bounds を返す。 */
FSceneBounds LoadAcsceneAssetPack(
    IAssetPackReader& pack, const char* virtual_path, ANode& root,
    TArray<FSpriteRequest>* out_sprites = nullptr,
    TArray<FRigidBodyRequest>* out_bodies = nullptr,
    TArray<FMaterialTexRequest>* out_mat_tex = nullptr) noexcept;

/**
 * プレハブ(.acsprefab = サブツリーの ACSCENE 直列化テキスト)を実行時に parent 配下へ
 * 生成し、生成したサブツリーの «ルートノード» を返す(失敗 nullptr)。
 *
 * @details 実コンポーネントを attach + authored 値適用 + SerialId 設定済み(オブジェクト参照も
 * 解決される)。返り値の root を SetPosition2D 等で配置/操作する。敵・弾などの動的生成に使う。
 * @param text NUL 終端の .acsprefab テキスト。
 * @param parent 生成先の親ノード(通常はシーンの Root)。
 * @return 生成したサブツリーのルートノード(parent の子。失敗で nullptr)。
 */
ANode* SpawnPrefabText(const char* text, ANode& parent) noexcept;

/**
 * プレハブファイル(.acsprefab)を実行時に parent 配下へ生成し、ルートノードを返す。
 *
 * @param path .acsprefab ファイルパス。
 * @param parent 生成先の親ノード。
 * @return 生成したサブツリーのルートノード(失敗で nullptr)。
 */
ANode* SpawnPrefabFile(const char* path, ANode& parent) noexcept;

/**
 * マテリアル法線マップ要求を GPU テクスチャ化し、各ノードへ非所有ポインタで配線する。
 *
 * @param device テクスチャ生成に使う RHI デバイス。
 * @param reqs LoadAcscene* が集めた法線マップ要求。
 * @param out_textures 生成したテクスチャの所有先 (呼び出し側が保持する)。
 */
void LoadSceneMaterialTextures(IRhiDevice& device, const TArray<FMaterialTexRequest>& reqs,
                               TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept;

/**
 * `.acpak` entry からマテリアル法線画像を読み、GPUテクスチャ化する。
 * 読み出し失敗時にloose fileへfallbackしない。
 */
void LoadSceneMaterialTexturesFromAssetPack(
    IRhiDevice& device, const TArray<FMaterialTexRequest>& reqs,
    IAssetPackReader& pack,
    TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept;

/**
 * 剛体要求から CRigidWorld2D にボディを積む (editor の物理 Play と同じ形状/サイズ規約)。
 *
 * @details circle: radius=base*0.5*max(scale)。box: half=base*0.5*scale。polygon: ローカル頂点に
 * scale を焼き込む。Static は質量 0。動的ボディだけ out_nodes/out_bodies に記録 (毎フレーム書き戻し用)。
 * @param world ボディを積む剛体ワールド。
 * @param reqs 剛体要求。
 * @param out_nodes 動的ボディの対応ノード (書き戻し先)。
 * @param out_bodies 動的ボディの index。
 */
void BuildSceneRigidBodies(CRigidWorld2D& world, const TArray<FRigidBodyRequest>& reqs,
                           TArray<ANode*>& out_nodes, TArray<u32>& out_bodies) noexcept;

/**
 * 剛体ワールドを 1 ステップ進め、動的ボディの位置/角度を対応ノードへ書き戻す。
 *
 * @param world 剛体ワールド。
 * @param nodes 動的ボディ対応ノード。
 * @param bodies 動的ボディ index。
 * @param dt 時間刻み。
 * @param gravity 重力 (+Y=下、ピクセルスケールなら ~900)。
 */
void StepSceneRigidBodies(CRigidWorld2D& world, const TArray<ANode*>& nodes,
                          const TArray<u32>& bodies, f32 dt, FVec2 gravity) noexcept;

/**
 * SPRT 要求の画像をロードして ASprite2DComponent を attach する (device が要るので render 中に呼ぶ)。
 *
 * @details 各 path を読み込み→デコード→GPU テクスチャ化し、req.node に ASprite2DComponent を付けて
 * テクスチャを bind する。生成テクスチャは out_textures が所有する (シーン寿命で保持すること)。
 * 失敗 (ファイル無し/デコード不可) のエントリは静かにスキップ。非 ASCII パスは未対応。
 * @param device GPU デバイス (rc.GetRenderer().Device())。
 * @param reqs LoadAcscene* が集めたスプライト要求。
 * @param out_textures 生成テクスチャの所有先 (シーンが保持)。
 */
void LoadSceneSprites(IRhiDevice& device, const TArray<FSpriteRequest>& reqs,
                      TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept;

/**
 * `.acpak` entry からスプライト画像を読み、GPUテクスチャ化する。
 * 読み出し失敗時にloose fileへfallbackしない。
 */
void LoadSceneSpritesFromAssetPack(
    IRhiDevice& device, const TArray<FSpriteRequest>& reqs,
    IAssetPackReader& pack,
    TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept;

} // namespace acs::game
