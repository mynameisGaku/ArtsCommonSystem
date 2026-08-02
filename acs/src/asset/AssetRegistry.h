// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"
#include "container/HashMap.h"
#include "threading/Mutex.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "asset/AssetFuture.h"
#include "asset/AssetPathInterner.h"
#include "asset/AssetRegistryDiagnostics.h"

namespace acs {

/** Registry が同期/非同期で完全に所有できる OS path の最大 code unit 数。 */
inline constexpr usize kAssetRegistryMaxPathLength = 1023u;

inline constexpr u16 kAssetRegistrySubNullPath          = 1u;
inline constexpr u16 kAssetRegistrySubNoLoader          = 2u;
inline constexpr u16 kAssetRegistrySubNullAsset         = 3u;
inline constexpr u16 kAssetRegistrySubInvalidPath       = 4u;
inline constexpr u16 kAssetRegistrySubPathTooLong       = 5u;
inline constexpr u16 kAssetRegistrySubInvalidLoader     = 6u;
inline constexpr u16 kAssetRegistrySubInvalidExtension  = 7u;
inline constexpr u16 kAssetRegistrySubDuplicateLoader   = 8u;
inline constexpr u16 kAssetRegistrySubOutOfMemory       = 9u;
inline constexpr u16 kAssetRegistrySubShuttingDown      = 13u;

/**
 * パスからアセットをロードして共有保持するレジストリ。
 *
 * @details
 * 登録済みローダを拡張子でマッチして同期/非同期ロードする。ロード済みアセットは
 * FAssetId をキーに TSharedPtr でキャッシュされ、同じパスの再ロードは同一インスタンスを返す。
 * FMutex でガードされ、非同期ワーカーからのキャッシュ挿入も安全。non-copy 型。
 */
class CAssetRegistry {
public:
    /** 空のレジストリを構築する (ローダ未登録)。 */
    CAssetRegistry() noexcept = default;

    /**
     * 指定アロケータを使う空のレジストリを構築する。
     *
     * @param allocator キャッシュ・ローダ配列のストレージ確保元。
     */
    explicit CAssetRegistry(IAllocator& allocator) noexcept
        : m_Cache(allocator), m_InFlight(allocator), m_Loaders(allocator), m_PathInterner(allocator)
    {
    }

    /** 実行中の同期・非同期ロードを待ってからキャッシュを解放する。 */
    ~CAssetRegistry() noexcept;

    /** コピー禁止 (FMutex とキャッシュを単独保持するため)。 */
    CAssetRegistry(const CAssetRegistry&) = delete;

    /** コピー代入も禁止。 */
    CAssetRegistry& operator=(const CAssetRegistry&) = delete;

    /**
     * ローダを登録する。
     *
     * @details 拡張子マッチで使われる。所有権はレジストリに渡らない。登録したローダは
     * レジストリの Shutdown 完了まで呼び出し側が生存させること。Shutdown 開始後は無視される。
     * @param loader 登録するローダ (null は無視)。
     */
    void RegisterLoader(IAssetLoader* loader) noexcept;

    /**
     * loader contract を検証して登録する checked API。
     * null、不正 extension、同一 extension、OOM、shutdown を分類して返す。
     */
    TResult<void> TryRegisterLoader(IAssetLoader* loader) noexcept;

    /** 標準ローダ群を一括登録する (Image / Audio / Mesh / Text / Binary)。 */
    void RegisterDefaultLoaders() noexcept;

    /**
     * パスからアセットを同期ロードする。
     *
     * @details キャッシュ済みなら即返却、未キャッシュならファイル読み込み + ローダ呼び出し後にキャッシュする。
     * @param path ロードするファイルのパス。
     * @return 成功ならアセット、失敗 (null path / ローダ無し / 読み込み失敗) ならエラー。
     */
    TResult<TSharedPtr<AAsset>> Load(const wchar_t* path) noexcept;

    /**
     * パスからアセットを非同期ロードする。
     *
     * @details CThreadPool ワーカーで実行し、完了は FAssetFuture で確認する。
     * キャッシュ済みなら即完了状態の future を返す。
     * @param path ロードするファイルのパス。
     * @return 完了確認用の FAssetFuture。
     */
    FAssetFuture LoadAsync(const wchar_t* path) noexcept;

    /** ロード共有とパス保持の累積診断値を一括取得する。 */
    FAssetRegistryDiagnostics Diagnostics() const noexcept;

    /**
     * 新規ロード受付を閉じ、処理中の同期・非同期ロードを待って全キャッシュを解放する。
     *
     * @details 冪等。終了開始後の Load/LoadAsync はエラーとなる。ロードコールバック自身から
     * 呼ぶと自己待機になるため、レジストリを所有するスレッドから呼ぶこと。
     */
    void Shutdown() noexcept;

    /**
     * 現在のロードを終了してストレージを解放し、新規ロード受付を再開する。
     *
     * @details Shutdown 済みでも呼べる。構築時のアロケータは維持される。
     * Shutdown と同じく、ロードコールバック自身から呼んではならない。
     */
    void Restart() noexcept;

    /**
     * キャッシュからのみアセットを取得する (ロードはしない)。
     *
     * @param id 探すアセット ID。
     * @return キャッシュにあればアセット、無ければ空の TSharedPtr。
     */
    TSharedPtr<AAsset> Find(FAssetId id) noexcept;

    /**
     * 指定アセットをキャッシュから外す (ファイル変更時の再読み込み用)。
     *
     * @param id キャッシュから外すアセット ID。
     */
    void Unload(FAssetId id) noexcept;

    /** 全キャッシュをクリアする。 */
    void Clear() noexcept;

    /**
     * ワーカースレッドからキャッシュへロック付きで挿入する (内部 API)。
     *
     * @param id 挿入するアセット ID。
     * @param a 挿入するアセット。
     */
    TResult<void> AsyncCacheInsert(FAssetId id, TSharedPtr<AAsset> a) noexcept;

    /** 完了した非同期要求を処理中テーブルから外す。 */
    void AsyncLoadFinished(FAssetId id) noexcept;

private:
    /**
     * 拡張子から適切なローダを選ぶ。
     *
     * @param path 拡張子を取り出す対象のパス。
     * @return マッチしたローダ、無ければ "*" のフォールバックローダ (それも無ければ nullptr)。
     */
    IAssetLoader* FindLoader(const wchar_t* path) noexcept;

    /** キャッシュ・ローダ配列を保護する FMutex。 */
    mutable FMutex                  m_Lock;

    /** ID をキーにしたアセットキャッシュ。 */
    THashMap<FAssetId, TSharedPtr<AAsset>>    m_Cache;

    /**
     * 同一 ID の未完了ロードが共有する状態。
     *
     * @details 同じパスへの同時 LoadAsync はこの状態を再利用し、ファイル入出力・
     * ローダー呼び出し・ジョブ確保を 1 回へ集約する。成功・失敗の完了時に必ず除去する。
     */
    THashMap<FAssetId, TSharedPtr<FAsyncLoadState>> m_InFlight;

    /** 登録済みローダ (非所有ポインタ)。 */
    TArray<IAssetLoader*>           m_Loaders;

    /** 非同期ジョブと再要求で共有する有界パス所有プール。 */
    CAssetPathInterner m_PathInterner;

    /** 有効な非同期要求数。 */
    u64 m_AsyncRequestCount = 0u;

    /** 進行中ジョブへ合流した非同期要求数。 */
    u64 m_AsyncCoalescedCount = 0u;

    /** 実際に投入した非同期ジョブ数。 */
    u64 m_AsyncJobCount = 0u;

    /** 実際に開始したファイル読込数。 */
    u64 m_PhysicalFileReadCount = 0u;

    /** キャッシュから返した要求数。 */
    u64 m_CacheHitCount = 0u;

    /** Shutdown が新規ロード受付を閉じた後は true (m_Lock で保護)。 */
    bool m_Closing = false;

    /** Shutdown が待機と全ストレージ解放まで完了した後は true (m_Lock で保護)。 */
    bool m_ShutdownComplete = false;

    /** ロック外でレジストリまたは登録ローダを参照しているロード処理数。 */
    FCompletionCounter m_ActiveOperations;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAssetRegistry = CAssetRegistry;

} // namespace acs
