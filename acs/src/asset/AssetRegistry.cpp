// SPDX-License-Identifier: Apache-2.0
// CAssetRegistry 実装
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "asset/TextAsset.h"
#include "asset/ImageAsset.h"
#include "asset/AudioAsset.h"
#include "asset/MeshAsset.h"
#include "asset/cinematics/CCinematicAssetLoader.h"
#include "platform/FileSystem.h"
#include "threading/ScopedLock.h"
#include "threading/Thread.h"
#include "memory/UniquePtr.h"
#include "foundation/Limits.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * wchar_t パスから拡張子を小文字 ASCII で取り出す。
 *
 * @details
 * 末尾から '.' を探し、見つかった拡張子を小文字化して out にコピーする。'.' が
 * 無ければ空文字列。ASCII 範囲外の文字は '?' に置換する (一般的でないため扱わない)。
 * @param path 拡張子を取り出す対象のパス。
 * @param out 拡張子を書き込む NUL 終端バッファ。
 * @param cap out バッファの容量 (バイト数)。
 */
void ExtractExtensionAscii(const wchar_t* path, char* out, usize cap) noexcept {
    // 末尾から '.' を探す
    usize len = 0;
    while (path[len]) ++len;
    isize dot = -1;
    for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
        if (path[i] == L'.') { dot = i; break; }
        if (path[i] == L'\\' || path[i] == L'/') break;  // ディレクトリ区切りで終了
    }
    if (dot < 0) { if (cap) out[0] = 0; return; }
    // 拡張子部分をコピーして小文字化
    usize w = 0;
    for (usize i = static_cast<usize>(dot) + 1; i < len && w + 1 < cap; ++i) {
        wchar_t c = path[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        out[w++] = (c < 128) ? static_cast<char>(c) : '?';
    }
    out[w] = 0;
}

/**
 * 2 つの NUL 終端 ASCII 文字列が等しいかを返す。
 *
 * @param a 比較する文字列 1。
 * @param b 比較する文字列 2。
 * @return 完全一致すれば true。
 */
bool StrEqAscii(const char* a, const char* b) noexcept {
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

u16 ValidateRegistryPath(const wchar_t* Path) noexcept
{
    if (Path == nullptr) return kAssetRegistrySubNullPath;
    usize Length = 0;
    while (Length <= kAssetRegistryMaxPathLength && Path[Length] != L'\0') {
        ++Length;
    }
    if (Length == 0) return kAssetRegistrySubInvalidPath;
    if (Length > kAssetRegistryMaxPathLength) return kAssetRegistrySubPathTooLong;
    return 0;
}

bool IsValidLoaderExtension(const char* Extension) noexcept
{
    if (Extension == nullptr || Extension[0] == '\0') return false;
    if (Extension[0] == '*' && Extension[1] == '\0') return true;
    for (usize Index = 0; Index < 31u; ++Index) {
        const char Character = Extension[Index];
        if (Character == '\0') return Index > 0;
        if (!((Character >= 'a' && Character <= 'z') ||
              (Character >= '0' && Character <= '9') ||
              Character == '_')) {
            return false;
        }
    }
    return false;
}

/**
 * パスから FAssetId を作る (wchar_t 列をバイト列としてハッシュ)。
 *
 * @param Path ハッシュ元のパス。
 * @return パスから生成した FAssetId。
 */
FAssetId MakeIdFromPath(const wchar_t* Path) noexcept
{
    /** NUL を除く UTF-16 path 長。 */
    usize Length = 0u;
    while (Path[Length] != L'\0') ++Length;
    return MakeAssetId(FStringView(reinterpret_cast<const char*>(Path), Length * sizeof(wchar_t)));
}

/** ロック外ロード処理の全 return 経路で処理中カウントを戻す。 */
class FActiveOperationGuard {
public:
    explicit FActiveOperationGuard(FCompletionCounter* counter) noexcept : m_Counter(counter)
    {
    }
    ~FActiveOperationGuard() noexcept
    {
        if (m_Counter) m_Counter->Done();
    }

    FActiveOperationGuard(const FActiveOperationGuard&) = delete;
    FActiveOperationGuard& operator=(const FActiveOperationGuard&) = delete;

private:
    FCompletionCounter* m_Counter = nullptr;
};

/** 即時errorを完了済みfutureとして返し、共有状態OOM時だけ空futureを返す。 */
FAssetFuture MakeCompletedAssetError(FErrorCode Error) noexcept
{
    /** error結果を所有する完了済み共有状態。 */
    auto State = MakeShared<FAsyncLoadState>();
    if (!State.Get()) return FAssetFuture{};
    State->error = Error;
    State->has_error = true;
    State->counter.Done();
    return FAssetFuture(Move(State));
}

} // namespace

CAssetRegistry::~CAssetRegistry() noexcept
{
    Shutdown();
}

void CAssetRegistry::RegisterLoader(IAssetLoader* loader) noexcept {
    (void)TryRegisterLoader(loader);
}

TResult<void> CAssetRegistry::TryRegisterLoader(IAssetLoader* loader) noexcept {
    if (!loader) {
        return ACS_ERR(Asset, kAssetRegistrySubInvalidLoader,
                       "CAssetRegistry::TryRegisterLoader: null loader");
    }
    const char* const Extension = loader->Extension();
    if (!IsValidLoaderExtension(Extension)) {
        return ACS_ERR(Asset, kAssetRegistrySubInvalidExtension,
                       "CAssetRegistry::TryRegisterLoader: invalid extension");
    }

    FScopedLock lk(m_Lock);
    if (m_Closing) {
        return ACS_ERR(Asset, kAssetRegistrySubShuttingDown,
                       "CAssetRegistry::TryRegisterLoader: registry is shutting down");
    }
    for (usize Index = 0; Index < m_Loaders.Num(); ++Index) {
        if (StrEqAscii(m_Loaders[Index]->Extension(), Extension)) {
            return ACS_ERR(Asset, kAssetRegistrySubDuplicateLoader,
                           "CAssetRegistry::TryRegisterLoader: duplicate extension");
        }
    }
    if (!m_Loaders.TryAdd(loader)) {
        return ACS_ERR(Memory, kAssetRegistrySubOutOfMemory,
                       "CAssetRegistry::TryRegisterLoader: allocation failed");
    }
    return Ok();
}

TResult<TSharedPtr<AAsset>> CAssetRegistry::TryPublishLoadedAsset(FAssetId id, TSharedPtr<AAsset> asset) noexcept
{
    if (!asset.Get()) {
        return ACS_ERR(Asset, kAssetRegistrySubNullAsset, "CAssetRegistry::TryPublishLoadedAsset: null asset");
    }

    FScopedLock lock(m_Lock);
    /** 同時loadが先に公開したcanonical asset。 */
    const TSharedPtr<AAsset>* const existing = m_Cache.Find(id);
    if (existing != nullptr && existing->Get() != nullptr) {
        return TResult<TSharedPtr<AAsset>>(OkInit, *existing);
    }

    if (!m_Closing && !m_Cache.TryAdd(id, asset)) {
        return ACS_ERR(Memory, kAssetRegistrySubOutOfMemory, "CAssetRegistry::TryPublishLoadedAsset: cache allocation failed");
    }

    // cache追加またはshutdown中の非保持成功が確定した後は失敗経路が無い。
    asset->SetId(id);
    asset->SetState(EAssetState::Ready);
    return TResult<TSharedPtr<AAsset>>(OkInit, Move(asset));
}

IAssetLoader* CAssetRegistry::FindLoader(const wchar_t* path) noexcept {
    char ext[32]{};
    ExtractExtensionAscii(path, ext, sizeof(ext));
    IAssetLoader* fallback = nullptr;
    for (usize i = 0; i < m_Loaders.Num(); ++i) {
        const char* const e = m_Loaders[i]->Extension();
        if (StrEqAscii(e, "*")) fallback = m_Loaders[i];
        if (StrEqAscii(e, ext)) return m_Loaders[i];
    }
    return fallback;  // 拡張子マッチなければ "*" のフォールバックを返す
}

TResult<TSharedPtr<AAsset>> CAssetRegistry::Load(const wchar_t* path) noexcept {
    const u16 PathError = ValidateRegistryPath(path);
    if (PathError != 0) {
        return ACS_ERR(Asset, PathError, "CAssetRegistry::Load: invalid path");
    }

    const FAssetId id = MakeIdFromPath(path);
    IAssetLoader* loader = nullptr;
    TSharedPtr<AAsset> cached;
    {
        FScopedLock lk(m_Lock);
        if (m_Closing) {
            return ACS_ERR(Asset, kAssetRegistrySubShuttingDown,
                           "CAssetRegistry is shutting down");
        }

        const TSharedPtr<AAsset>* hit = m_Cache.Find(id);
        if (hit && hit->Get()) {
            cached = *hit;
            ++m_CacheHitCount;
        } else {
            loader = FindLoader(path);
            // loader と this をロック外で参照する前に登録する。Shutdown はこの値が 0 に
            // 戻るまでキャッシュ・ローダ配列・レジストリ本体を破棄しない。
            if (loader) {
                m_ActiveOperations.Add(1);
                ++m_PhysicalFileReadCount;
            }
        }
    }

    if (cached.Get()) return TResult<TSharedPtr<AAsset>>(OkInit, Move(cached));
    if (!loader) {
        return ACS_ERR(Asset, kAssetRegistrySubNoLoader, "no loader for this asset path");
    }
    FActiveOperationGuard operation(&m_ActiveOperations);

    // ファイルを読み込む
    auto bytes_r = CFileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<TSharedPtr<AAsset>>(bytes_r.Error());

    // ローダ呼び出し
    auto asset_r = loader->LoadFromBytes(id, bytes_r.Value());
    if (asset_r.IsErr()) return Err<TSharedPtr<AAsset>>(asset_r.Error());

    TSharedPtr<AAsset> a = Move(asset_r.Value());
    if (!a.Get())  // ローダが OkInit で null を返した場合 (alloc 失敗等) の null-deref 回避
        return ACS_ERR(Asset, kAssetRegistrySubNullAsset, "loader returned null asset");
    return TryPublishLoadedAsset(id, Move(a));
}

namespace {
/** 非同期ロードワーカーに渡すジョブ引数 (heap 確保し所有権をワーカーへ渡す)。 */
struct FAsyncLoadJob {
    /** このジョブ本体を確保したアロケータ。ワーカースレッドでの解放にも同じものを使う。 */
    IAllocator* allocator = nullptr;

    /** キャッシュ挿入先のレジストリ。 */
    CAssetRegistry*           registry  = nullptr;

    /** 結果書き込み先 (worker と future で共有)。 */
    TSharedPtr<FAsyncLoadState>       state;

    /** レジストリの Shutdown が待つ処理中カウンタ。 */
    FCompletionCounter* active_operations = nullptr;

    /** 実行するローダ。 */
    IAssetLoader*            loader    = nullptr;

    /** ロード対象のアセット ID。 */
    FAssetId                  id        = FAssetId{};

    /** ワーカー完了まで共有する不変パス。 */
    TSharedPtr<FInternedAssetPath> path;
};
} // namespace

/**
 * 非同期ロードを実行するワーカー関数 (CThreadPool から呼ばれる)。
 *
 * @details
 * ファイル読み込み → ローダ呼び出し → キャッシュ挿入を行い、結果かエラーを
 * state に書き込んで counter を Done() する。ジョブは TUniquePtr のスコープ抜けで自動 delete される。
 * @param user AsyncLoadJob* を指す不透明ポインタ (所有権を受け取る)。
 * @param worker 呼び出し元ワーカーのインデックス (未使用)。
 */
void CAssetRegistry::AsyncLoadWorker(void* user, u32 /*worker*/) noexcept {
    FAsyncLoadJob* const raw_job = static_cast<FAsyncLoadJob*>(user);
    // 処理中カウントはジョブ引数と、その中の共有状態を解放し終えてから戻す。
    // 先に Done すると Shutdown 後の MemorySystem 停止とジョブ解放が競合する。
    FActiveOperationGuard operation(raw_job ? raw_job->active_operations : nullptr);
    TUniquePtr<FAsyncLoadJob> job(raw_job, raw_job ? raw_job->allocator : nullptr);
    if (!job) return;

    auto bytes_r = CFileSystem::ReadAllBytes(job->path->Path());
    if (bytes_r.IsErr()) {
        job->state->error = bytes_r.Error();
        job->state->has_error = true;
        job->registry->AsyncLoadFinished(job->id);
        job->state->counter.Done();
        return;
    }

    auto asset_r = job->loader->LoadFromBytes(job->id, bytes_r.Value());
    if (asset_r.IsErr()) {
        job->state->error = asset_r.Error();
        job->state->has_error = true;
        job->registry->AsyncLoadFinished(job->id);
        job->state->counter.Done();
        return;
    }

    TSharedPtr<AAsset> a = Move(asset_r.Value());
    if (!a.Get()) {  // ローダが OkInit で null を返した場合の null-deref 回避
        job->state->error = ACS_ERR(Asset, kAssetRegistrySubNullAsset,
                                    "loader returned null asset");
        job->state->has_error = true;
        job->registry->AsyncLoadFinished(job->id);
        job->state->counter.Done();
        return;
    }
    /** 同時loadのcache winnerと統合した公開結果。 */
    auto PublishResult = job->registry->TryPublishLoadedAsset(job->id, Move(a));
    if (PublishResult.IsErr()) {
        job->state->error = PublishResult.Error();
        job->state->has_error = true;
        job->registry->AsyncLoadFinished(job->id);
        job->state->counter.Done();
        return;
    }

    job->state->result = Move(PublishResult.Value());
    job->registry->AsyncLoadFinished(job->id);
    job->state->counter.Done();
    // job は TUniquePtr のスコープ抜けで自動 delete
}

TResult<void> CAssetRegistry::AsyncCacheInsert(FAssetId id, TSharedPtr<AAsset> a) noexcept {
    FScopedLock lk(m_Lock);
    if (m_Closing) {
        // Shutdown は既に受理した load の完了を待つ。結果は future に返し、
        // 解放中の cache へだけ挿入しない。
        return Ok();
    }
    if (!m_Cache.TryAdd(id, Move(a))) {
        return ACS_ERR(Memory, kAssetRegistrySubOutOfMemory,
                       "CAssetRegistry::AsyncCacheInsert: cache allocation failed");
    }
    return Ok();
}

void CAssetRegistry::AsyncLoadFinished(FAssetId id) noexcept {
    FScopedLock lk(m_Lock);
    m_InFlight.Remove(id);
}

FAssetFuture CAssetRegistry::LoadAsync(const wchar_t* path) noexcept {
    const u16 PathError = ValidateRegistryPath(path);
    if (PathError != 0) {
        return MakeCompletedAssetError(ACS_ERR(Asset, PathError, "LoadAsync: invalid path"));
    }

    const FAssetId id = MakeIdFromPath(path);

    // 進行中要求への合流は新しい共有状態を確保する前に完了する。
    {
        FScopedLock lk(m_Lock);
        if (!m_Closing) {
            const TSharedPtr<FAsyncLoadState>* pending = m_InFlight.Find(id);
            if (pending && pending->Get()) {
                ++m_AsyncRequestCount;
                ++m_AsyncCoalescedCount;
                return FAssetFuture(*pending);
            }
        }
    }

    /** 新規要求または即時完了結果が所有する共有状態。 */
    auto state = MakeShared<FAsyncLoadState>();
    if (!state.Get()) return FAssetFuture{};

    // 受付判定・キャッシュ確認・ローダ確保・処理中登録を同じロック区間で行う。
    IAssetLoader* loader = nullptr;
    {
        FScopedLock lk(m_Lock);
        if (m_Closing) {
            state->error = ACS_ERR(Asset, kAssetRegistrySubShuttingDown,
                                   "CAssetRegistry is shutting down");
            state->has_error = true;
            state->counter.Done();
            return FAssetFuture(Move(state));
        }
        ++m_AsyncRequestCount;

        const TSharedPtr<AAsset>* hit = m_Cache.Find(id);
        if (hit && hit->Get()) {
            state->result = *hit;
            ++m_CacheHitCount;
            state->counter.Done();
            return FAssetFuture(Move(state));
        }
        const TSharedPtr<FAsyncLoadState>* pending = m_InFlight.Find(id);
        if (pending && pending->Get()) {
            ++m_AsyncCoalescedCount;
            return FAssetFuture(*pending);
        }
        loader = FindLoader(path);
        if (loader) {
            if (!m_InFlight.TryAdd(id, state)) {
                state->error = ACS_ERR(Memory, kAssetRegistrySubOutOfMemory, "LoadAsync: in-flight allocation failed");
                state->has_error = true;
                state->counter.Done();
                return FAssetFuture(Move(state));
            }
            m_ActiveOperations.Add(1);
        }
    }
    if (!loader) {
        state->error = ACS_ERR(Asset, kAssetRegistrySubNoLoader, "LoadAsync: no loader");
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }

    // cache / in-flight hit ではワーカー用 path を保持しない。新規ジョブだけが
    // path の長さに合わせた共有領域を取得する。
    /** intern 対象 path の NUL を除く長さ。 */
    usize PathLength = 0u;
    while (path[PathLength] != L'\0') ++PathLength;
    /** 共有 path の取得結果。 */
    auto InternResult = m_PathInterner.Intern(path, PathLength);
    if (InternResult.IsErr()) {
        AsyncLoadFinished(id);
        m_ActiveOperations.Done();
        state->error = InternResult.Error();
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }
    /** 非同期 job が完了まで所有する共有 path。 */
    TSharedPtr<FInternedAssetPath> InternedPath = Move(InternResult.Value());

    // ジョブを heap 確保し CThreadPool に投入
    auto job = MakeUnique<FAsyncLoadJob>();
    if (!job) {
        AsyncLoadFinished(id);
        m_ActiveOperations.Done();
        state->error = ACS_ERR(Memory, kAssetRegistrySubOutOfMemory, "LoadAsync: alloc");
        state->has_error = true;
        state->counter.Done();
        return FAssetFuture(Move(state));
    }
    job->allocator = job.GetAllocator();
    job->registry = this;
    job->state    = state;
    job->active_operations = &m_ActiveOperations;
    job->loader   = loader;
    job->id       = id;
    job->path     = Move(InternedPath);
    {
        /** job 投入診断値を保護する lock。 */
        FScopedLock lk(m_Lock);
        ++m_AsyncJobCount;
        ++m_PhysicalFileReadCount;
    }

    FTask t{};
    t.fn      = &CAssetRegistry::AsyncLoadWorker;
    t.user    = job.Get();
    t.counter = nullptr;     // 完了通知は state->counter 側で行う
    auto sub = CThreadPool::Submit(t);
    if (sub.IsErr()) {
        // 投入失敗時は同期的に実行
        AsyncLoadWorker(job.Release(), 0);
        return FAssetFuture(Move(state));
    }
    job.Release();   // 所有権を worker に渡した
    return FAssetFuture(Move(state));
}

TSharedPtr<AAsset> CAssetRegistry::Find(FAssetId id) noexcept {
    FScopedLock lk(m_Lock);
    const TSharedPtr<AAsset>* hit = m_Cache.Find(id);
    return (hit && hit->Get()) ? *hit : TSharedPtr<AAsset>();
}

void CAssetRegistry::Unload(FAssetId id) noexcept {
    FScopedLock lk(m_Lock);
    m_Cache.Remove(id);
}

void CAssetRegistry::Clear() noexcept {
    FScopedLock lk(m_Lock);
    m_Cache.Reset();
}

FAssetRegistryDiagnostics CAssetRegistry::Diagnostics() const noexcept
{
    /** 呼び出し元へ返す診断 snapshot。 */
    FAssetRegistryDiagnostics Result{};
    {
        /** registry 内診断値の同時取得を守る lock。 */
        FScopedLock Lock(m_Lock);
        Result.async_request_count = m_AsyncRequestCount;
        Result.async_coalesced_count = m_AsyncCoalescedCount;
        Result.async_job_count = m_AsyncJobCount;
        Result.physical_file_read_count = m_PhysicalFileReadCount;
        Result.cache_hit_count = m_CacheHitCount;
    }
    Result.path_interner = m_PathInterner.Diagnostics();
    return Result;
}

void CAssetRegistry::Shutdown() noexcept
{
    {
        FScopedLock lk(m_Lock);
        if (m_ShutdownComplete) return;
        m_Closing = true;
    }

    // ThreadPool が生きていれば待機側もキュー排出を手伝う。既に停止済みの場合でも、
    // 同期 Load が別スレッドで戻るまで待てるよう短時間 sleep で確認を続ける。
    while (!m_ActiveOperations.Finished()) {
        if (CThreadPool::WorkerCount() != 0)
            CThreadPool::Wait(m_ActiveOperations);
        else
            SleepMs(1);
    }

    FScopedLock lk(m_Lock);
    if (m_ShutdownComplete) return;
    // 確保元を維持したまま容量を返す。再起動後も同じアロケータを使用する。
    m_Cache.Empty();
    m_InFlight.Empty();
    m_Loaders.Empty();
    m_PathInterner.Reset();
    m_ShutdownComplete = true;
}

void CAssetRegistry::Restart() noexcept
{
    Shutdown();

    FScopedLock lk(m_Lock);
    m_Closing = false;
    m_ShutdownComplete = false;
    m_AsyncRequestCount = 0u;
    m_AsyncCoalescedCount = 0u;
    m_AsyncJobCount = 0u;
    m_PhysicalFileReadCount = 0u;
    m_CacheHitCount = 0u;
}

namespace {
/** 画像ローダ実体 (プロセス寿命)。 */
CImageAssetLoader  g_image_loader;

/** WAV 音声ローダ実体。 */
CWavAssetLoader    g_wav_loader;

/** MP3 音声ローダ実体。 */
CMp3AssetLoader    g_mp3_loader;

/** FLAC 音声ローダ実体。 */
CFlacAssetLoader   g_flac_loader;

/** OGG Vorbis 音声ローダ実体。 */
COggAssetLoader    g_ogg_loader;

/** glTF メッシュローダ実体。 */
CGltfAssetLoader   g_gltf_loader;

/** GLB メッシュローダ実体。 */
CGlbAssetLoader    g_glb_loader;

/** Wavefront OBJ メッシュローダ実体。 */
CObjAssetLoader    g_obj_loader;

/** FBX メッシュローダ実体。 */
CFbxAssetLoader    g_fbx_loader;

/** テキストローダ実体。 */
CTextAssetLoader   g_text_loader;

/** バイナリフォールバックローダ実体。 */
CBinaryAssetLoader g_binary_loader;
/** .cine bytesをACinematicAssetへ変換するローダ実体です。 */
asset::CCinematicAssetLoader g_cinematic_loader;

/**
 * 拡張子別名のラッパローダ。
 *
 * @details 同じローダ実体を別の拡張子でも登録できるよう、TypeId/LoadFromBytes を委譲し Extension だけ差し替える。
 */
class FAliasLoader final : public IAssetLoader {
public:
    /**
     * 委譲先ローダと別名拡張子を指定して構築する。
     *
     * @param base 実処理を委譲するローダ実体。
     * @param ext このラッパが担当する拡張子。
     */
    FAliasLoader(IAssetLoader* base, const char* ext) noexcept : m_Base(base), m_Ext(ext) {}

    /**
     * 委譲先のアセット型 ID を返す。
     *
     * @return base->TypeId()。
     */
    AssetType   TypeId()    const noexcept override { return m_Base->TypeId(); }

    /**
     * この別名の拡張子を返す。
     *
     * @return 構築時に渡した拡張子。
     */
    const char* Extension() const noexcept override { return m_Ext; }

    /**
     * 委譲先ローダでバイト列からアセットを生成する。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes ファイル全体のバイト列。
     * @return base->LoadFromBytes() の結果。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override {
        return m_Base->LoadFromBytes(id, bytes);
    }
private:
    /** 実処理を委譲するローダ実体。 */
    IAssetLoader* m_Base;

    /** このラッパが担当する拡張子。 */
    const char*   m_Ext;
};

/** 画像ローダの .jpg 別名。 */
FAliasLoader g_image_jpg { &g_image_loader, "jpg"  };

/** 画像ローダの .jpeg 別名。 */
FAliasLoader g_image_jpeg{ &g_image_loader, "jpeg" };

/** 画像ローダの .bmp 別名。 */
FAliasLoader g_image_bmp { &g_image_loader, "bmp"  };

/** 画像ローダの .tga 別名。 */
FAliasLoader g_image_tga { &g_image_loader, "tga"  };

/** 画像ローダの .gif 別名。 */
FAliasLoader g_image_gif { &g_image_loader, "gif"  };

/** 画像ローダの .hdr 別名 (HDR 画像)。 */
FAliasLoader g_image_hdr { &g_image_loader, "hdr"  };

/** 画像ローダの .pic 別名。 */
FAliasLoader g_image_pic { &g_image_loader, "pic"  };

/** 画像ローダの .pnm 別名。 */
FAliasLoader g_image_pnm { &g_image_loader, "pnm"  };

/** 画像ローダの .ppm 別名。 */
FAliasLoader g_image_ppm { &g_image_loader, "ppm"  };

/** 画像ローダの .pgm 別名。 */
FAliasLoader g_image_pgm { &g_image_loader, "pgm"  };

/** 画像ローダの .psd 別名。 */
FAliasLoader g_image_psd { &g_image_loader, "psd"  };

/** OGG ローダの .oga 別名 (oga は ogg の別名)。 */
FAliasLoader g_ogg_oga { &g_ogg_loader, "oga" };

/** テキストローダの .json 別名。 */
FAliasLoader g_text_json { &g_text_loader, "json" };

/** テキストローダの .xml 別名。 */
FAliasLoader g_text_xml  { &g_text_loader, "xml"  };

/** テキストローダの .yaml 別名。 */
FAliasLoader g_text_yaml { &g_text_loader, "yaml" };

/** テキストローダの .yml 別名。 */
FAliasLoader g_text_yml  { &g_text_loader, "yml"  };

/** テキストローダの .toml 別名。 */
FAliasLoader g_text_toml { &g_text_loader, "toml" };

/** テキストローダの .ini 別名。 */
FAliasLoader g_text_ini  { &g_text_loader, "ini"  };

/** テキストローダの .csv 別名。 */
FAliasLoader g_text_csv  { &g_text_loader, "csv"  };

/** テキストローダの .md 別名。 */
FAliasLoader g_text_md   { &g_text_loader, "md"   };

/** テキストローダの .log 別名。 */
FAliasLoader g_text_log  { &g_text_loader, "log"  };

/** テキストローダの .hlsl 別名。 */
FAliasLoader g_text_hlsl { &g_text_loader, "hlsl" };

/** テキストローダの .glsl 別名。 */
FAliasLoader g_text_glsl { &g_text_loader, "glsl" };

/** テキストローダの .vert 別名。 */
FAliasLoader g_text_vert { &g_text_loader, "vert" };

/** テキストローダの .frag 別名。 */
FAliasLoader g_text_frag { &g_text_loader, "frag" };

/** テキストローダの .lua 別名。 */
FAliasLoader g_text_lua  { &g_text_loader, "lua"  };

/** テキストローダの .py 別名。 */
FAliasLoader g_text_py   { &g_text_loader, "py"   };
} // namespace

TResult<void> CAssetRegistry::TryRegisterDefaultLoaders() noexcept
{
    /** 既存の登録順とfallbackの最終位置を保つ標準loader一覧。 */
    IAssetLoader* const DefaultLoaders[] = {&g_image_loader, &g_image_jpg, &g_image_jpeg, &g_image_bmp, &g_image_tga, &g_image_gif, &g_image_hdr, &g_image_pic, &g_image_pnm, &g_image_ppm, &g_image_pgm, &g_image_psd, &g_wav_loader, &g_mp3_loader, &g_flac_loader, &g_ogg_loader, &g_ogg_oga, &g_gltf_loader, &g_glb_loader, &g_obj_loader, &g_fbx_loader, &g_text_loader, &g_text_json, &g_text_xml, &g_text_yaml, &g_text_yml, &g_text_toml, &g_text_ini, &g_text_csv, &g_text_md, &g_text_log, &g_text_hlsl, &g_text_glsl, &g_text_vert, &g_text_frag, &g_text_lua, &g_text_py, &g_cinematic_loader, &g_binary_loader,};

    FScopedLock lock(m_Lock);
    if (m_Closing) {
        return ACS_ERR(Asset, kAssetRegistrySubShuttingDown, "CAssetRegistry::TryRegisterDefaultLoaders: registry is shutting down");
    }

    /** まだ登録されていない標準loader数。 */
    usize MissingCount = 0u;
    for (IAssetLoader* const DefaultLoader : DefaultLoaders) {
        const char* const Extension = DefaultLoader->Extension();
        if (!IsValidLoaderExtension(Extension)) {
            return ACS_ERR(Asset, kAssetRegistrySubInvalidExtension, "CAssetRegistry::TryRegisterDefaultLoaders: invalid extension");
        }

        /** 同じ標準loaderが既に登録済みならidempotent successとして扱う。 */
        bool AlreadyRegistered = false;
        for (usize Index = 0u; Index < m_Loaders.Num(); ++Index) {
            if (!StrEqAscii(m_Loaders[Index]->Extension(), Extension)) continue;
            if (m_Loaders[Index] != DefaultLoader) {
                return ACS_ERR(Asset, kAssetRegistrySubDuplicateLoader, "CAssetRegistry::TryRegisterDefaultLoaders: duplicate extension");
            }
            AlreadyRegistered = true;
            break;
        }
        if (!AlreadyRegistered) ++MissingCount;
    }

    if (MissingCount > TNumLimits<usize>::Max() - m_Loaders.Num() || !m_Loaders.TryReserve(m_Loaders.Num() + MissingCount)) {
        return ACS_ERR(Memory, kAssetRegistrySubOutOfMemory, "CAssetRegistry::TryRegisterDefaultLoaders: allocation failed");
    }

    /** 予期しない追加失敗時に戻す登録済みloader数。 */
    const usize OriginalCount = m_Loaders.Num();
    for (IAssetLoader* const DefaultLoader : DefaultLoaders) {
        /** preflightで同一pointerを確認済みの登録は追加しない。 */
        bool AlreadyRegistered = false;
        for (usize Index = 0u; Index < OriginalCount; ++Index) {
            if (m_Loaders[Index] == DefaultLoader) {
                AlreadyRegistered = true;
                break;
            }
        }
        if (!AlreadyRegistered && !m_Loaders.TryAdd(DefaultLoader)) {
            (void)m_Loaders.TrySetNum(OriginalCount);
            return ACS_ERR(Memory, kAssetRegistrySubOutOfMemory, "CAssetRegistry::TryRegisterDefaultLoaders: append failed");
        }
    }
    return Ok();
}

void CAssetRegistry::RegisterDefaultLoaders() noexcept
{
    (void)TryRegisterDefaultLoaders();
}

} // namespace acs
