// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar P (Scale-Stream) — FStreamingDirector (大規模シーン chunk streaming)
//
// 役割:
//   オープンワールド / 広大なステージで全アセットを常駐させられない場合に、
//   ビューア (カメラ) 位置を中心とした矩形範囲のチャンク (cx, cy) だけを
//   load 状態に保ち、範囲外を unload する。FCollisionWorld2D の SpatialGrid と
//   思想は似ているが、こちらは「アセットの load/unload」を扱う上位レイヤ。
//
// 使い方 (典型例):
//   acs::game::FStreamingDirector dir;
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
//   ・実アセットロード: 1 chunk = 1 FAssetBundle。SetAssetRegistry() で app 所有の
//     FAssetRegistry を差し込むと、Loading 遷移時に bundle.BeginLoad(registry) を発行し、
//     bundle.Progress()/IsLoaded() で実完了を判定、Unloading で bundle.Unload() する。
//     registry が未設定 (nullptr) のときは simulated load time = 0.5s/chunk の
//     フォールバックで進行する (ヘッドレステスト / registry を持たない用途向け)。
//   ・Pillar G の FAssetBundle と二段構え: FStreamingDirector が各チャンクの FAssetBundle
//     を保有し、チャンクごとのアセット集合 (パス) を SetChunkPathFormat() で決める。
//   ・非コピー・非ムーブ (内部 TArray 規約 / 単一所有を強制)。
//   ・全 API noexcept、STL 不使用 (acs::TArray<FChunkInfo> で管理)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "container/String.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"

namespace acs { class FAssetRegistry; }

namespace acs::game {

// 前方宣言: FAssetBundle はチャンクごとの実アセットロード単位。
class FAssetBundle;

// チャンクの 2D 整数座標。FNodeId / FShapeId と異なり generation を持たない
// (同じ (cx, cy) は常に同じチャンクを指す)。比較は値ベース。
struct FChunkId {
    i32 cx = 0;
    i32 cy = 0;

    constexpr FChunkId() noexcept = default;
    constexpr FChunkId(i32 x, i32 y) noexcept : cx(x), cy(y) {}

    constexpr bool operator==(FChunkId o) const noexcept { return cx == o.cx && cy == o.cy; }
    constexpr bool operator!=(FChunkId o) const noexcept { return !(*this == o); }
};

// チャンクのライフサイクル状態。
//   Unloaded:  メモリ不在 (= 内部 m_Chunks に存在しないのと等価)
//   Queued:    範囲内に入ったが Loading スロット空き待ち
//   Loading:   実 asset load 進行中 (registry 接続時は bundle.BeginLoad 済で完了待ち、
//              非接続時は elapsed が積算される)
//   Loaded:    使用可能。範囲外に出るまで保持。
//   Unloading: 範囲外に出て解放処理中 (bundle.Unload を発行して同フレームで破棄)
enum class EChunkState : u8 {
    Unloaded  = 0,
    Queued    = 1,
    Loading   = 2,
    Loaded    = 3,
    Unloading = 4,
};

// FStreamingDirector: ビューアを中心とした矩形範囲のチャンクを load/unload 管理する。
//
// ライフサイクル: Init() → 毎フレーム SetViewerPos + Tick → ClearAll() (シーン終了時)。
// Tick() は (1) 範囲内チャンクを Queued に挿入、(2) Loading 上限内で Queued→Loading→Loaded
// を進行、(3) 範囲外の Loaded を Unloading→Unloaded に遷移、を行う。
class FStreamingDirector {
public:
    FStreamingDirector() noexcept = default;
    // dtor は .cpp 側で定義する (TUniquePtr<FAssetBundle> の破棄に FAssetBundle の
    // 完全型が必要なため。ヘッダのみ include する TU で不完全型 delete を避ける)。
    ~FStreamingDirector() noexcept;

    FStreamingDirector(const FStreamingDirector&)            = delete;
    FStreamingDirector& operator=(const FStreamingDirector&) = delete;
    FStreamingDirector(FStreamingDirector&&)                 = delete;
    FStreamingDirector& operator=(FStreamingDirector&&)      = delete;

    // chunk_size: 1 チャンクの world 座標サイズ (典型値 100.0f = 100m 四方)。
    // view_radius_chunks: ビューアチャンクを中心とした保持半径 (chunks 数)。
    //   例: 2 → 5x5 = 25 chunks を Loaded に保つ。0 でも 1x1 だけは保持される。
    // 0 以下 / 不正値は既定値にフォールバック。多重呼び出しは値の更新のみ
    // (既存チャンクの再評価は次の Tick() で自動的に行われる)。
    void Init(f32 chunk_size = 100.0f, i32 view_radius_chunks = 2) noexcept;

    // 実アセットロード用の registry を差し込む (app 所有 = FApplication::GetAssets() /
    // FGame 経由、非所有 raw ptr)。設定すると各チャンクが Loading に入るとき
    // FAssetBundle::BeginLoad(*registry) を発行し、進捗/完了を bundle から取得する。
    // nullptr を渡す (= 未設定) と simulated load time フォールバックで動作する。
    // 既に Loading 中のチャンクには影響しない (次に Loading へ昇格するものから適用)。
    void            SetAssetRegistry(FAssetRegistry* registry) noexcept { m_Registry = registry; }
    FAssetRegistry* GetAssetRegistry() const noexcept { return m_Registry; }

    // チャンク (cx, cy) → アセットパスを組み立てる printf 風フォーマットを設定する。
    // 第 1, 第 2 引数に cx, cy (いずれも i32) が渡される。既定は "chunks/chunk_%d_%d.bundle"。
    // 内部で FString::AppendFormat に転送するため、必ず "%d" を 2 つこの順で含めること。
    // fmt が nullptr / 既定どおりで良ければ呼ぶ必要はない。
    // registry 未設定時はパスは使われない (simulated path のため no-op)。
    void SetChunkPathFormat(const char* fmt) noexcept;

    // ビューア (カメラ) のワールド座標を更新する。
    // 次の Tick() で必要なチャンクの load/unload が再評価される。
    void SetViewerPos(FVec2 pos) noexcept;

    // 同時 Loading 数の上限を設定する (既定 4)。0 を渡した場合 1 にクランプ。
    // Loading 中の数がこれを超えると新規 Queued は次フレームに繰り越される。
    void SetMaxConcurrentLoads(u32 n) noexcept;

    // 毎フレーム呼ぶ。
    //   1. ビューア周辺の必要チャンクを Queued として挿入 (重複は無視)。
    //   2. Loading の進捗を dt 加算で進める。完了で Loaded に遷移。
    //   3. 範囲外の Loaded を Unloading → 即 Unloaded (= m_Chunks から除去) に。
    //   4. Loading 上限の範囲で Queued → Loading に昇格。
    // dt < 0 は 0 にクランプ (時間巻き戻し防止)。
    void Tick(f32 dt) noexcept;

    // 指定チャンクの現状態を返す。未登録チャンクは Unloaded。
    EChunkState GetState(FChunkId id) const noexcept;

    // Loaded 状態のチャンク数。
    u32 LoadedCount() const noexcept;

    // Loading 状態のチャンク数 (Queued は含まない)。
    u32 LoadingCount() const noexcept;

    // 指定チャンクを強制的に Unloaded にする (デバッグ / メモリ圧追従用)。
    // Loading 中でも即座に破棄する (bundle.Unload で TRc を drop。FAssetBundle は同期
    // ロードなので「進行中の async load」は無く、cancel 不要で即時解放できる)。
    // 範囲内にあれば次 Tick() で再び Queued に戻る点に注意。
    void ForceUnload(FChunkId id) noexcept;

    // 全チャンクを破棄する。シーン遷移時に呼ぶ。
    void ClearAll() noexcept;

private:
    // 1 チャンクの管理情報。
    // elapsed は simulated フォールバック時の Loading 中のみ意味を持つ
    // (registry 接続時は bundle 進捗を見るので未使用、Loaded 到達後はリセットされる)。
    //
    // bundle は registry 接続時のみ生成される (MakeUnique で遅延生成し、Unloading で破棄)。
    // path は bundle が const char* を借用するため FChunkInfo が所有する必要がある
    // (FAssetBundle::Add は文字列を借用するだけ = bundle より長寿命であること)。
    // FString / TUniquePtr メンバを持つため FChunkInfo はムーブのみ可 (コピー不可)。
    // → TArray の swap-erase / Grow は Move 経路を使うこと (RemoveAtSwap / Move 代入)。
    struct FChunkInfo {
        FChunkId               id      {};
        EChunkState            state   = EChunkState::Unloaded;
        f32                   elapsed = 0.0f;
        FString                path;    // "chunks/chunk_<cx>_<cy>.bundle" 等 (bundle が借用)
        TUniquePtr<FAssetBundle> bundle; // registry 接続時のみ生成 (非接続時は null)
    };

    // 同名 (cx, cy) のチャンクを検索。無ければ nullptr。
    FChunkInfo*       Find(FChunkId id) noexcept;
    const FChunkInfo* Find(FChunkId id) const noexcept;

    // ビューアチャンク (cx, cy) を返す。
    FChunkId ViewerChunk() const noexcept;

    // (cx, cy) がビューア中心の view_radius 矩形に入っているか。
    bool InRange(FChunkId id) const noexcept;

    // 範囲内チャンクを走査し、未登録分を Queued として追加する。
    void EnqueueInRange() noexcept;

    // Queued → Loading への昇格 (max_concurrent_loads を遵守)。
    // registry 接続時は bundle を生成して BeginLoad、非接続時は elapsed をリセットする。
    void PromoteQueuedToLoading() noexcept;

    // チャンク (cx, cy) の bundle を生成し、SetChunkPathFormat のパスを Add → BeginLoad。
    // registry が null のときは何もしない (simulated 経路で進行)。
    void BeginChunkLoad(FChunkInfo& c) noexcept;

    // registry 未接続時の simulated load time = 0.5s/chunk (フォールバック)。
    static constexpr f32 kSimulatedLoadSeconds = 0.5f;

    // SetChunkPathFormat 未設定時の既定パスフォーマット (cx, cy = i32)。
    static constexpr const char* kDefaultChunkPathFormat = "chunks/chunk_%d_%d.bundle";

    // 設定値。Init 未呼出時のフォールバック既定。
    f32 m_ChunkSize           = 100.0f;
    i32 m_ViewRadius          = 2;
    u32 m_MaxConcurrentLoads = 4;

    // 実アセットロード接続先 (非所有)。null なら simulated フォールバック。
    FAssetRegistry* m_Registry = nullptr;

    // チャンクパス組み立てフォーマット (空なら kDefaultChunkPathFormat を使用)。
    FString m_ChunkPathFormat;

    FVec2 m_ViewerPos = {0.0f, 0.0f};

    // 管理対象チャンク群。範囲外で Unloaded 確定したものは TArray から除去する
    // (= Unloaded 状態の FChunkInfo は内部に残らない、というインバリアント)。
    TArray<FChunkInfo> m_Chunks;
};

} // namespace acs::game
