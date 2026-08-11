// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "foundation/Types.h"
#include "container/InlineArray.h"

namespace acs::game {

/**
 * 遅延実行コマンドの本体となる関数ポインタ型。
 *
 * @details user は Enqueue 時に渡したコンテキストポインタ (this 想定)。STL の
 * std::function は使わない (ACS 規約: heap allocation / RTTI / 例外を持ち込まない)。
 * @param user Enqueue 時に渡したコンテキストポインタ。
 */
using SceneCommandFn = void(*)(void* user) noexcept;

/**
 * キュー内の 1 コマンドエントリ。
 *
 * @details label / fn / user は呼出し側所有で、本クラスは複製しない。
 */
struct FCommandRecord {
    /** 同一性比較に使うラベル (文字列リテラル前提、複製しない)。 */
    const char* label    = nullptr;

    /** 実行するコマンド本体。 */
    SceneCommandFn   fn       = nullptr;

    /** fn に渡すコンテキストポインタ (this 想定)。 */
    void*       user     = nullptr;

    /** true なら Flush で 1 回実行後に削除、false なら毎 Flush 残す。 */
    bool        one_shot = true;

    /** 実行順 (昇順、低いほど先に実行。既定 100)。 */
    u32         priority = 100u;
};

/**
 * 走査中に発火した構造変更要求をフレーム末で順次実行する遅延実行キュー。
 *
 * @details
 * OnUpdate / OnDraw の走査中に editor 等が「ノード追加」「コンポーネント差し替え」を
 * 要求しても、Flush 時に integrity を保ったまま priority 昇順で適用する。CSceneManager の
 * 単一 pending op と違い複数 command を保持し、one_shot/repeating・denounce・cancel を持つ。
 * 非コピー・非ムーブで、関数ポインタ + void* user により STL を使わず全 noexcept で実装する。
 */
class CSceneCommandQueue {
public:
    /** 空のキューを構築する。 */
    CSceneCommandQueue() noexcept = default;

    /** キューを破棄する (保留 command の callback は呼ばない)。 */
    ~CSceneCommandQueue() noexcept = default;

    /** コピー禁止 (発火中の参照との競合を防ぐため)。 */
    CSceneCommandQueue(const CSceneCommandQueue&)            = delete;

    /** コピー代入も禁止。 */
    CSceneCommandQueue& operator=(const CSceneCommandQueue&) = delete;

    /** ムーブ禁止 (AScene 埋め込み前提、所有権の曖昧さを持ち込まない)。 */
    CSceneCommandQueue(CSceneCommandQueue&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSceneCommandQueue& operator=(CSceneCommandQueue&&)      = delete;

    /**
     * 末尾に command を追加する。
     *
     * @details 同 label が既に居ても重ねて入れる。fn / label が nullptr なら警告ログを出して no-op。
     * @param label 同一性比較に使うラベル (文字列リテラル前提)。
     * @param fn 実行するコマンド本体。
     * @param user fn に渡すコンテキストポインタ。
     * @param priority 実行順 (昇順、既定 100)。
     * @param one_shot true なら Flush で 1 回だけ実行 (既定 true)。
     */
    void Enqueue(const char* label, SceneCommandFn fn, void* user,
                 u32 priority = 100u, bool one_shot = true) noexcept;

    /**
     * 同 label が既にキュー上に居なければ追加する (denounce)。
     *
     * @details 連打抑制 / Flush 待ちの重複要求を 1 つに畳む用途。既存があれば no-op で、
     * priority は新規追加時のみ反映する (one_shot は常に true 固定)。
     * @param label 同一性比較に使うラベル。
     * @param fn 実行するコマンド本体。
     * @param user fn に渡すコンテキストポインタ。
     * @param priority 新規追加時の実行順 (既定 100)。
     */
    void EnqueueIfAbsent(const char* label, SceneCommandFn fn, void* user,
                         u32 priority = 100u) noexcept;

    /**
     * label に一致する全 command を削除する (one_shot/repeating 両方)。
     *
     * @param label 削除対象のラベル (nullptr なら no-op)。
     */
    void Cancel(const char* label) noexcept;

    /**
     * priority 昇順で全 command を実行する。
     *
     * @details 同 priority 内は Enqueue 順 (安定)。実行後 one_shot は削除、repeating は残す。
     * Flush 中に Enqueue された command は今回実行せず次回 Flush に持ち越す (再エントランシ安全)。
     */
    void Flush() noexcept;

    /**
     * 現在キュー上にある command 数を返す。
     *
     * @return one_shot + repeating の合計数。
     */
    u32 PendingCount() const noexcept;

    /** 現在のコマンド列がオブジェクト内領域だけを使用しているかを返す。 */
    bool UsesInlineStorage() const noexcept {
        return m_Records.UsesInlineStorage();
    }

    /**
     * label に一致する command が 1 つ以上あるかを返す。
     *
     * @param label 検索するラベル (nullptr なら false)。
     * @return 一致する command があれば true。
     */
    bool Contains(const char* label) const noexcept;

    /**
     * 全 command を破棄する (callback は呼ばない)。
     *
     * @details AScene::OnExit 等で使う。
     */
    void ClearAll() noexcept;

private:
    /**
     * 全 command を priority 昇順に安定ソートする (insertion sort、N が小さい想定)。
     *
     * @details 同 priority は Enqueue 順 (元の index 順) を保存する。
     */
    void StableSortByPriority() noexcept;

    /** 保持中の command 列。 */
    static constexpr usize kInlineCommandCapacity = 16u;
    TInlineArray<FCommandRecord, kInlineCommandCapacity> m_Records;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSceneCommandQueue = CSceneCommandQueue;

} // namespace acs::game
