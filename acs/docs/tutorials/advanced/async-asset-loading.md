# 非同期アセット読み込み

[`CAssetRegistry`](../../reference/symbols/asset/cassetregistry-b3795924.html) の [`LoadAsync`](../../reference/symbols/asset/cassetregistry-b3795924/loadasync-a249c823.html) は、ファイル読み込みとローダー処理の完了状態を [`FAssetFuture`](../../reference/symbols/asset/fassetfuture-8ae23d9a.html) として返します。通常はスレッドプールのワーカースレッドが処理し、更新側は [`IsReady`](../../reference/symbols/asset/fassetfuture-8ae23d9a/isready-1f32eeb2.html) を使って待機せずに確認できます。

`CApplication` は `OnStart` より前に標準ローダーを登録します。画像、音声、glTF、GLB、OBJ、FBX、テキスト、バイナリは `GetAssets()` から要求できます。

## フレームを止めずに結果を受け取る

次の例はメッシュの読み込み要求を `OnStart` で発行し、完了したフレームだけ結果を取り出します。

```cpp
#include "app/Application.h"
#include "asset/AssetFuture.h"
#include "asset/MeshAsset.h"
#include "foundation/Log.h"
#include "memory/SharedPtr.h"

class CAsyncMeshApp final : public acs::CApplication
{
protected:
    void OnStart() noexcept override
    {
        m_Future = GetAssets().LoadAsync(L"assets/terrain.glb");
        m_Pending = m_Future.Valid();
        if (!m_Pending)
            ACS_LOG_ERROR("非同期 Asset 読み込みの状態を確保できません");
    }

    void OnUpdate(acs::f32) noexcept override
    {
        if (!m_Pending || !m_Future.IsReady())
            return;

        m_Pending = false;
        auto loaded = m_Future.Get();
        if (loaded.IsErr())
        {
            ACS_LOG_ERROR("terrain.glb を読み込めません: %s", loaded.Error().message);
            m_Future = acs::FAssetFuture{};
            return;
        }

        if (!loaded.Value() || loaded.Value()->Type() != acs::AMeshAsset::StaticType())
        {
            ACS_LOG_ERROR("terrain.glb は AMeshAsset ではありません");
            m_Future = acs::FAssetFuture{};
            return;
        }

        m_Asset = loaded.Value();
        m_Future = acs::FAssetFuture{};
    }

private:
    const acs::AMeshAsset* Mesh() const noexcept
    {
        if (!m_Asset || m_Asset->Type() != acs::AMeshAsset::StaticType())
            return nullptr;
        return static_cast<const acs::AMeshAsset*>(m_Asset.Get());
    }

    acs::FAssetFuture m_Future;
    acs::TSharedPtr<acs::AAsset> m_Asset;
    bool m_Pending = false;
};
```

[`Get`](../../reference/symbols/asset/fassetfuture-8ae23d9a/get-7b1acfa7.html) は完了まで待つ関数です。上の例では `IsReady()` が `true` の場合だけ呼ぶため、更新スレッドを待機させません。成功値は `TSharedPtr<AAsset>` です。型を確認せずに派生型へ変換せず、[`AAsset::Type`](../../reference/symbols/asset/aasset-06291e0c/type-10ad0fae.html) と対象型の `StaticType()` を比較します。

`m_Asset` は共有所有者です。[`CAssetRegistry::Unload`](../../reference/symbols/asset/cassetregistry-b3795924/unload-34bf5f79.html) や `Clear` でキャッシュから外した後も、`m_Asset` が保持している間はアセットが生存します。`Mesh()` が返すポインターは非所有なので、使用中は `m_Asset` を解放しません。

## 完了待ちが必要な場合

[`Wait`](../../reference/symbols/asset/fassetfuture-8ae23d9a/wait-c63602e6.html) は `Get()` の別名です。完了前に呼ぶと呼び出し元を待機させます。ワーカースレッドから呼んだ場合は待機中に別のタスクの処理へ参加します。

```cpp
auto loaded = future.Wait();
if (loaded.IsErr())
{
    ACS_LOG_ERROR("Asset を読み込めません: %s", loaded.Error().message);
    return;
}
acs::TSharedPtr<acs::AAsset> asset = loaded.Value();
```

起動時に必須の小さなアセットを揃える用途では `Wait()` を使用できます。通常のフレーム更新では `IsReady()` で確認し、完了前に `Get()` または `Wait()` を呼びません。

## 直ちに完了する場合

次の条件では、返された非同期結果が最初から完了済みです。

- 同じ正規化パスのアセットがキャッシュにある。
- パス、ローダー、終了状態、同時処理数の検証で失敗した。
- ワーカースレッドへタスクを投入できず、`LoadAsync` 内で同期処理を完了した。

最後の条件では `LoadAsync` 自体がファイル読み込みの完了まで戻らない場合があります。呼び出しを常に一定時間内へ収める API ではありません。

## 失敗条件

- 非同期結果の共有状態を確保できない場合、[`Valid`](../../reference/symbols/asset/fassetfuture-8ae23d9a/valid-b85fdafa.html) は `false` です。この状態で `Get()` を呼ぶとエラーを返します。
- null、空、正規化できないパス、または1023コード単位を超えるパスは完了済みエラーになります。
- 対応ローダーがない、レジストリが終了処理中、同時処理数が上限に達した場合も完了済みエラーになります。
- ファイル読み込み、アセットの生成、キャッシュの確保に失敗した場合は、完了後の `TResult` がエラーになります。
- `FAssetFuture` に取消APIはありません。不要になった非同期結果を解放しても、受理済みの読み込みはレジストリの管理下で完了します。

`CAssetRegistry` は破棄または [`Shutdown`](../../reference/symbols/asset/cassetregistry-b3795924/shutdown-7ed595cf.html) の際に実行中の読み込みを待ちます。独自ローダーを登録した場合、そのローダーは `Shutdown` 完了まで生存させます。
