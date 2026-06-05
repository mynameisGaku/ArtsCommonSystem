// SPDX-License-Identifier: Apache-2.0
// アセットレジストリ（パスからロードして共有保持する）
//
// 使い方:
//   FAssetRegistry reg;
//   reg.RegisterLoader(MakeShared<FBinaryAssetLoader>().Get());
//
//   auto r = reg.Load(L"data/save.bin");
//   if (r.IsOk()) {
//       TSharedPtr<Asset> a = r.Value();
//       // a を保持し続ければレジストリ内部でもキャッシュされる
//   }
//
//   // 同じパスで再度 Load すれば同じ TSharedPtr を返す（キャッシュヒット）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"
#include "container/HashMap.h"
#include "threading/Mutex.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "asset/AssetFuture.h"

namespace acs {

/**
 * パスからアセットをロードして共有保持するレジストリ。
 *
 * @details
 * 登録済みローダを拡張子でマッチして同期/非同期ロードする。ロード済みアセットは
 * FAssetId をキーに TSharedPtr でキャッシュされ、同じパスの再ロードは同一インスタンスを返す。
 * FMutex でガードされ、非同期ワーカーからのキャッシュ挿入も安全。non-copy 型。
 */
class FAssetRegistry {
public:
    /** 空のレジストリを構築する (ローダ未登録)。 */
    FAssetRegistry() noexcept = default;

    /** レジストリを破棄する (キャッシュは TSharedPtr が解放、ローダは非所有)。 */
    ~FAssetRegistry() noexcept = default;

    /** コピー禁止 (Mutex とキャッシュを単独保持するため)。 */
    FAssetRegistry(const FAssetRegistry&) = delete;

    /** コピー代入も禁止。 */
    FAssetRegistry& operator=(const FAssetRegistry&) = delete;

    /**
     * ローダを登録する。
     *
     * @details 拡張子マッチで使われる。所有権はレジストリに渡らない (呼び出し側が寿命を管理)。
     * @param loader 登録するローダ (null は無視)。
     */
    void RegisterLoader(IAssetLoader* loader) noexcept;

    /** 標準ローダ群を一括登録する (Image / Audio / Mesh / Text / Binary)。 */
    void RegisterDefaultLoaders() noexcept;

    /**
     * パスからアセットを同期ロードする。
     *
     * @details キャッシュ済みなら即返却、未キャッシュならファイル読み込み + ローダ呼び出し後にキャッシュする。
     * @param path ロードするファイルのパス。
     * @return 成功ならアセット、失敗 (null path / ローダ無し / 読み込み失敗) ならエラー。
     */
    TResult<TSharedPtr<Asset>> Load(const wchar_t* path) noexcept;

    /**
     * パスからアセットを非同期ロードする。
     *
     * @details FThreadPool ワーカーで実行し、完了は FAssetFuture で確認する。
     * キャッシュ済みなら即完了状態の future を返す。
     * @param path ロードするファイルのパス。
     * @return 完了確認用の FAssetFuture。
     */
    FAssetFuture LoadAsync(const wchar_t* path) noexcept;

    /**
     * キャッシュからのみアセットを取得する (ロードはしない)。
     *
     * @param id 探すアセット ID。
     * @return キャッシュにあればアセット、無ければ空の TSharedPtr。
     */
    TSharedPtr<Asset> Find(FAssetId id) noexcept;

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
    void AsyncCacheInsert(FAssetId id, TSharedPtr<Asset> a) noexcept;

private:
    /**
     * 拡張子から適切なローダを選ぶ。
     *
     * @param path 拡張子を取り出す対象のパス。
     * @return マッチしたローダ、無ければ "*" のフォールバックローダ (それも無ければ nullptr)。
     */
    IAssetLoader* FindLoader(const wchar_t* path) noexcept;

    /** キャッシュ・ローダ配列を保護する Mutex。 */
    FMutex                          m_Lock;

    /** ID をキーにしたアセットキャッシュ。 */
    THashMap<FAssetId, TSharedPtr<Asset>>    m_Cache;

    /** 登録済みローダ (非所有ポインタ)。 */
    TArray<IAssetLoader*>           m_Loaders;
};

} // namespace acs
