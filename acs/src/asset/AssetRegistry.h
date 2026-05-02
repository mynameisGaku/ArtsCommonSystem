// アセットレジストリ（パスからロードして共有保持する）
//
// 使い方:
//   AssetRegistry reg;
//   reg.RegisterLoader(MakeRc<BinaryAssetLoader>().Get());
//
//   auto r = reg.Load(L"data/save.bin");
//   if (r.IsOk()) {
//       Rc<Asset> a = r.Value();
//       // a を保持し続ければレジストリ内部でもキャッシュされる
//   }
//
//   // 同じパスで再度 Load すれば同じ Rc を返す（キャッシュヒット）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Rc.h"
#include "container/HashMap.h"
#include "threading/Mutex.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"

namespace acs {

class AssetRegistry {
public:
    AssetRegistry() noexcept = default;
    ~AssetRegistry() noexcept = default;

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // ローダを登録（拡張子マッチで使われる、所有権はレジストリ側に渡らない）
    void RegisterLoader(IAssetLoader* loader) noexcept;

    // 同期ロード（ファイル読み込み + ローダ呼び出し、キャッシュ済みなら即返却）
    Result<Rc<Asset>> Load(const wchar_t* path) noexcept;

    // キャッシュからのみ取得（ロードはしない、未キャッシュなら nullptr Rc）
    Rc<Asset> Find(AssetId id) noexcept;

    // キャッシュから外す（ファイル変更時の再読み込み用）
    void Unload(AssetId id) noexcept;

    // 全キャッシュをクリア
    void Clear() noexcept;

private:
    // 拡張子から適切なローダを選ぶ（マッチなしならフォールバック "*" を返す）
    IAssetLoader* FindLoader(const wchar_t* path) noexcept;

    Mutex                          _lock;
    HashMap<AssetId, Rc<Asset>>    _cache;
    Array<IAssetLoader*>           _loaders;
};

} // namespace acs
