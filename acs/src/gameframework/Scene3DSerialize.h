// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — 3D シーン (ANode ツリー) のテキストシリアライズ
// -----------------------------------------------------------------------------
// CScene3D の階層 + 各ノードの FTransform3D (pos/euler/scale) + AMeshComponent3D
// (prim/color/mesh path) を行ベースのテキストへ往復させる。editor_abi の 3D ビュー
// ポートの scene3d_serialize/load_text が委譲する «正準フォーマット» (移行後)。
//
// フォーマット (行ベース、editor の N3D/MSH3D を階層対応に拡張):
//   N3D <id> <parent> <prim> <px py pz> <rx ry rz(度)> <sx sy sz> <r g b a> <name>
//   MSH3D <id> <mesh_path>
//   ・id        = DFS pre-order の通し番号 (root=0)。
//   ・parent    = 親の id (-1 = root 自身)。
//   ・prim      = EMeshPrimitive3D の整数 (AMeshComponent3D 無しは -1)。
//   ・rot       = FTransform3D::EulerDeg() (度、XYZ。|Y|<90° で往復一致)。
//   ・MSH3D     = prim==Mesh かつ mesh path を持つノードのみ (アセット実体のロードは
//                 呼び出し側の責務。本シリアライザはパスのみ往復する)。
//
// 規約: no-STL (C の strtol/strtof/snprintf のみ) / 全 noexcept / 固定上限。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "math/Vec.h"

namespace acs::game {

class IAssetPackReader;
class CSceneNodeGraph;

inline constexpr u32 kScene3DSerializeMaxInputBytes = 4u * 1024u * 1024u;
inline constexpr u32 kScene3DSerializeMaxNodeCount = 65536u;
inline constexpr u32 kScene3DSerializeMaxTreeDepth = 512u;
inline constexpr u32 kScene3DSerializeMaxLineBytes = 4095u;
inline constexpr u32 kScene3DSerializeMaxNameBytes = 127u;
inline constexpr u32 kScene3DSerializeMaxMeshPathBytes = 299u;
inline constexpr u32 kScene3DSerializeMaxMaterialPathBytes = 299u;
inline constexpr u32 kScene3DSerializeMaxComponentsPerNode = 1024u;
inline constexpr u32 kScene3DSerializeMaxDirectiveRecords = 262144u;
inline constexpr u32 kScene3DSerializeMaxCameraCount = 256u;
inline constexpr u32 kScene3DSerializeMaxCameraIdBytes = 64u;
inline constexpr u64 kScene3DAssetMaxBytes = 256u * 1024u * 1024u;

/** Scene3D テキスト保存・読み込みの安定した失敗理由。 */
enum class EScene3DSerializeError : u8 {
    None = 0,
    NullInput,
    NullOutput,
    BufferTooSmall,
    InputTooLarge,
    LineTooLong,
    InvalidLine,
    InvalidInteger,
    InvalidNumber,
    InvalidPrimitive,
    InvalidNodeId,
    DuplicateNodeId,
    InvalidParent,
    MissingRoot,
    MultipleRoots,
    NodeLimitExceeded,
    TreeDepthLimitExceeded,
    InvalidName,
    InvalidMeshPath,
    DuplicateMeshPath,
    AllocationFailure,
    DuplicateNodeReference,
    CyclicNodeGraph,
    SerializedSizeOverflow,
    SceneChangedDuringSave,
    InvalidHeader,
    UnsupportedVersion,
    UnsupportedDirective,
    InvalidNodeFlags,
    InvalidMaterial,
    InvalidComponent,
    InvalidComponentProperty,
    InvalidCamera,
    DuplicateCamera,
    CameraLimitExceeded,
    DirectiveLimitExceeded,
    ComponentLimitExceeded,
    FileOpenFailed,
    FileSeekFailed,
    FileSizeLimitExceeded,
    FileReadFailed,
    EmbeddedNul,
    AssetPathInvalid,
    AssetMissing,
    AssetDecodeFailed,
    MaterialDecodeFailed,
};

/** Authored ACS3D camera projection encoded by CAM3D. */
enum class EScene3DCameraProjection : u8 {
    Perspective = 0,
    Orthographic = 1,
};

/**
 * Deterministically selected authored camera returned by the checked loader.
 *
 * @details The camera pose is derived from the selected N3D node's complete
 * world transform after hierarchy commit. Forward is local +Z and Up is local
 * +Y. StableId is a case-sensitive canonical ASCII identity.
 */
struct FScene3DCameraState {
    bool IsAuthored = false;
    bool IsActivePreferred = false;
    i32 NodeId = -1;
    i32 Priority = 0;
    EScene3DCameraProjection Projection =
        EScene3DCameraProjection::Perspective;
    f32 FovYDegrees = 60.0f;
    f32 OrthographicHeight = 10.0f;
    f32 NearPlane = 0.05f;
    f32 FarPlane = 1000.0f;
    FVec3 Position{0.0f, 0.0f, 0.0f};
    FVec3 Forward{0.0f, 0.0f, 1.0f};
    FVec3 Up{0.0f, 1.0f, 0.0f};
    char StableId[kScene3DSerializeMaxCameraIdBytes + 1u]{};
};

/** 検証付きテキスト保存結果。RequiredBytes は終端 NUL を含む必要容量。 */
struct FScene3DSaveResult {
    EScene3DSerializeError Error = EScene3DSerializeError::None;
    u32 BytesWritten = 0u;
    u32 RequiredBytes = 0u;
    u32 NodeCount = 0u;
    u32 MeshPathCount = 0u;
    u32 CameraCount = 0u;

    bool Succeeded() const noexcept {
        return Error == EScene3DSerializeError::None && BytesWritten > 0u
            && RequiredBytes == BytesWritten + 1u;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** 検証付きテキスト読み込み結果。失敗時は宛先シーンを変更しない。 */
struct FScene3DLoadResult {
    EScene3DSerializeError Error = EScene3DSerializeError::None;
    u32 BytesConsumed = 0u;
    u32 NodeCount = 0u;
    u32 MeshPathCount = 0u;
    u32 ErrorLine = 0u;
    u32 DependenciesLoaded = 0u;
    u32 CameraCount = 0u;
    u32 ActivePreferredCameraCount = 0u;
    FScene3DCameraState ActiveCamera{};

    bool Succeeded() const noexcept {
        return Error == EScene3DSerializeError::None && NodeCount > 0u;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ログ・テレメトリ用の安定した ASCII エラー名。 */
const char* Scene3DSerializeErrorName(EScene3DSerializeError error) noexcept;

/**
 * シーンを検証・計測してからテキストへ保存する。
 *
 * @details buf=nullptr/cap=0 はサイズ照会。容量不足では出力を変更しない。
 * RequiredBytes は終端 NUL を含み、BytesWritten は含まない。
 */
FScene3DSaveResult TrySaveScene3DText(
    const CSceneNodeGraph& graph, char* out, u32 cap) noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
FScene3DSaveResult TrySaveScene3DText(
    const CScene3D& scene, char* out, u32 cap) noexcept;

/**
 * CScene3D をテキストへ直列化する (root + 全子孫、構造 + transform + メッシュ記述)。
 *
 * @param scene 直列化するシーン。
 * @param out 出力バッファ (null 終端される)。
 * @param cap out の容量。
 * @return 書き込んだ文字数 (null 終端を除く)。失敗時は 0。
 */
u32 SaveScene3DText(const CSceneNodeGraph& graph, char* out, u32 cap) noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
u32 SaveScene3DText(const CScene3D& scene, char* out, u32 cap) noexcept;

/**
 * size bytes のテキストを完全検証してから既存シーンを置き換える。
 *
 * @details size は終端 NUL を含めない。切詰め、長過ぎる行、非有限値、巨大/重複 id、
 * 不正 parent、深度超過、孤立 MSH3D は置換前に拒否する。
 */
FScene3DLoadResult TryLoadScene3DText(
    CSceneNodeGraph& graph, const char* text, u32 size) noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
FScene3DLoadResult TryLoadScene3DText(
    CScene3D& scene, const char* text, u32 size) noexcept;

/**
 * 旧 `.acs3d` 文書または canonical bootstrap を loose file から読み、
 * 参照メッシュ/マテリアルも検証して復元する。
 *
 * @details 本文・全依存の解析が完了するまで scene を変更しない。相対参照は scene
 * file の親ディレクトリを基準に解決し、失敗時に別の探索 root へ fallback しない。
 */
FScene3DLoadResult TryLoadScene3DFile(
    CSceneNodeGraph& graph, const char* path) noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
FScene3DLoadResult TryLoadScene3DFile(
    CScene3D& scene, const char* path) noexcept;

/**
 * `.acpak` 内の canonical bootstrap と参照メッシュ/マテリアルを transactional に復元する。
 *
 * @details virtual path は `/` 区切りの pack 内相対 path のみ受理する。entry 不足、
 * CRC/解凍失敗、unsupported mesh、壊れた material では loose file に fallback しない。
 */
FScene3DLoadResult TryLoadScene3DAssetPack(
    CSceneNodeGraph& graph, IAssetPackReader& pack,
    const char* virtual_path = "main.acscene") noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
FScene3DLoadResult TryLoadScene3DAssetPack(
    CScene3D& scene, IAssetPackReader& pack,
    const char* virtual_path = "main.acscene") noexcept;

/**
 * SaveScene3DText のテキストから CScene3D を復元する (既存内容を置き換える)。
 *
 * @details
 * 互換用の NUL 終端 C 文字列 API。詳細結果と入力サイズ上限が必要なら
 * TryLoadScene3DText を使う。
 * @param scene 復元先のシーン (内容は置き換わる)。
 * @param text 直列化テキスト。
 * @return 解析が成立したら true (text==null は false)。
 */
bool LoadScene3DText(CSceneNodeGraph& graph, const char* text) noexcept;

/** CScene3D 互換 overload (所有 graph へ委譲する)。 */
bool LoadScene3DText(CScene3D& scene, const char* text) noexcept;

} // namespace acs::game
