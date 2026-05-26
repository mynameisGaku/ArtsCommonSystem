// SPDX-License-Identifier: Apache-2.0
// 非同期アセットロードの結果ハンドル
//
// 使い方:
//   AssetFuture fut = registry.LoadAsync(L"big_mesh.glb");
//   ...
//   if (fut.IsReady()) {
//       auto r = fut.Get();
//       if (r.IsOk()) TRc<Asset> a = r.Value();
//   }
//   // または同期待ち（ワーカースチール参加付き）
//   auto r = fut.Wait();
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "memory/Rc.h"
#include "asset/Asset.h"
#include "threading/ThreadPool.h"

namespace acs {

// 内部共有状態（worker と future の双方が保持）
struct AsyncLoadState {
    CompletionCounter counter{1};   // 1 → 0 で完了
    TRc<Asset>         result;
    FErrorCode         error{};
    bool              has_error = false;
};

class AssetFuture {
public:
    AssetFuture() noexcept = default;

    // 内部用: AssetRegistry::LoadAsync が状態を渡してくる
    explicit AssetFuture(TRc<AsyncLoadState> s) noexcept : _state(Move(s)) {}

    // 完了済みか（ノンブロッキング、true ならGet がすぐ返る）
    bool IsReady() const noexcept {
        return _state.Get() && _state->counter.Finished();
    }

    // 有効なフューチャか（失敗構築時に false）
    bool Valid() const noexcept { return _state.Get() != nullptr; }

    // 完了を待って結果を取り出す（呼び出しスレッドがワーカーなら手伝う）
    TResult<TRc<Asset>> Get() noexcept {
        if (!_state.Get()) return ACS_ERR(Asset, 10, "AssetFuture: empty");
        ThreadPool::Wait(_state->counter);
        if (_state->has_error) return TResult<TRc<Asset>>(_state->error);
        return TResult<TRc<Asset>>(OkInit, _state->result);
    }

    // 別名: Wait() = Get() （直感的に使えるよう）
    TResult<TRc<Asset>> Wait() noexcept { return Get(); }

private:
    TRc<AsyncLoadState> _state;
};

} // namespace acs
