// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar P (Scale-Stream) — StreamingDirector 実装 (bridge スケルトン)
//
// 現フェーズ (P-1): chunk state machine + 範囲内/外判定 + Loading 上限 + simulated
// load time のみを実装。実 asset load は TODO に集約し、Phase P-2 で AssetBundle と
// 接続する (1 chunk = 1 AssetBundle 相当)。
//
// 設計メモ:
//   ・Unloaded 状態の ChunkInfo は内部 Array に残さない (= Find() == nullptr が Unloaded)。
//     これにより Array サイズはおおむね「保持半径内のチャンク数 + 範囲外で Unloading 進行中」
//     に収まる。view_radius=2 なら 25 chunks 前後。
//   ・Loading の同時上限は Promote 時点でのみチェック。既に Loading 中のチャンクが
//     その後の Tick() で進行することは妨げない (= 完了優先で枠を空ける方針)。
//   ・本スケルトンでは Unloading → Unloaded は同フレーム内で即遷移させる。
//     Phase P-2 で「Unloading 中は GPU resource flush 待ち」になる場合、ここに
//     カウンタを持たせて段階的に進める。
#include "gameframework/StreamingDirector.h"
#include "foundation/Log.h"
#include "math/Math.h"  // Floor

namespace acs::game {

void StreamingDirector::Init(f32 chunk_size, i32 view_radius_chunks) noexcept {
    _chunk_size  = chunk_size > 0.0f ? chunk_size : 100.0f;
    _view_radius = view_radius_chunks >= 0 ? view_radius_chunks : 2;
    // _max_concurrent_loads / _viewer_pos / _chunks は触らない
    // (Init を「設定値の更新」用途でも呼べるようにするため)。
}

void StreamingDirector::SetViewerPos(Vec2 pos) noexcept {
    _viewer_pos = pos;
}

void StreamingDirector::SetMaxConcurrentLoads(u32 n) noexcept {
    // 0 だと永遠に Queued から脱出できないので 1 にクランプ。
    _max_concurrent_loads = n > 0 ? n : 1;
}

StreamingDirector::ChunkInfo* StreamingDirector::Find(ChunkId id) noexcept {
    const usize n = _chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (_chunks[i].id == id) return &_chunks[i];
    }
    return nullptr;
}

const StreamingDirector::ChunkInfo* StreamingDirector::Find(ChunkId id) const noexcept {
    const usize n = _chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (_chunks[i].id == id) return &_chunks[i];
    }
    return nullptr;
}

ChunkId StreamingDirector::ViewerChunk() const noexcept {
    // chunk_size は Init で正値にクランプ済 → 0 除算なし。
    const f32 inv = 1.0f / _chunk_size;
    const i32 cx  = static_cast<i32>(Floor(_viewer_pos.x * inv));
    const i32 cy  = static_cast<i32>(Floor(_viewer_pos.y * inv));
    return ChunkId{cx, cy};
}

bool StreamingDirector::InRange(ChunkId id) const noexcept {
    const ChunkId v = ViewerChunk();
    const i32 dx = id.cx - v.cx;
    const i32 dy = id.cy - v.cy;
    // 矩形 (チェビシェフ) 範囲: max(|dx|, |dy|) <= view_radius。
    // 円形にしたい場合は Phase P-2 で Vec2 距離判定に切り替える。
    const i32 adx = dx >= 0 ? dx : -dx;
    const i32 ady = dy >= 0 ? dy : -dy;
    const i32 mx  = adx > ady ? adx : ady;
    return mx <= _view_radius;
}

void StreamingDirector::EnqueueInRange() noexcept {
    const ChunkId v = ViewerChunk();
    for (i32 dy = -_view_radius; dy <= _view_radius; ++dy) {
        for (i32 dx = -_view_radius; dx <= _view_radius; ++dx) {
            const ChunkId cid{v.cx + dx, v.cy + dy};
            if (Find(cid) != nullptr) continue;   // 既に管理下 (どの状態でも)
            ChunkInfo info{};
            info.id      = cid;
            info.state   = ChunkState::Queued;
            info.elapsed = 0.0f;
            _chunks.PushBack(info);
        }
    }
}

void StreamingDirector::PromoteQueuedToLoading() noexcept {
    // 現在 Loading 中のチャンクをカウント。
    u32 loading_now = 0;
    const usize n = _chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (_chunks[i].state == ChunkState::Loading) ++loading_now;
    }
    // 空きスロットが無ければ何もしない。
    if (loading_now >= _max_concurrent_loads) return;

    // 先頭から走査して Queued を Loading に昇格。
    // (Phase P-2 でビューア距離による優先度ソートを入れる予定。今はキュー順 = 挿入順。)
    for (usize i = 0; i < n; ++i) {
        if (_chunks[i].state != ChunkState::Queued) continue;
        _chunks[i].state   = ChunkState::Loading;
        _chunks[i].elapsed = 0.0f;
        // TODO(Phase P-2): ここで AssetBundle::BeginLoad() を発行する。
        //   _chunks[i].bundle.Add(ChunkAssetPath(cid));
        //   _chunks[i].bundle.BeginLoad();
        ++loading_now;
        if (loading_now >= _max_concurrent_loads) break;
    }
}

void StreamingDirector::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;   // 時間巻き戻し防止 (pause からの再開等)

    // 1. 範囲内チャンクを Queued として追加 (未登録分のみ)。
    EnqueueInRange();

    // 2. Loading の進捗を進めて完了で Loaded へ、範囲外 Loaded は Unloading へ。
    //    走査中に EraseSwap せず、Unloaded フラグを別途立てて後で一括除去する
    //    (Array のインデックス安定性を保つため)。
    for (usize i = 0; i < _chunks.Size(); ++i) {
        ChunkInfo& c = _chunks[i];
        switch (c.state) {
        case ChunkState::Loading: {
            c.elapsed += dt;
            // TODO(Phase P-2): bundle.IsLoaded() を見て遷移する。
            // 現状は simulated time での近似。
            if (c.elapsed >= kSimulatedLoadSeconds) {
                c.state   = ChunkState::Loaded;
                c.elapsed = 0.0f;
            }
            break;
        }
        case ChunkState::Loaded: {
            if (!InRange(c.id)) {
                // 範囲外に出た → Unloading 経由で破棄。
                c.state = ChunkState::Unloading;
            }
            break;
        }
        case ChunkState::Queued: {
            if (!InRange(c.id)) {
                // 範囲外に出た Queued は load 開始すらしていないので即 Unloaded。
                c.state = ChunkState::Unloading;
            }
            break;
        }
        case ChunkState::Unloading:
        case ChunkState::Unloaded:
        default:
            // Unloading は次ステップで Array から除去される。
            // Unloaded は通常 _chunks に存在しないが、防御的に no-op。
            break;
        }
    }

    // 3. Unloading のものを実際に破棄する。
    //    TODO(Phase P-2): ここで bundle.Unload() を呼び、Rc<Asset> を drop。
    //    Phase P-1 はメタデータだけなので Array から除去するのみ。
    {
        usize i = 0;
        while (i < _chunks.Size()) {
            if (_chunks[i].state == ChunkState::Unloading) {
                // swap-erase で O(1) 削除 (順序は保たない = ChunkInfo は (cx,cy) で識別)
                const usize last = _chunks.Size() - 1;
                if (i != last) _chunks[i] = _chunks[last];
                _chunks.PopBack();
            } else {
                ++i;
            }
        }
    }

    // 4. 同時 Loading 上限に空きがあれば Queued を昇格。
    PromoteQueuedToLoading();
}

ChunkState StreamingDirector::GetState(ChunkId id) const noexcept {
    const ChunkInfo* c = Find(id);
    return c != nullptr ? c->state : ChunkState::Unloaded;
}

u32 StreamingDirector::LoadedCount() const noexcept {
    u32 n = 0;
    const usize sz = _chunks.Size();
    for (usize i = 0; i < sz; ++i) {
        if (_chunks[i].state == ChunkState::Loaded) ++n;
    }
    return n;
}

u32 StreamingDirector::LoadingCount() const noexcept {
    u32 n = 0;
    const usize sz = _chunks.Size();
    for (usize i = 0; i < sz; ++i) {
        if (_chunks[i].state == ChunkState::Loading) ++n;
    }
    return n;
}

void StreamingDirector::ForceUnload(ChunkId id) noexcept {
    // swap-erase で即破棄 (TODO(Phase P-2): Loading 中だった場合は async load の cancel)。
    const usize n = _chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (_chunks[i].id == id) {
            const usize last = _chunks.Size() - 1;
            if (i != last) _chunks[i] = _chunks[last];
            _chunks.PopBack();
            return;
        }
    }
    // 見つからなければ既に Unloaded 扱い (no-op)。
    ACS_LOG_TRACE("StreamingDirector::ForceUnload: chunk (%d,%d) not found", id.cx, id.cy);
}

void StreamingDirector::ClearAll() noexcept {
    // TODO(Phase P-2): Loading 中の全 bundle の cancel + Loaded 全 bundle の Unload。
    _chunks.Clear();
}

} // namespace acs::game
