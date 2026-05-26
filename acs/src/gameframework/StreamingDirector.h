// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar P (Scale-Stream) — StreamingDirector (大規模シーン chunk streaming)
//
// 役割:
//   オープンワールド / 広大なステージで全アセットを常駐させられない場合に、
//   ビューア (カメラ) 位置を中心とした矩形範囲のチャンク (cx, cy) だけを
//   load 状態に保ち、範囲外を unload する。CollisionWorld2D の SpatialGrid と
//   思想は似ているが、こちらは「アセットの load/unload」を扱う上位レイヤ。
//
// 使い方 (典型例):
//   acs::game::StreamingDirector dir;
//   dir.Init(/*chunk_size=*/100.0f, /*view_radius=*/2);
//   dir.SetMaxConcurrentLoads(4);
//   // 毎フレーム:
//   dir.SetViewerPos({ camera.x, camera.y });
//   dir.Tick(dt);
//   // ロード状況を UI 等で確認:
//   const u32 loading = dir.LoadingCount();
//   const u32 loaded  = dir.LoadedCount();
//
// 設計方針:
//   ・チャンクは 2D 整数座標 (cx, cy) で識別。world (x, y) → cx = floor(x / chunk_size)。
//   ・view_radius_chunks=2 は ビューアチャンクを中心に 5x5 (= (2*2+1)^2 = 25 個)。
//   ・状態遷移: Unloaded → Queued → Loading → Loaded → Unloading → Unloaded。
//     Tick() で「同時 Loading 数 ≤ max_concurrent_loads」を保ちつつキューを進める。
//   ・実アセットロード (AssetBundle / AssetRegistry 接続) は TODO 化。
//     スケルトンでは elapsed += dt で simulated load time = 0.5s/chunk として進行。
//   ・Pillar G の AssetBundle と二段構え: 1 chunk = 1 AssetBundle 相当の単位を想定し、
//     Phase P-2 で StreamingDirector が AssetBundle を保有する形に拡張する。
//   ・非コピー・非ムーブ (内部 TArray 規約 / 単一所有を強制)。
//   ・全 API noexcept、STL 不使用 (acs::TArray<ChunkInfo> で管理)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// チャンクの 2D 整数座標。NodeId / ShapeId と異なり generation を持たない
// (同じ (cx, cy) は常に同じチャンクを指す)。比較は値ベース。
struct ChunkId {
    i32 cx = 0;
    i32 cy = 0;

    constexpr ChunkId() noexcept = default;
    constexpr ChunkId(i32 x, i32 y) noexcept : cx(x), cy(y) {}

    constexpr bool operator==(ChunkId o) const noexcept { return cx == o.cx && cy == o.cy; }
    constexpr bool operator!=(ChunkId o) const noexcept { return !(*this == o); }
};

// チャンクのライフサイクル状態。
//   Unloaded:  メモリ不在 (= 内部 _chunks に存在しないのと等価)
//   Queued:    範囲内に入ったが Loading スロット空き待ち
//   Loading:   実 asset load 進行中 (elapsed が積算される)
//   Loaded:    使用可能。範囲外に出るまで保持。
//   Unloading: 範囲外に出て解放処理中 (Phase P-2 で async unload するための予約状態)
enum class EChunkState : u8 {
    Unloaded  = 0,
    Queued    = 1,
    Loading   = 2,
    Loaded    = 3,
    Unloading = 4,
};

// StreamingDirector: ビューアを中心とした矩形範囲のチャンクを load/unload 管理する。
//
// ライフサイクル: Init() → 毎フレーム SetViewerPos + Tick → ClearAll() (シーン終了時)。
// Tick() は (1) 範囲内チャンクを Queued に挿入、(2) Loading 上限内で Queued→Loading→Loaded
// を進行、(3) 範囲外の Loaded を Unloading→Unloaded に遷移、を行う。
class StreamingDirector {
public:
    StreamingDirector() noexcept = default;
    ~StreamingDirector() noexcept = default;

    StreamingDirector(const StreamingDirector&)            = delete;
    StreamingDirector& operator=(const StreamingDirector&) = delete;
    StreamingDirector(StreamingDirector&&)                 = delete;
    StreamingDirector& operator=(StreamingDirector&&)      = delete;

    // chunk_size: 1 チャンクの world 座標サイズ (典型値 100.0f = 100m 四方)。
    // view_radius_chunks: ビューアチャンクを中心とした保持半径 (chunks 数)。
    //   例: 2 → 5x5 = 25 chunks を Loaded に保つ。0 でも 1x1 だけは保持される。
    // 0 以下 / 不正値は既定値にフォールバック。多重呼び出しは値の更新のみ
    // (既存チャンクの再評価は次の Tick() で自動的に行われる)。
    void Init(f32 chunk_size = 100.0f, i32 view_radius_chunks = 2) noexcept;

    // ビューア (カメラ) のワールド座標を更新する。
    // 次の Tick() で必要なチャンクの load/unload が再評価される。
    void SetViewerPos(FVec2 pos) noexcept;

    // 同時 Loading 数の上限を設定する (既定 4)。0 を渡した場合 1 にクランプ。
    // Loading 中の数がこれを超えると新規 Queued は次フレームに繰り越される。
    void SetMaxConcurrentLoads(u32 n) noexcept;

    // 毎フレーム呼ぶ。
    //   1. ビューア周辺の必要チャンクを Queued として挿入 (重複は無視)。
    //   2. Loading の進捗を dt 加算で進める。完了で Loaded に遷移。
    //   3. 範囲外の Loaded を Unloading → 即 Unloaded (= _chunks から除去) に。
    //   4. Loading 上限の範囲で Queued → Loading に昇格。
    // dt < 0 は 0 にクランプ (時間巻き戻し防止)。
    void Tick(f32 dt) noexcept;

    // 指定チャンクの現状態を返す。未登録チャンクは Unloaded。
    EChunkState GetState(ChunkId id) const noexcept;

    // Loaded 状態のチャンク数。
    u32 LoadedCount() const noexcept;

    // Loading 状態のチャンク数 (Queued は含まない)。
    u32 LoadingCount() const noexcept;

    // 指定チャンクを強制的に Unloaded にする (デバッグ / メモリ圧追従用)。
    // Loading 中でも即座に破棄する (実 asset load の cancel は Phase P-2 で対応)。
    // 範囲内にあれば次 Tick() で再び Queued に戻る点に注意。
    void ForceUnload(ChunkId id) noexcept;

    // 全チャンクを破棄する。シーン遷移時に呼ぶ。
    void ClearAll() noexcept;

private:
    // 1 チャンクの管理情報。
    // elapsed は Loading 中のみ意味を持つ (Loaded 到達後はリセットされる)。
    struct ChunkInfo {
        ChunkId    id      {};
        EChunkState state   = EChunkState::Unloaded;
        f32        elapsed = 0.0f;
    };

    // 同名 (cx, cy) のチャンクを検索。無ければ nullptr。
    ChunkInfo*       Find(ChunkId id) noexcept;
    const ChunkInfo* Find(ChunkId id) const noexcept;

    // ビューアチャンク (cx, cy) を返す。
    ChunkId ViewerChunk() const noexcept;

    // (cx, cy) がビューア中心の view_radius 矩形に入っているか。
    bool InRange(ChunkId id) const noexcept;

    // 範囲内チャンクを走査し、未登録分を Queued として追加する。
    void EnqueueInRange() noexcept;

    // Queued → Loading への昇格 (max_concurrent_loads を遵守)。
    void PromoteQueuedToLoading() noexcept;

    // simulated load time = 0.5s/chunk。Phase P-2 で AssetBundle::Progress() と置換予定。
    static constexpr f32 kSimulatedLoadSeconds = 0.5f;

    // 設定値。Init 未呼出時のフォールバック既定。
    f32 _chunk_size           = 100.0f;
    i32 _view_radius          = 2;
    u32 _max_concurrent_loads = 4;

    FVec2 _viewer_pos = {0.0f, 0.0f};

    // 管理対象チャンク群。範囲外で Unloaded 確定したものは TArray から除去する
    // (= Unloaded 状態の ChunkInfo は内部に残らない、というインバリアント)。
    TArray<ChunkInfo> _chunks;
};

} // namespace acs::game
