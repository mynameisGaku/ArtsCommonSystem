// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/LogSinkHandle.h"

namespace acs {

/** 破棄時に FLogger のログ購読を解除する移動専用所有権。 */
class FLogSinkSubscription {
public:
    /** 購読を持たない空の所有権を作る。 */
    FLogSinkSubscription() noexcept = default;

    /** 保持中の購読を解除する。 */
    ~FLogSinkSubscription() noexcept;

    /** 購読の重複所有を防ぐためコピー構築を禁止する。 */
    FLogSinkSubscription(const FLogSinkSubscription&) = delete;

    /** 購読の重複所有を防ぐためコピー代入を禁止する。 */
    FLogSinkSubscription& operator=(const FLogSinkSubscription&) = delete;

    /**
     * 購読の所有権を移す。
     * @param other 移動元の所有権。
     */
    FLogSinkSubscription(FLogSinkSubscription&& other) noexcept;

    /**
     * 現在の購読を解除して所有権を移す。
     * @param other 移動元の所有権。
     * @return この所有権。
     */
    FLogSinkSubscription& operator=(FLogSinkSubscription&& other) noexcept;

    /** 保持中の購読が現在の Logger 世代でも有効かを返す。 */
    bool IsValid() const noexcept;

    /** 保持中の購読を解除し、解除できたかを返す。 */
    bool Reset() noexcept;

    /** 保持中の購読ハンドルを返す。 */
    FLogSinkHandle Handle() const noexcept { return m_Handle; }

private:
    /**
     * 登録済みハンドルの所有権を作る。
     * @param handle 所有する購読ハンドル。
     */
    explicit FLogSinkSubscription(FLogSinkHandle handle) noexcept : m_Handle(handle) {}

    /** 解除対象を識別するハンドル。 */
    FLogSinkHandle m_Handle{};

    /** 所有権付き購読を生成する Logger。 */
    friend class FLogger;
};

} // namespace acs
