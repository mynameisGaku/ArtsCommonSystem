// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/AssetPathInternerDiagnostics.h"
#include "asset/InternedAssetPath.h"
#include "container/HashMap.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"
#include "threading/Mutex.h"

namespace acs {

/** 保持するアセットパス数の上限。 */
inline constexpr usize kAssetPathInternerMaxEntries = 256u;

/** 保持する NUL 込み文字数の上限。 */
inline constexpr usize kAssetPathInternerMaxCodeUnits = 64u * 1024u;

/** Intern に空パスが渡された。 */
inline constexpr u16 kAssetPathInternerSubInvalidPath = 14u;

/** 異なる完全パスが同じ FAssetId になった。 */
inline constexpr u16 kAssetPathInternerSubHashCollision = 15u;

/** 共有パスまたは table の確保に失敗した。 */
inline constexpr u16 kAssetPathInternerSubOutOfMemory = 16u;

/**
 * アセットパスを有界に共有する所有プール。
 *
 * @details 未使用要素だけを追い出し、使用中パスのアドレスと寿命を維持する。
 */
class FAssetPathInterner {
public:
    /** デフォルトアロケータで空のプールを構築する。 */
    FAssetPathInterner() noexcept;

    /** 指定アロケータで空のプールを構築する。 */
    explicit FAssetPathInterner(FAllocator& Allocator) noexcept;

    /** コピーを禁止する。 */
    FAssetPathInterner(const FAssetPathInterner&) = delete;

    /** コピー代入を禁止する。 */
    FAssetPathInterner& operator=(const FAssetPathInterner&) = delete;

    /**
     * 検証済みパスを共有する。
     *
     * @param Path NUL 終端されたパス。
     * @param Length NUL を除く文字数。
     * @return 共有パス。確保失敗またはハッシュ衝突時はエラー。
     */
    TResult<TSharedPtr<FInternedAssetPath>> Intern(const wchar_t* Path, usize Length) noexcept;

    /** 累積診断値を一括取得する。 */
    FAssetPathInternerDiagnostics Diagnostics() const noexcept;

    /** 保持参照を解放し、診断値を初期化する。 */
    void Reset() noexcept;

private:
    /** 必要量を収めるため未使用要素を追い出す。 */
    void EvictUnusedUntilFit(usize RequiredCodeUnits) noexcept;

    /** 共有オブジェクトの確保元。 */
    FAllocator* m_Allocator = nullptr;

    /** パス ID ごとの共有文字列。 */
    THashMap<FAssetId, TSharedPtr<FInternedAssetPath>> m_Paths;

    /** 共有プールと診断値を保護する。 */
    mutable FMutex m_Lock;

    /** 累積診断値。 */
    FAssetPathInternerDiagnostics m_Diagnostics{};
};

} // namespace acs
