// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / no-code オーサリング・カタログ
//
// ノードグラフを「コードを書かずに組む → ロード時にソースコードの関数/変数へ結ぶ」
// no-code オーサリングを成立させるための 2 つの軽量カタログを提供する。
//
//   ・FBtConditionRegistry … Condition デコレーター用。名前 → bool(*)(void* bb)。
//     ゲーム側が `Register("CanSeePlayer", &CanSeePlayer)` で登録し、エディタ/
//     インタプリタが Condition ノードの名前をキーに引いて評価する (true なら子を
//     実行、false なら子をスキップして Failure)。FBtActionRegistry の bool 版。
//
//   ・FBtBlackboardSchema … 比較条件 (variable <op> constant) 用。ブラックボードの
//     フィールドを「名前 + 型 + バイトオフセット」で宣言するスキーマ。ゲーム側が
//     `schema.Add("health", EBtVarType::F32, offsetof(Bb, health))` で登録すると、
//     エディタは変数候補をドロップダウン表示でき、インタプリタはロード時に解決した
//     オフセットで BtCompareVar により値を読んで定数と比較する。
//
// 設計選択: FBtActionRegistry と同形 (STL 不使用 / 例外なし / 固定長配列 / name は
// 内部バッファへコピー / 全 noexcept)。
#pragma once

#include "foundation/Types.h"
#include "gameframework/BehaviorTree.h"

#include <cstdio>    // std::snprintf
#include <cstring>   // std::strcmp

namespace acs::game::btedit {

/**
 * Condition 名 → bool 関数 の解決テーブル (Condition デコレーター用)。
 *
 * @details ゲーム側が条件関数 (bool(*)(void* bb)) を名前付きで登録し、グラフ
 *          インタプリタが Condition ノードの名前をキーに引いて評価する。
 */
class FBtConditionRegistry {
public:
    /** 条件関数の型 (blackboard を受け取り bool を返す)。 */
    using Fn = bool(*)(void* blackboard) noexcept;

    /** 登録できる条件の上限。 */
    static constexpr u32 kMax = 64u;

    /** 1 条件名の最大長 (NUL 含む)。 */
    static constexpr u32 kNameLen = 48u;

    /** 空のレジストリを構築する。 */
    FBtConditionRegistry() noexcept = default;

    /** 全登録を消す。 */
    void Clear() noexcept { m_Count = 0u; }

    /**
     * 条件を名前付きで登録する (同名は上書き)。
     *
     * @param name 条件名 (グラフの Condition ノード名と一致させる)。
     * @param fn 評価する bool 関数ポインタ。
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
     * 名前から条件関数を引く。
     *
     * @param name 探す条件名。
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

/**
 * ブラックボード変数の宣言テーブル (名前 + 型 + バイトオフセット)。
 *
 * @details
 * ゲーム側が blackboard 構造体のフィールドを名前付きで登録し、エディタが変数候補を
 * 提示・インタプリタが BtCompareVar でオフセット読みするための schema。
 * offsetof で登録する想定。
 */
class FBtBlackboardSchema {
public:
    /** 登録できる変数の上限。 */
    static constexpr u32 kMax = 64u;

    /** 1 変数名の最大長 (NUL 含む)。 */
    static constexpr u32 kNameLen = 48u;

    /** 空のスキーマを構築する。 */
    FBtBlackboardSchema() noexcept = default;

    /** 全登録を消す。 */
    void Clear() noexcept { m_Count = 0u; }

    /**
     * 変数を登録する (同名は上書き)。
     *
     * @param name 変数名 (グラフの比較条件で参照する)。
     * @param type 変数の型。
     * @param offset blackboard 先頭からのバイトオフセット (offsetof 推奨)。
     * @return 登録できたら true (name が null、または上限到達で false)。
     */
    bool Add(const char* name, EBtVarType type, u32 offset) noexcept {
        if (name == nullptr) return false;
        for (u32 i = 0; i < m_Count; ++i) {
            if (std::strcmp(m_Names[i], name) == 0) { m_Types[i] = type; m_Offsets[i] = offset; return true; }
        }
        if (m_Count >= kMax) return false;
        std::snprintf(m_Names[m_Count], kNameLen, "%s", name);
        m_Types[m_Count]   = type;
        m_Offsets[m_Count] = offset;
        ++m_Count;
        return true;
    }

    /**
     * 名前から変数のインデックスを引く。
     *
     * @param name 探す変数名。
     * @return 見つかったインデックス (無ければ kInvalid)。
     */
    u32 IndexOf(const char* name) const noexcept {
        if (name == nullptr) return kInvalid;
        for (u32 i = 0; i < m_Count; ++i) {
            if (std::strcmp(m_Names[i], name) == 0) return i;
        }
        return kInvalid;
    }

    /** 登録数を返す。 */
    u32 Count() const noexcept { return m_Count; }

    /** i 番目の変数名を返す (範囲外は "")。 */
    const char* NameAt(u32 i) const noexcept { return (i < m_Count) ? m_Names[i] : ""; }

    /** i 番目の変数型を返す (範囲外は F32)。 */
    EBtVarType TypeAt(u32 i) const noexcept { return (i < m_Count) ? m_Types[i] : EBtVarType::F32; }

    /** i 番目の変数オフセットを返す (範囲外は 0)。 */
    u32 OffsetAt(u32 i) const noexcept { return (i < m_Count) ? m_Offsets[i] : 0u; }

    /** IndexOf の "見つからない" シグナル。 */
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

private:
    /** 変数名 (固定バッファへコピー)。 */
    char       m_Names[kMax][kNameLen] = {};

    /** 変数型 (m_Names と同 index)。 */
    EBtVarType m_Types[kMax] = {};

    /** バイトオフセット (m_Names と同 index)。 */
    u32        m_Offsets[kMax] = {};

    /** 登録数。 */
    u32        m_Count = 0u;
};

/**
 * エディタ所有の動的ブラックボード (名前+型+値を自前で保持する変数ストア)。
 *
 * @details
 * FBtBlackboardSchema が「C++ 構造体のフィールドへの offset 参照 (= コード所有・
 * エディタは読み取り専用)」なのに対し、本クラスは値そのものを内部に持つため、
 * エディタから変数の追加 / リネーム / 削除 / 型変更 / 値編集ができる。コード側は
 * 名前で Get/Set してアクセスし (`bb.SetF32("hp", 100)`)、グラフの Compare
 * デコレーターは名前で値を引いて比較する。これにより「コードに無い変数をエディタ
 * だけで足して条件に使う」完全な no-code オーサリングが成立する。
 *
 * void* blackboard として渡せば、Action/Condition 関数からは
 * `static_cast<FBtBlackboard*>(bb)` で取り回せる。
 */
class FBtBlackboard {
public:
    /** 保持できる変数の上限。 */
    static constexpr u32 kMax = 64u;

    /** 1 変数名の最大長 (NUL 含む)。 */
    static constexpr u32 kNameLen = 48u;

    /** IndexOf / Add の "無効" シグナル。 */
    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    /** 空のブラックボードを構築する。 */
    FBtBlackboard() noexcept = default;

    /** 全変数を消す。 */
    void Clear() noexcept { m_Count = 0u; }

    // ===== schema 編集 (エディタ / コードから) =====

    /**
     * 変数を追加する (値は 0 初期化)。
     *
     * @param name 変数名 (nullptr なら "var<N>" を自動生成)。同名があればそれを返す。
     * @param type 変数の型。
     * @return 追加 / 既存のインデックス (上限到達は kInvalid)。
     */
    u32 Add(const char* name, EBtVarType type) noexcept {
        if (name != nullptr) {
            const u32 ex = IndexOf(name);
            if (ex != kInvalid) return ex;
        }
        if (m_Count >= kMax) return kInvalid;
        const u32 idx = m_Count;
        if (name != nullptr) {
            std::snprintf(m_Vars[idx].name, kNameLen, "%s", name);
        } else {
            // 既存と衝突しない自動名を生成 ("var0", "var1", …)。Remove で index が
            // 詰まっても名前は据え置きなので、suffix==idx が既存名と被り得る → 空きを探す。
            char cand[kNameLen];
            u32 suffix = idx;
            do {
                std::snprintf(cand, kNameLen, "var%u", suffix);
                ++suffix;
            } while (IndexOf(cand) != kInvalid);   // 既存 (0..m_Count-1) に無いものを採用
            std::snprintf(m_Vars[idx].name, kNameLen, "%s", cand);
        }
        m_Vars[idx].type  = type;
        m_Vars[idx].val.i = 0;    // 全ビット 0 = false / 0 / 0.0f
        ++m_Count;
        return idx;
    }

    /**
     * idx の変数をリネームする。
     *
     * @param idx 対象インデックス。
     * @param newname 新しい名前 (nullptr / 空は無視)。
     * @return 成功なら true。
     */
    bool Rename(u32 idx, const char* newname) noexcept {
        if (idx >= m_Count || newname == nullptr || newname[0] == '\0') return false;
        std::snprintf(m_Vars[idx].name, kNameLen, "%s", newname);
        return true;
    }

    /**
     * idx の変数を削除する (後続を詰める)。
     *
     * @param idx 対象インデックス。
     * @return 成功なら true。
     */
    bool Remove(u32 idx) noexcept {
        if (idx >= m_Count) return false;
        for (u32 i = idx; i + 1 < m_Count; ++i) m_Vars[i] = m_Vars[i + 1];
        --m_Count;
        return true;
    }

    /**
     * idx の変数の型を変更する (現在値を新型へ値変換して保持)。
     *
     * @param idx 対象インデックス。
     * @param type 新しい型。
     * @return 成功なら true。
     */
    bool SetType(u32 idx, EBtVarType type) noexcept {
        if (idx >= m_Count) return false;
        const f32 cur = ValueAsF32(idx);     // 現在値を f32 で取得してから付け替え
        m_Vars[idx].type = type;
        switch (type) {
            case EBtVarType::Bool: m_Vars[idx].val.b = (cur != 0.0f); break;
            case EBtVarType::I32:  m_Vars[idx].val.i = static_cast<i32>(cur); break;
            case EBtVarType::F32:  m_Vars[idx].val.f = cur; break;
        }
        return true;
    }

    // ===== 問い合わせ =====

    /** 名前からインデックスを引く (無ければ kInvalid)。 */
    u32 IndexOf(const char* name) const noexcept {
        if (name == nullptr) return kInvalid;
        for (u32 i = 0; i < m_Count; ++i) {
            if (std::strcmp(m_Vars[i].name, name) == 0) return i;
        }
        return kInvalid;
    }

    /** 変数が存在するか。 */
    bool Has(const char* name) const noexcept { return IndexOf(name) != kInvalid; }

    /** 変数数。 */
    u32 Count() const noexcept { return m_Count; }

    /** idx の変数名 (範囲外は "")。 */
    const char* NameAt(u32 idx) const noexcept { return (idx < m_Count) ? m_Vars[idx].name : ""; }

    /** idx の変数名の編集可能バッファ (UI のインライン rename 用、範囲外は nullptr)。 */
    char* NameBufAt(u32 idx) noexcept { return (idx < m_Count) ? m_Vars[idx].name : nullptr; }

    /** idx の型 (範囲外は F32)。 */
    EBtVarType TypeAt(u32 idx) const noexcept { return (idx < m_Count) ? m_Vars[idx].type : EBtVarType::F32; }

    // ===== 値アクセス (名前指定、コードから) =====

    bool GetBool(const char* name) const noexcept { const u32 i = IndexOf(name); return (i != kInvalid) && m_Vars[i].val.b; }
    i32  GetI32 (const char* name) const noexcept { const u32 i = IndexOf(name); return (i != kInvalid) ? m_Vars[i].val.i : 0; }
    f32  GetF32 (const char* name) const noexcept { const u32 i = IndexOf(name); return (i != kInvalid) ? m_Vars[i].val.f : 0.0f; }

    void SetBool(const char* name, bool v) noexcept { const u32 i = IndexOf(name); if (i != kInvalid) m_Vars[i].val.b = v; }
    void SetI32 (const char* name, i32  v) noexcept { const u32 i = IndexOf(name); if (i != kInvalid) m_Vars[i].val.i = v; }
    void SetF32 (const char* name, f32  v) noexcept { const u32 i = IndexOf(name); if (i != kInvalid) m_Vars[i].val.f = v; }

    /** 名前指定で値を f32 に正規化して返す (Compare 評価用、無ければ 0)。 */
    f32 GetAsF32(const char* name) const noexcept {
        const u32 i = IndexOf(name);
        return (i != kInvalid) ? ValueAsF32(i) : 0.0f;
    }

    // ===== 値アクセス (インデックス指定の生ポインタ、UI のインライン編集用) =====
    // 型が一致するときだけ非 null を返す (呼び出し側は TypeAt で分岐すること)。

    bool* BoolPtrAt(u32 idx) noexcept { return (idx < m_Count && m_Vars[idx].type == EBtVarType::Bool) ? &m_Vars[idx].val.b : nullptr; }
    i32*  I32PtrAt (u32 idx) noexcept { return (idx < m_Count && m_Vars[idx].type == EBtVarType::I32)  ? &m_Vars[idx].val.i : nullptr; }
    f32*  F32PtrAt (u32 idx) noexcept { return (idx < m_Count && m_Vars[idx].type == EBtVarType::F32)  ? &m_Vars[idx].val.f : nullptr; }

private:
    /** 1 変数 = 名前 + 型 + 値 (型に応じて union のいずれかを使う)。 */
    struct Var {
        char       name[kNameLen] = {};
        EBtVarType type           = EBtVarType::F32;
        union { bool b; i32 i; f32 f; } val { };
    };

    /** idx の現在値を f32 に正規化して返す (内部ヘルパ)。 */
    f32 ValueAsF32(u32 idx) const noexcept {
        switch (m_Vars[idx].type) {
            case EBtVarType::Bool: return m_Vars[idx].val.b ? 1.0f : 0.0f;
            case EBtVarType::I32:  return static_cast<f32>(m_Vars[idx].val.i);
            case EBtVarType::F32:  return m_Vars[idx].val.f;
        }
        return 0.0f;
    }

    /** 変数配列。 */
    Var m_Vars[kMax] = {};

    /** 変数数。 */
    u32 m_Count = 0u;
};

} // namespace acs::game::btedit
