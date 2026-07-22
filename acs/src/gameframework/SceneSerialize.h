// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — シーン (ANode ツリー) のシリアライズ (SceneSerialize)
// -----------------------------------------------------------------------------
// ANode の階層構造 + 各ノードの FTransform3D + 描画フラグ (enabled/visible/
// drawLayer/drawPriority/ySortEnabled/ySortBias) を自己記述バイト列へ往復させる
// 「シーンの骨格」永続化。
// レベル配置 (どこに何があるか) をディスク/メモリに保存・復元する土台。
//
// フォーマット (version 4):
//   [u32 magic][u32 version][u32 node_count]
//   per node (DFS pre-order = 親が子より先):
//     [i32 parent_index (-1=root)]
//     [f32 px][f32 py][f32 pz]
//     [f32 qx][f32 qy][f32 qz][f32 qw]
//     [f32 sx][f32 sy][f32 sz]
//     [u8 enabled][u8 visible][i32 drawLayer][i32 drawPriority]
//     [u8 ySortEnabled][f32 ySortBias]
//     [u32 component_count]
//     per component (ReflectName を持つもののみ):
//       [u8 name_len][reflect_name…][u32 payload_len][payload (ReflectSerialize の出力)]
//
// コンポーネント:
//   ・ComponentFactory + 非テンプレート attach + ReflectName 橋で「型を知らずに」復元する。
//   ・値は ReflectSerialize で往復する。public フィールドを ACS_RFIELD 反射した
//     コンポーネントは値も完全に復元される。private メンバのみ (ACS_RPROP スキーマ) の
//     同梱コンポーネントは payload が空となり factory 既定値で復元される (attach は保つ)。
//   ・ReflectName を持たない (未対応の) コンポーネント / Abstract で実体化できない型は
//     save 時にスキップ (= 復元されない)。
//
// 範囲:
//   ・ノードは素の ANode として復元する (派生ノード型のロジックは持たない。
//     データ駆動シーン = 素ノード + コンポーネントを想定)。
//
// 規約: no-STL / no-exceptions / 全 noexcept / 固定バッファ。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "memory/ObjectPtr.h"

namespace acs::game {

class ANode;

/** シーン骨格フォーマットの識別 + バージョン。 */
inline constexpr u32 kSceneSerializeMagic   = 0xAC5F2002u;
inline constexpr u32 kSceneSerializeVersion = 4u;   // v4: ANode の完全な FTransform3D を含む
inline constexpr u32 kSceneSerializeMaxNodeCount = 65536u;
inline constexpr u32 kSceneSerializeMaxTreeDepth = 512u;
inline constexpr u32 kSceneSerializeMaxComponentCountPerNode = 1024u;
inline constexpr u32 kSceneSerializeMaxComponentPayloadBytes = 4096u;

/** 検証付き読み込みが返す失敗理由。 */
enum class ESceneSerializeError : u8 {
    None = 0,
    NullInput,
    TruncatedData,
    InvalidMagic,
    UnsupportedVersion,
    EmptyTree,
    NodeLimitExceeded,
    ComponentLimitExceeded,
    InvalidComponentName,
    ComponentPayloadLimitExceeded,
    InvalidComponentPayload,
    InvalidStructure,
    AllocationFailure,
    NullRoot,
    NullOutput,
    BufferTooSmall,
    DuplicateNodeReference,
    CyclicNodeGraph,
    SerializedSizeOverflow,
    SceneChangedDuringSave,
};

/** 検証付きシーン保存の結果。容量不足時も RequiredBytes で再試行容量が分かる。 */
struct FSceneSaveResult {
    ESceneSerializeError Error = ESceneSerializeError::None;
    u32 BytesWritten = 0u;
    u32 RequiredBytes = 0u;
    u32 NodeCount = 0u;
    u32 ComponentCount = 0u;

    bool Succeeded() const noexcept {
        return Error == ESceneSerializeError::None && BytesWritten > 0u
            && BytesWritten == RequiredBytes;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** 検証付きシーン読み込みの結果。失敗時は Root が必ず空になる。 */
struct FSceneLoadResult {
    TObjectPtr<ANode> Root{};
    ESceneSerializeError Error = ESceneSerializeError::None;
    u32 BytesRead = 0u;
    u32 FormatVersion = 0u;
    u32 DepthCappedNodeCount = 0u;

    bool Succeeded() const noexcept {
        return Error == ESceneSerializeError::None && Root.Get() != nullptr;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ログ・診断表示用の安定した ASCII エラー名を返す。 */
const char* SceneSerializeErrorName(ESceneSerializeError error) noexcept;

/**
 * root とその子孫を検証・計測してから骨格バイト列へ直列化する。
 *
 * @details
 * DFS は明示スタックで行い、ノード上限、共有子、循環、コンポーネント上限、
 * 非終端を含む不正 ReflectName、payload 上限を出力前に検出する。容量不足では buf を
 * 変更せず BufferTooSmall と正確な RequiredBytes を返す。buf=nullptr/cap=0 は
 * サイズ照会として利用でき、検証成功後に BufferTooSmall と RequiredBytes を返す。
 * 呼び出し中にツリーまたはコンポーネントを別スレッドから変更してはならない。
 *
 * @param root 直列化するツリーまたは subtree の根。
 * @param buf  出力バッファ。サイズ照会では nullptr 可。
 * @param cap  buf の容量。サイズ照会では 0。
 */
FSceneSaveResult TrySaveNodeTree(const ANode* root, u8* buf, u32 cap) noexcept;

/**
 * root とその子孫を骨格バイト列へ直列化する (構造 + transform + 描画フラグ)。
 *
 * @param root 直列化するツリーの根 (この root 自身も含む)。
 * @param buf  出力バッファ。
 * @param cap  buf の容量。
 * @return 書き込んだバイト数。失敗なら 0。詳細な失敗理由は TrySaveNodeTree を使う。
 */
u32 SaveNodeTree(const ANode* root, u8* buf, u32 cap) noexcept;

/**
 * SaveNodeTree のバイト列を検証してからツリーを復元する。
 *
 * @details
 * 不正な親 index、入力上限超過、途中で切れたデータは部分成功させず、Root を空にして
 * 詳細な Error を返す。敵対的な深い親チェーンだけはノードを失わないよう root 直下へ
 * 付け替え、DepthCappedNodeCount で変更件数を通知する。v2/v3/v4 を読み込める。
 */
FSceneLoadResult TryLoadNodeTree(const u8* data, u32 size) noexcept;

/**
 * SaveNodeTree のバイト列からツリーを復元する (素の ANode で再構築)。
 *
 * @details 互換用の簡易 API。詳細な失敗理由が必要なら TryLoadNodeTree を使う。
 * @param data 直列化データ。
 * @param size data のバイト数。
 * @return 復元したツリーの根 (所有権は呼び出し側)。失敗 / 空は null。
 */
TObjectPtr<ANode> LoadNodeTree(const u8* data, u32 size) noexcept;

} // namespace acs::game
