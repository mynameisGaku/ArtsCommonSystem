// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar P (Scale-Stream) — CStreamingDirector 実装 (CAssetBundle 実接続)
//
// chunk state machine + 範囲内/外判定 + Loading 上限 に加え、実 asset load を
// チャンクごとの CAssetBundle で行う。SetAssetRegistry() で app 所有の CAssetRegistry を
// 差し込むと、Loading 遷移時に bundle.BeginLoad(registry) を発行し、bundle.Progress() /
// IsLoaded() で完了を判定、Unloading で bundle.Unload() する。registry 未設定 (nullptr)
// のときは simulated load time = 0.5s/chunk のフォールバックで動作する。
//
// 設計メモ:
//   ・Unloaded 状態の FChunkInfo は内部 TArray に残さない (= Find() == nullptr が Unloaded)。
//     これにより TArray サイズはおおむね「保持半径内のチャンク数 + 範囲外で Unloading 進行中」
//     に収まる。view_radius=2 なら 25 chunks 前後。
//   ・Loading の同時上限は Promote 時点でのみチェック。既に Loading 中のチャンクが
//     その後の Tick() で進行することは妨げない (= 完了優先で枠を空ける方針)。
//   ・Unloading → Unloaded は同フレーム内で即遷移させる (bundle.Unload を発行してから
//     TArray から除去)。CAssetBundle は同期 Load なので Unload も即時で完結する。
//   ・FChunkInfo は FString / TUniquePtr<CAssetBundle> を持つためムーブのみ可。TArray の
//     swap-erase は RemoveAtSwap (内部 Move 代入) を使い、コピー代入を避ける。
#include "gameframework/StreamingDirector.h"
#include "gameframework/AssetBundle.h"   // 実 asset load 単位 (BeginLoad/Progress/IsLoaded/Unload)
#include "foundation/Log.h"
#include "math/Math.h"  // Floor

namespace acs::game {

CStreamingDirector::~CStreamingDirector() noexcept {
    // TArray<FChunkInfo> の破棄で各 FChunkInfo::bundle (TUniquePtr<CAssetBundle>) が
    // drop される。ここは CAssetBundle 完全型が見えている TU なので不完全型 delete に
    // ならない (= dtor をヘッダで = default にしない理由)。
    // 各 bundle の Unload は TSharedPtr<Asset> drop で暗黙に行われるため明示呼び出しは不要。
}

void CStreamingDirector::Init(f32 chunk_size, i32 view_radius_chunks) noexcept {
    m_ChunkSize  = chunk_size > 0.0f ? chunk_size : 100.0f;
    m_ViewRadius = view_radius_chunks >= 0 ? view_radius_chunks : 2;
    // m_MaxConcurrentLoads / m_ViewerPos / m_Chunks は触らない
    // (Init を「設定値の更新」用途でも呼べるようにするため)。
}

void CStreamingDirector::SetViewerPos(FVec2 pos) noexcept {
    m_ViewerPos = pos;
}

void CStreamingDirector::SetMaxConcurrentLoads(u32 n) noexcept {
    // 0 だと永遠に Queued から脱出できないので 1 にクランプ。
    m_MaxConcurrentLoads = n > 0 ? n : 1;
}

void CStreamingDirector::SetChunkPathFormat(const char* fmt) noexcept {
    if (fmt == nullptr || fmt[0] == 0) {
        // 空指定は既定フォーマットに戻す。
        m_ChunkPathFormat.Clear();
        return;
    }
    m_ChunkPathFormat.Clear();
    m_ChunkPathFormat.Append(FStringView(fmt));
}

CStreamingDirector::FChunkInfo* CStreamingDirector::Find(FChunkId id) noexcept {
    const usize n = m_Chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].id == id) return &m_Chunks[i];
    }
    return nullptr;
}

const CStreamingDirector::FChunkInfo* CStreamingDirector::Find(FChunkId id) const noexcept {
    const usize n = m_Chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].id == id) return &m_Chunks[i];
    }
    return nullptr;
}

FChunkId CStreamingDirector::ViewerChunk() const noexcept {
    // chunk_size は Init で正値にクランプ済 → 0 除算なし。
    const f32 inv = 1.0f / m_ChunkSize;
    const i32 cx  = static_cast<i32>(Floor(m_ViewerPos.x * inv));
    const i32 cy  = static_cast<i32>(Floor(m_ViewerPos.y * inv));
    return FChunkId{cx, cy};
}

bool CStreamingDirector::InRange(FChunkId id) const noexcept {
    const FChunkId v = ViewerChunk();
    const i32 dx = id.cx - v.cx;
    const i32 dy = id.cy - v.cy;
    // 矩形 (チェビシェフ) 範囲: max(|dx|, |dy|) <= view_radius。
    // 円形にしたい場合は FVec2 距離判定に切り替える (現状は矩形でチャンク数を確定させる)。
    const i32 adx = dx >= 0 ? dx : -dx;
    const i32 ady = dy >= 0 ? dy : -dy;
    const i32 mx  = adx > ady ? adx : ady;
    return mx <= m_ViewRadius;
}

void CStreamingDirector::EnqueueInRange() noexcept {
    const FChunkId v = ViewerChunk();
    for (i32 dy = -m_ViewRadius; dy <= m_ViewRadius; ++dy) {
        for (i32 dx = -m_ViewRadius; dx <= m_ViewRadius; ++dx) {
            const FChunkId cid{v.cx + dx, v.cy + dy};
            if (Find(cid) != nullptr) continue;   // 既に管理下 (どの状態でも)
            // FChunkInfo は move-only (FString / TUniquePtr 保有) のため EmplaceBack で
            // その場構築し、戻り参照にフィールドを設定する (コピー PushBack は使えない)。
            FChunkInfo& info = m_Chunks.EmplaceBack();
            info.id      = cid;
            info.state   = EChunkState::Queued;
            info.elapsed = 0.0f;
        }
    }
}

void CStreamingDirector::BeginChunkLoad(FChunkInfo& c) noexcept {
    // registry 未接続なら実ロードはしない (simulated 経路で elapsed が進む)。
    if (m_Registry == nullptr) return;

    // チャンクパスを組み立てる。CAssetBundle::Add は const char* を「借用」するため、
    // bundle が借用するポインタは bundle 寿命中ずっと不変アドレスでなければならない。
    // FChunkInfo は TArray 内で move される (Grow / RemoveAtSwap) が、FString が SSO の
    // 場合 move でインラインバッファのアドレスが変わり、bundle の借用ポインタが dangling
    // になる。これを避けるため path を必ずヒープへ退避させ (Reserve で SSO 上限超え →
    // Grow)、move しても Data() が m_Heap.data の安定アドレスを返すようにする。
    const char* fmt = m_ChunkPathFormat.IsEmpty()
                          ? kDefaultChunkPathFormat
                          : m_ChunkPathFormat.Data();
    c.path.Clear();
    c.path.Reserve(FString::kSsoCapacity + 1);  // SSO を外してヒープ確保 → アドレス安定
    c.path.AppendFormat(fmt, c.id.cx, c.id.cy);

    // bundle を遅延生成 (Unloading で破棄 → 再 Loading で作り直し)。
    c.bundle = MakeUnique<CAssetBundle>();
    if (!c.bundle) {
        // 生成失敗 (確保不能) は simulated 経路へフォールバック。
        ACS_LOG_WARN("CStreamingDirector::BeginChunkLoad: bundle 生成失敗 (%d,%d)",
                     c.id.cx, c.id.cy);
        return;
    }
    // c.path.Data() は NUL 終端で bundle 寿命中有効 (FChunkInfo が path を所有)。
    c.bundle->Add(c.path.Data());
    c.bundle->BeginLoad(*m_Registry);
}

void CStreamingDirector::PromoteQueuedToLoading() noexcept {
    // 現在 Loading 中のチャンクをカウント。
    u32 loading_now = 0;
    const usize n = m_Chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].state == EChunkState::Loading) ++loading_now;
    }
    // 空きスロットが無ければ何もしない。
    if (loading_now >= m_MaxConcurrentLoads) return;

    // 先頭から走査して Queued を Loading に昇格。
    // (キュー順 = 挿入順。ビューア距離優先度は将来 EnqueueInRange を中心優先で並べる余地あり。)
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].state != EChunkState::Queued) continue;
        m_Chunks[i].state   = EChunkState::Loading;
        m_Chunks[i].elapsed = 0.0f;
        // registry 接続時は bundle.BeginLoad を発行 (同期ロード)。非接続時は no-op で
        // simulated time 経路に乗る。CAssetBundle::BeginLoad は同期だが、bundle.IsLoaded()
        // ポーリングで一貫した状態遷移を保つ (失敗 entry も「完了」として Loaded に進む)。
        BeginChunkLoad(m_Chunks[i]);
        ++loading_now;
        if (loading_now >= m_MaxConcurrentLoads) break;
    }
}

void CStreamingDirector::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;   // 時間巻き戻し防止 (pause からの再開等)

    // 1. 範囲内チャンクを Queued として追加 (未登録分のみ)。
    EnqueueInRange();

    // 2. Loading の進捗を進めて完了で Loaded へ、範囲外 Loaded は Unloading へ。
    //    走査中に EraseSwap せず、Unloaded フラグを別途立てて後で一括除去する
    //    (TArray のインデックス安定性を保つため)。
    for (usize i = 0; i < m_Chunks.Size(); ++i) {
        FChunkInfo& c = m_Chunks[i];
        switch (c.state) {
        case EChunkState::Loading: {
            if (c.bundle) {
                // registry 接続: bundle の集約進捗で完了を判定する。
                // CAssetBundle::BeginLoad は同期ロードのため、Loading に入った直後の
                // 本フレームで IsLoaded() == true になるのが通常 (失敗 entry も完了扱い)。
                // 将来 bundle を非同期化しても、この polling ループはそのまま機能する。
                if (c.bundle->IsLoaded()) {
                    c.state   = EChunkState::Loaded;
                    c.elapsed = 0.0f;
                }
            } else {
                // registry 未接続フォールバック: simulated time での近似進行。
                c.elapsed += dt;
                if (c.elapsed >= kSimulatedLoadSeconds) {
                    c.state   = EChunkState::Loaded;
                    c.elapsed = 0.0f;
                }
            }
            break;
        }
        case EChunkState::Loaded: {
            if (!InRange(c.id)) {
                // 範囲外に出た → Unloading 経由で破棄。
                c.state = EChunkState::Unloading;
            }
            break;
        }
        case EChunkState::Queued: {
            if (!InRange(c.id)) {
                // 範囲外に出た Queued は load 開始すらしていないので即 Unloaded。
                c.state = EChunkState::Unloading;
            }
            break;
        }
        case EChunkState::Unloading:
        case EChunkState::Unloaded:
        default:
            // Unloading は次ステップで TArray から除去される。
            // Unloaded は通常 m_Chunks に存在しないが、防御的に no-op。
            break;
        }
    }

    // 3. Unloading のものを実際に破棄する。
    //    bundle.Unload() で保持していた TSharedPtr<Asset> を drop し (refcount を落とし)、
    //    TArray からも除去する。RemoveAtSwap は内部で Move 代入を使うため、move-only な
    //    FChunkInfo (FString / TUniquePtr 保有) でも安全に O(1) 削除できる
    //    (順序は保たない = FChunkInfo は (cx,cy) で識別するので問題なし)。
    {
        usize i = 0;
        while (i < m_Chunks.Size()) {
            if (m_Chunks[i].state == EChunkState::Unloading) {
                if (m_Chunks[i].bundle) m_Chunks[i].bundle->Unload();
                m_Chunks.RemoveAtSwap(i);
                // RemoveAtSwap で末尾要素が i に来るため i は進めない (同位置を再評価)。
            } else {
                ++i;
            }
        }
    }

    // 4. 同時 Loading 上限に空きがあれば Queued を昇格。
    PromoteQueuedToLoading();
}

EChunkState CStreamingDirector::GetState(FChunkId id) const noexcept {
    const FChunkInfo* c = Find(id);
    return c != nullptr ? c->state : EChunkState::Unloaded;
}

u32 CStreamingDirector::LoadedCount() const noexcept {
    u32 n = 0;
    const usize sz = m_Chunks.Size();
    for (usize i = 0; i < sz; ++i) {
        if (m_Chunks[i].state == EChunkState::Loaded) ++n;
    }
    return n;
}

u32 CStreamingDirector::LoadingCount() const noexcept {
    u32 n = 0;
    const usize sz = m_Chunks.Size();
    for (usize i = 0; i < sz; ++i) {
        if (m_Chunks[i].state == EChunkState::Loading) ++n;
    }
    return n;
}

void CStreamingDirector::ForceUnload(FChunkId id) noexcept {
    // swap-erase で即破棄。Loading 中だった場合も bundle.Unload で TSharedPtr を即 drop する
    // (CAssetBundle は同期ロードなので「進行中の async load」は存在しない)。
    const usize n = m_Chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].id == id) {
            if (m_Chunks[i].bundle) m_Chunks[i].bundle->Unload();
            m_Chunks.RemoveAtSwap(i);  // 内部 Move 代入で move-only FChunkInfo に対応
            return;
        }
    }
    // 見つからなければ既に Unloaded 扱い (no-op)。
    ACS_LOG_TRACE("CStreamingDirector::ForceUnload: chunk (%d,%d) not found", id.cx, id.cy);
}

void CStreamingDirector::ClearAll() noexcept {
    // 全チャンクの bundle を Unload してから破棄する (Loaded/Loading 問わず TSharedPtr を drop)。
    // FChunkInfo のデストラクタ (TUniquePtr<CAssetBundle> drop) でも実体は解放されるが、
    // Unload を明示することで registry refcount を決定的タイミングで落とす。
    const usize n = m_Chunks.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Chunks[i].bundle) m_Chunks[i].bundle->Unload();
    }
    m_Chunks.Clear();
}

} // namespace acs::game
