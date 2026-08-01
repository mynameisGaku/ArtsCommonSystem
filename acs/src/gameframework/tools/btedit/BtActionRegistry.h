// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / CBtActionRegistry
//
// ノードグラフの Action ノード (= 名前) を、実行時の関数ポインタへ解決するための
// 名前→Fn テーブル。ゲーム側が起動時に Register("MoveToPlayer", &MoveToPlayer) のように
// 登録し、エディタ/インタプリタが Action ノードの表示名をキーに Find して呼ぶ。
// これにより「コードを書かずにノードグラフを組む → 実行時はゲームが登録した関数が走る」
// という no-code オーサリングが成立する。
//
// 設計選択: STL 不使用 / 例外なし / 固定長配列。name は内部の固定バッファへコピーする
// (= 呼び出し側のリテラル寿命に依存しない)。Fn は FBtAction::Fn と同型。
#pragma once

#include "foundation/Types.h"
#include "gameframework/BehaviorTree.h"

#include <cstdio>    // std::snprintf
#include <cstring>   // std::strcmp

namespace acs::game::btedit {

/**
 * Action 名 → 関数ポインタ の解決テーブル。
 *
 * @details ゲーム側がアクション関数を名前付きで登録し、グラフインタプリタが Action
 *          ノードの名前をキーに引いて呼ぶ。固定長 (kMax) で STL 不使用。
 */
class CBtActionRegistry {
public:
    /** Action 関数の型 (FBtAction と同型: EBtStatus(*)(void* bb, f32 dt) noexcept)。 */
    using Fn = FBtAction::Fn;

    /** 登録できるアクションの上限。 */
    static constexpr u32 kMax = 64u;

    /** 1 アクション名の最大長 (NUL 含む)。 */
    static constexpr u32 kNameLen = 48u;

    /** 空のレジストリを構築する。 */
    CBtActionRegistry() noexcept = default;

    /** 全登録を消す。 */
    void Clear() noexcept { m_Count = 0u; }

    /**
     * アクションを名前付きで登録する (同名は上書き)。
     *
     * @param name アクション名 (グラフの Action ノード名と一致させる)。
     * @param fn 実行する関数ポインタ。
     * @return 登録できたら true (name/fn が null、または上限到達で false)。
     */
    bool Register(const char* name, Fn fn) noexcept {
        if (name == nullptr || fn == nullptr) return false;
        for (u32 i = 0; i < m_Count; ++i) {
            if (std::strcmp(m_Names[i], name) == 0) { m_Fns[i] = fn; return true; }
        }
        if (m_Count >= kMax) return false;
        std::snprintf(m_Names[m_Count], kNameLen, "%s", name);
        m_Fns[m_Count] = fn;
        ++m_Count;
        return true;
    }

    /**
     * 名前からアクション関数を引く。
     *
     * @param name 探すアクション名。
     * @return 一致する関数ポインタ (無ければ nullptr)。
     */
    Fn Find(const char* name) const noexcept {
        if (name == nullptr) return nullptr;
        for (u32 i = 0; i < m_Count; ++i) {
            if (std::strcmp(m_Names[i], name) == 0) return m_Fns[i];
        }
        return nullptr;
    }

    /** 登録数を返す。 */
    u32 Count() const noexcept { return m_Count; }

    /**
     * i 番目の登録名を返す (UI の候補リスト用)。
     *
     * @param i インデックス。
     * @return 登録名 (範囲外は "")。
     */
    const char* NameAt(u32 i) const noexcept { return (i < m_Count) ? m_Names[i] : ""; }

private:
    /** 登録名 (固定バッファへコピー)。 */
    char m_Names[kMax][kNameLen] = {};

    /** 登録関数ポインタ (m_Names と同 index)。 */
    Fn   m_Fns[kMax] = {};

    /** 登録数。 */
    u32  m_Count = 0u;
};

using FBtActionRegistry = CBtActionRegistry;

} // namespace acs::game::btedit
