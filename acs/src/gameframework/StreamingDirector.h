// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar P (Scale-Stream) — CStreamingDirector (大規模シーン chunk streaming)
//
// 役割:
//   オープンワールド / 広大なステージで全アセットを常駐させられない場合に、
//   ビューア (カメラ) 位置を中心とした矩形範囲のチャンク (cx, cy) だけを
//   load 状態に保ち、範囲外を unload する。CCollisionWorld2D の SpatialGrid と
//   思想は似ているが、こちらは「アセットの load/unload」を扱う上位レイヤ。
//
// 使い方 (典型例):
//   acs::game::CStreamingDirector dir;
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
//   ・実アセットロード: 1 chunk = 1 CAssetBundle。SetAssetRegistry() で app 所有の
//     CAssetRegistry を差し込むと、Loading 遷移時に bundle.BeginLoad(registry) を発行し、
//     bundle.Progress()/IsLoaded() で実完了を判定、Unloading で bundle.Unload() する。
//     registry が未設定 (nullptr) のときは simulated load time = 0.5s/chunk の
//     フォールバックで進行する (ヘッドレステスト / registry を持たない用途向け)。
//   ・Pillar G の CAssetBundle と二段構え: CStreamingDirector が各チャンクの CAssetBundle
//     を保有し、チャンクごとのアセット集合 (パス) を SetChunkPathFormat() で決める。
//   ・非コピー・非ムーブ (内部 TArray 規約 / 単一所有を強制)。
//   ・全 API noexcept、STL 不使用 (acs::TArray<FChunkInfo> で管理)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "container/String.h"
#include "gameframework/Forward.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"

namespace acs { class CAssetRegistry; }

namespace acs::game {

/**
 * チャンクの 2D 整数座標 (cx, cy)。
 *
 * @details
 * FNodeId / FShapeId と異なり generation を持たない。同じ (cx, cy) は常に同じ
 * チャンクを指すため、比較は単純な値ベース。
 */
struct FChunkId {
    /** チャンクの X 座標 (= floor(world.x / chunk_size))。 */
    i32 cx = 0;

    /** チャンクの Y 座標 (= floor(world.y / chunk_size))。 */
    i32 cy = 0;

    /** 原点 (0, 0) のチャンク ID を構築する。 */
    constexpr FChunkId() noexcept = default;

    /**
     * 指定座標のチャンク ID を構築する。
     *
     * @param x チャンク X 座標。
     * @param y チャンク Y 座標。
     */
    constexpr FChunkId(i32 x, i32 y) noexcept : cx(x), cy(y) {}

    /**
     * 2 つのチャンク ID が同一座標かを返す。
     *
     * @param o 比較対象のチャンク ID。
     * @return cx・cy がともに一致すれば true。
     */
    constexpr bool operator==(FChunkId o) const noexcept { return cx == o.cx && cy == o.cy; }

    /**
     * 2 つのチャンク ID が異なる座標かを返す。
     *
     * @param o 比較対象のチャンク ID。
     * @return cx・cy のいずれかが異なれば true。
     */
    constexpr bool operator!=(FChunkId o) const noexcept { return !(*this == o); }
};

/**
 * チャンクのライフサイクル状態。
 *
 * @details
 * Unloaded はメモリ不在 (= 内部 m_Chunks に存在しないのと等価)。Queued は範囲内に
 * 入ったが Loading スロット空き待ち。Loading は実 asset load 進行中 (registry 接続時は
 * bundle.BeginLoad 済で完了待ち、非接続時は elapsed が積算される)。Loaded は使用可能で
 * 範囲外に出るまで保持。Unloading は範囲外に出て解放処理中 (bundle.Unload を発行して
 * 同フレームで破棄)。
 */
enum class EChunkState : u8 {
    /** メモリ不在 (内部テーブルに存在しないのと等価)。 */
    Unloaded  = 0,

    /** 範囲内に入ったが Loading スロットの空き待ち。 */
    Queued    = 1,

    /** 実 asset load 進行中 (registry 接続時は完了待ち、非接続時は elapsed 積算)。 */
    Loading   = 2,

    /** ロード完了で使用可能。範囲外に出るまで保持される。 */
    Loaded    = 3,

    /** 範囲外に出て解放処理中 (bundle.Unload 発行 → 同フレーム破棄)。 */
    Unloading = 4,
};

/**
 * ビューアを中心とした矩形範囲のチャンクを load/unload 管理するディレクタ。
 *
 * @details
 * ライフサイクルは Init() → 毎フレーム SetViewerPos + Tick → ClearAll() (シーン終了時)。
 * Tick() は (1) 範囲内チャンクを Queued に挿入、(2) Loading 上限内で
 * Queued→Loading→Loaded を進行、(3) 範囲外の Loaded を Unloading→Unloaded に遷移、を
 * 行う。SetAssetRegistry() で実 CAssetRegistry を差すと各チャンクが 1 CAssetBundle を
 * 実ロードし、未設定時は simulated load time でフォールバックする。non-copy / non-move。
 */
class CStreamingDirector {
public:
    /** 空状態で構築する (設定は Init で行う)。 */
    CStreamingDirector() noexcept = default;

    /**
     * 全チャンクを破棄する。
     *
     * @details
     * TUniquePtr<CAssetBundle> の破棄に CAssetBundle の完全型が必要なため、本体は
     * .cpp 側で定義する (ヘッダのみ include する TU で不完全型 delete を避ける)。
     */
    ~CStreamingDirector() noexcept;

    /** コピー禁止 (内部 TArray と単一所有を強制するため)。 */
    CStreamingDirector(const CStreamingDirector&)            = delete;

    /** コピー代入も禁止。 */
    CStreamingDirector& operator=(const CStreamingDirector&) = delete;

    /** ムーブ禁止。 */
    CStreamingDirector(CStreamingDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CStreamingDirector& operator=(CStreamingDirector&&)      = delete;

    /**
     * チャンクサイズと保持半径を設定する。
     *
     * @details
     * 0 以下 / 不正値は既定値にフォールバックする。多重呼び出しは値の更新のみで、
     * 既存チャンクの再評価は次の Tick() で自動的に行われる。
     * @param chunk_size 1 チャンクの world 座標サイズ (典型値 100.0f = 100m 四方)。
     * @param view_radius_chunks ビューアチャンクを中心とした保持半径 (chunks 数)。例: 2 → 5x5 = 25 chunks を Loaded に保つ。0 でも 1x1 だけは保持される。
     */
    void Init(f32 chunk_size = 100.0f, i32 view_radius_chunks = 2) noexcept;

    /**
     * 実アセットロード用の registry を差し込む。
     *
     * @details
     * app 所有 (CApplication::GetAssets() / CGame 経由、非所有 raw ptr)。設定すると各
     * チャンクが Loading に入るとき CAssetBundle::BeginLoad(*registry) を発行し、進捗/
     * 完了を bundle から取得する。nullptr を渡す (= 未設定) と simulated load time
     * フォールバックで動作する。既に Loading 中のチャンクには影響しない (次に Loading へ
     * 昇格するものから適用)。
     * @param registry 接続する CAssetRegistry (nullptr で simulated フォールバック)。
     */
    void            SetAssetRegistry(CAssetRegistry* registry) noexcept { m_Registry = registry; }

    /**
     * 現在接続中の registry を返す。
     *
     * @return 設定済みの CAssetRegistry (未設定なら nullptr)。
     */
    CAssetRegistry* GetAssetRegistry() const noexcept { return m_Registry; }

    /**
     * チャンク (cx, cy) → アセットパスを組み立てる printf 風フォーマットを設定する。
     *
     * @details
     * 第 1, 第 2 引数に cx, cy (いずれも i32) が渡される。既定は
     * "chunks/chunk_%d_%d.bundle"。内部で FString::AppendFormat に転送するため、必ず
     * "%d" を 2 つこの順で含めること。fmt が nullptr / 既定どおりで良ければ呼ぶ必要は
     * ない。registry 未設定時はパスは使われない (simulated path のため no-op)。
     * @param fmt cx, cy を埋め込む printf 風フォーマット文字列。
     */
    void SetChunkPathFormat(const char* fmt) noexcept;

    /**
     * ビューア (カメラ) のワールド座標を更新する。
     *
     * @details 次の Tick() で必要なチャンクの load/unload が再評価される。
     * @param pos 新しいビューアのワールド座標。
     */
    void SetViewerPos(FVec2 pos) noexcept;

    /**
     * 同時 Loading 数の上限を設定する。
     *
     * @details
     * 既定 4。0 を渡した場合 1 にクランプ。Loading 中の数がこれを超えると新規 Queued は
     * 次フレームに繰り越される。
     * @param n 同時 Loading 数の上限。
     */
    void SetMaxConcurrentLoads(u32 n) noexcept;

    /**
     * 毎フレーム呼んでチャンクの load/unload を進める。
     *
     * @details
     * (1) ビューア周辺の必要チャンクを Queued として挿入 (重複は無視)、(2) Loading の
     * 進捗を dt 加算で進め、完了で Loaded に遷移、(3) 範囲外の Loaded を Unloading → 即
     * Unloaded (= m_Chunks から除去) に、(4) Loading 上限の範囲で Queued → Loading に
     * 昇格、を行う。dt < 0 は 0 にクランプする (時間巻き戻し防止)。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 指定チャンクの現状態を返す。
     *
     * @param id 状態を問い合わせるチャンク ID。
     * @return チャンクの EChunkState (未登録チャンクは Unloaded)。
     */
    EChunkState GetState(FChunkId id) const noexcept;

    /**
     * Loaded 状態のチャンク数を返す。
     *
     * @return Loaded 状態のチャンク数。
     */
    u32 LoadedCount() const noexcept;

    /**
     * Loading 状態のチャンク数を返す (Queued は含まない)。
     *
     * @return Loading 状態のチャンク数。
     */
    u32 LoadingCount() const noexcept;

    /**
     * 指定チャンクを強制的に Unloaded にする (デバッグ / メモリ圧追従用)。
     *
     * @details
     * Loading 中でも即座に破棄する (bundle.Unload で TSharedPtr を drop。CAssetBundle は
     * 同期ロードなので「進行中の async load」は無く、cancel 不要で即時解放できる)。範囲
     * 内にあれば次 Tick() で再び Queued に戻る点に注意。
     * @param id 強制 unload するチャンク ID。
     */
    void ForceUnload(FChunkId id) noexcept;

    /** 全チャンクを破棄する (シーン遷移時に呼ぶ)。 */
    void ClearAll() noexcept;

private:
    /**
     * 1 チャンクの管理情報。
     *
     * @details
     * elapsed は simulated フォールバック時の Loading 中のみ意味を持つ (registry 接続時は
     * bundle 進捗を見るので未使用、Loaded 到達後はリセットされる)。bundle は registry
     * 接続時のみ生成される (MakeUnique で遅延生成し、Unloading で破棄)。path は bundle が
     * const char* を借用するため FChunkInfo が所有する必要がある (CAssetBundle::Add は
     * 文字列を借用するだけ = bundle より長寿命であること)。FString / TUniquePtr メンバを
     * 持つため FChunkInfo はムーブのみ可 (コピー不可) で、TArray の swap-erase / Grow は
     * Move 経路を使うこと。
     */
    struct FChunkInfo {
        /** チャンクの 2D 整数座標。 */
        FChunkId               id      {};

        /** チャンクの現在のライフサイクル状態。 */
        EChunkState            state   = EChunkState::Unloaded;

        /** simulated フォールバック Loading 中の経過秒 (registry 接続時は未使用)。 */
        f32                   elapsed = 0.0f;

        /** チャンクのアセットパス ("chunks/chunk_<cx>_<cy>.bundle" 等。bundle が借用)。 */
        FString                path;

        /** チャンクの実アセットロード単位 (registry 接続時のみ生成、非接続時は null)。 */
        TUniquePtr<CAssetBundle> bundle;
    };

    /**
     * 同名 (cx, cy) のチャンクを検索する。
     *
     * @param id 探すチャンク ID。
     * @return 見つかった FChunkInfo へのポインタ (無ければ nullptr)。
     */
    FChunkInfo*       Find(FChunkId id) noexcept;

    /**
     * 同名 (cx, cy) のチャンクを検索する (const 版)。
     *
     * @param id 探すチャンク ID。
     * @return 見つかった FChunkInfo への const ポインタ (無ければ nullptr)。
     */
    const FChunkInfo* Find(FChunkId id) const noexcept;

    /**
     * ビューア位置から算出したビューアチャンク座標を返す。
     *
     * @return ビューアが属するチャンク (cx, cy)。
     */
    FChunkId ViewerChunk() const noexcept;

    /**
     * 指定チャンクがビューア中心の view_radius 矩形に入っているかを返す。
     *
     * @param id 判定するチャンク ID。
     * @return 範囲内なら true。
     */
    bool InRange(FChunkId id) const noexcept;

    /** 範囲内チャンクを走査し、未登録分を Queued として追加する。 */
    void EnqueueInRange() noexcept;

    /**
     * Queued のチャンクを Loading へ昇格する (max_concurrent_loads を遵守)。
     *
     * @details registry 接続時は bundle を生成して BeginLoad、非接続時は elapsed をリセットする。
     */
    void PromoteQueuedToLoading() noexcept;

    /**
     * チャンクの bundle を生成し、パスを Add して BeginLoad を発行する。
     *
     * @details registry が null のときは何もしない (simulated 経路で進行)。
     * @param c ロードを開始するチャンク情報。
     */
    void BeginChunkLoad(FChunkInfo& c) noexcept;

    /** registry 未接続時の simulated load time (秒/chunk、フォールバック)。 */
    static constexpr f32 kSimulatedLoadSeconds = 0.5f;

    /** SetChunkPathFormat 未設定時の既定パスフォーマット (cx, cy = i32)。 */
    static constexpr const char* kDefaultChunkPathFormat = "chunks/chunk_%d_%d.bundle";

    /** 1 チャンクの world 座標サイズ (Init 未呼出時のフォールバック既定)。 */
    f32 m_ChunkSize           = 100.0f;

    /** ビューアチャンク中心の保持半径 (chunks 数、Init 未呼出時の既定)。 */
    i32 m_ViewRadius          = 2;

    /** 同時 Loading 数の上限。 */
    u32 m_MaxConcurrentLoads = 4;

    /** 実アセットロード接続先 (非所有)。null なら simulated フォールバック。 */
    CAssetRegistry* m_Registry = nullptr;

    /** チャンクパス組み立てフォーマット (空なら kDefaultChunkPathFormat を使用)。 */
    FString m_ChunkPathFormat;

    /** 現在のビューアのワールド座標。 */
    FVec2 m_ViewerPos = {0.0f, 0.0f};

    /**
     * 管理対象チャンク群。
     *
     * @details 範囲外で Unloaded 確定したものは TArray から除去する (= Unloaded 状態の FChunkInfo は内部に残らない、というインバリアント)。
     */
    TArray<FChunkInfo> m_Chunks;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FStreamingDirector = CStreamingDirector;

} // namespace acs::game
