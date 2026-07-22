// SPDX-License-Identifier: Apache-2.0
// 非同期アセットロードの結果ハンドル
//
// 使い方:
//   FAssetFuture fut = registry.LoadAsync(L"big_mesh.glb");
//   ...
//   if (fut.IsReady()) {
//       auto r = fut.Get();
//       if (r.IsOk()) TSharedPtr<FAsset> a = r.Value();
//   }
//   // または同期待ち（ワーカースチール参加付き）
//   auto r = fut.Wait();
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "memory/SharedPtr.h"
#include "asset/Asset.h"
#include "threading/ThreadPool.h"

namespace acs {

/**
 * 非同期ロードの内部共有状態 (worker と future の双方が保持する)。
 *
 * @details
 * ワーカースレッドが結果かエラーを書き込み counter を Done() する。future 側は
 * counter の完了でそれを読み取る。TSharedPtr で共有されるため寿命は両者で管理される。
 */
struct FAsyncLoadState {
    /** 残ジョブ数カウンタ (1 → 0 で完了)。 */
    FCompletionCounter counter{1};

    /** ロード成功時のアセット (共有所有)。 */
    TSharedPtr<FAsset>         result;

    /** ロード失敗時のエラーコード (has_error が true のとき有効)。 */
    FErrorCode         error{};

    /** エラーが発生したら true。 */
    bool              has_error = false;
};

/**
 * 非同期アセットロードの結果ハンドル。
 *
 * @details
 * FAssetRegistry::LoadAsync が返す。IsReady() でノンブロッキングに完了確認し、
 * Get()/Wait() で完了を待って結果を取り出す。内部共有状態 (FAsyncLoadState) を
 * TSharedPtr で保持し、ワーカーと結果領域を共有する。
 */
class FAssetFuture {
public:
    /** 空の (無効な) future を構築する。 */
    FAssetFuture() noexcept = default;

    /**
     * 共有状態から future を構築する (FAssetRegistry::LoadAsync 用、内部用)。
     *
     * @param s ワーカーと共有する非同期ロード状態。
     */
    explicit FAssetFuture(TSharedPtr<FAsyncLoadState> s) noexcept : _state(Move(s)) {}

    /**
     * 完了済みかをノンブロッキングで返す。
     *
     * @return 完了していれば true (true なら Get() が即返る)。
     */
    bool IsReady() const noexcept {
        return _state.Get() && _state->counter.Finished();
    }

    /**
     * 有効な future かを返す。
     *
     * @return 共有状態を持っていれば true (失敗構築時は false)。
     */
    bool Valid() const noexcept { return _state.Get() != nullptr; }

    /**
     * 完了を待って結果を取り出す。
     *
     * @details 呼び出しスレッドがワーカーの場合はワーカースチールに参加して手伝う。
     * @return 成功ならロード済みアセット、失敗ならエラー。空 future ならエラー。
     */
    TResult<TSharedPtr<FAsset>> Get() noexcept {
        if (!_state.Get()) return ACS_ERR(Asset, 10, "FAssetFuture: empty");
        FThreadPool::Wait(_state->counter);
        if (_state->has_error) return TResult<TSharedPtr<FAsset>>(_state->error);
        return TResult<TSharedPtr<FAsset>>(OkInit, _state->result);
    }

    /**
     * Get() の別名 (直感的に同期待ちと分かるように)。
     *
     * @return Get() と同じ結果。
     */
    TResult<TSharedPtr<FAsset>> Wait() noexcept { return Get(); }

private:
    /** ワーカーと共有する非同期ロード状態 (null なら無効 future)。 */
    TSharedPtr<FAsyncLoadState> _state;
};

} // namespace acs
