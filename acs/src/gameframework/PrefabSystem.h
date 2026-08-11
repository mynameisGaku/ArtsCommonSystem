// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "memory/ObjectPtr.h"

namespace acs::game {

// ANode の full type はこのヘッダでは不要 (TObjectPtr<ANode> の宣言として
// 触るだけ)。factory 関数を実装する cpp 側で `gameframework/ANode.h` を include すること。
class ANode;

/**
 * Prefab を識別する packed 32bit handle (generational)。
 *
 * @details
 * layout は low24 = index、high8 = generation。m_Packed == 0 が invalid。
 * FNodeId / FShapeId と完全に同じパターン。
 */
struct FPrefabId {
    /** index (low24) と generation (high8) を詰めた 32bit 値。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な FPrefabId を構築する。 */
    constexpr FPrefabId() noexcept = default;

    /**
     * index と generation から FPrefabId を構築する。
     *
     * @param index 24bit のスロット index。
     * @param gen 8bit の generation。
     */
    constexpr FPrefabId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * スロット index を取り出す。
     *
     * @return low24 の index。
     */
    constexpr u32 Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を取り出す。
     *
     * @return high8 の generation。
     */
    constexpr u8  Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * handle が invalid でないかを返す。
     *
     * @details m_Packed == 0 のみ invalid。登録済 slot に存在するかは CPrefabSystem 側で別途検証する。
     * @return invalid でなければ true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 2 つの FPrefabId が同一かを比較する。
     *
     * @param o 比較対象の FPrefabId。
     * @return packed 値が等しければ true。
     */
    constexpr bool operator==(FPrefabId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 2 つの FPrefabId が異なるかを比較する。
     *
     * @param o 比較対象の FPrefabId。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FPrefabId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * Prefab を実体化するファクトリ関数の signature。
 *
 * @details
 * user_data は Register 時に渡されたコンテキストポインタ (closure 代替)。
 * 返り値は新規 ANode ツリーの所有権で、失敗時は空 TObjectPtr (= bool == false) を返してよい。
 */
using PrefabFactoryFn = TObjectPtr<ANode>(*)(void* user_data) noexcept;

/**
 * 名前付き Prefab レジストリ。
 *
 * @details
 * テンプレート (= factory 関数) を登録し、ID または名前から ANode ツリーを spawn する。
 * 1 セッション内で通常数十〜数百件の登録を想定し、線形走査ベース。登録された factory
 * ポインタの所有移譲を抑止するため non-copy / non-move。
 */
class CPrefabSystem {
public:
    /** 空のレジストリを構築する。 */
    CPrefabSystem() noexcept = default;

    /** レジストリを破棄する (factory ポインタは保管するだけなので解放処理はない)。 */
    ~CPrefabSystem() noexcept = default;

    /** コピー禁止 (登録された factory ポインタの所有移譲を抑止)。 */
    CPrefabSystem(const CPrefabSystem&)            = delete;

    /** コピー代入も禁止。 */
    CPrefabSystem& operator=(const CPrefabSystem&) = delete;

    /** ムーブ禁止。 */
    CPrefabSystem(CPrefabSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPrefabSystem& operator=(CPrefabSystem&&)      = delete;

    /**
     * 新規 Prefab を登録して FPrefabId を返す。
     *
     * @details
     * name は永続文字列を渡すこと (string literal か別バッファ管理)。CPrefabSystem は複製せず
     * ポインタを保管する。nullptr / 空文字 / factory == nullptr は弾いて invalid を返す。
     * 同名既存があっても新規 entry を作って別 ID を返す (上書きしない。FindByName は登録順で
     * 最初に見つけたものを返す)。
     * @param name Prefab 名 (永続文字列、複製されない)。
     * @param factory Prefab を実体化するファクトリ関数。
     * @param user_data factory に渡すコンテキストポインタ (既定 nullptr)。
     * @return 登録した Prefab の FPrefabId (バリデーション失敗 / 24bit 容量上限時は invalid)。
     */
    FPrefabId Register(const char* name, PrefabFactoryFn factory, void* user_data = nullptr) noexcept;

    /**
     * 名前で Prefab を検索する (登録順で最初に一致したもの)。
     *
     * @param name 検索する Prefab 名。
     * @return 一致した Prefab の FPrefabId (一致しない / name == nullptr なら invalid)。
     */
    FPrefabId FindByName(const char* name) const noexcept;

    /**
     * ID から ANode ツリーを spawn する (factory を 1 回呼ぶ)。
     *
     * @param id spawn する Prefab の FPrefabId。
     * @return 生成した ANode ツリー (id が invalid / stale / factory が nullptr なら空 TObjectPtr)。
     */
    TObjectPtr<ANode> Spawn(FPrefabId id) noexcept;

    /**
     * 名前から ANode ツリーを spawn する (内部で FindByName → Spawn 相当)。
     *
     * @param name spawn する Prefab 名。
     * @return 生成した ANode ツリー (見つからない場合は空 TObjectPtr)。
     */
    TObjectPtr<ANode> SpawnByName(const char* name) noexcept;

    /**
     * 登録を解除する。
     *
     * @details slot を再利用可能にし、generation を +1 して既存 handle を stale 化する。
     * @param id 解除する Prefab の FPrefabId。
     * @return 実際に解除したら true、id が invalid / stale なら false (no-op)。
     */
    bool Unregister(FPrefabId id) noexcept;

    /**
     * 現在 active な (= Unregister されていない) 登録数を返す。
     *
     * @return active な登録数。
     */
    u32 Count() const noexcept { return m_ActiveCount; }

    /**
     * デバッグ用に Prefab 名を取得する。
     *
     * @details 呼び出し側が条件分岐せずログにそのまま流せるよう nullptr は返さない。
     * @param id 名前を取得する Prefab の FPrefabId。
     * @return Prefab 名 (invalid / stale なら "(unknown)")。
     */
    const char* GetName(FPrefabId id) const noexcept;

    /**
     * 全登録を破棄する (既存の ID は全て stale 化される)。
     *
     * @details slot と generation 履歴は保持し、次の Register で世代を進める。
     * これにより ClearAll 前の ID が同じ index へ再登録した新 Prefab を指すことを防ぐ。
     */
    void ClearAll() noexcept;

private:
    /**
     * 1 件の Prefab 登録を表すスロット。
     *
     * @details m_Entries 内に index 順で並び、index 0 は invalid 予約用の dummy。
     */
    struct FPrefabEntry {
        /** Prefab 名 (永続文字列へのポインタ、複製しない)。 */
        const char*     name      = nullptr;

        /** Prefab を実体化するファクトリ関数。 */
        PrefabFactoryFn factory   = nullptr;

        /** factory に渡すコンテキストポインタ。 */
        void*           user_data = nullptr;

        /** generation (0 = 未使用、1〜255 で循環し stale handle を検出)。 */
        u8              gen       = 0;

        /** スロットが現在登録中かどうか。 */
        bool            active    = false;
    };

    /**
     * 未使用 slot を探して index を返す。
     *
     * @details 空きが無ければ末尾に push する。index 0 は invalid 予約なので、24bit の
     * index 上限へ到達した場合だけ失敗値 0 を返す。
     * @return 確保したスロットの index (容量上限時は 0)。
     */
    u32 AcquireSlot() noexcept;

    /** 登録スロットの配列 (index 0 は dummy)。 */
    TArray<FPrefabEntry> m_Entries;

    /** 現在 active な登録数。 */
    u32                m_ActiveCount = 0;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPrefabSystem = CPrefabSystem;

} // namespace acs::game
