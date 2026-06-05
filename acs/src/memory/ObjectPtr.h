// SPDX-License-Identifier: Apache-2.0
// ACS Memory — FObject + TObjectPtr / TWeakObjectPtr / TStrongObjectPtr
//   （UE の UObject + TObjectPtr/TWeakObjectPtr/TStrongObjectPtr に相当）
//
// TSharedPtr/TWeakPtr との違い:
//   ・FObject は「自分の制御ブロックへの逆ポインタ」を内部に持つ。そのため
//     生ポインタ (T*) からでも強参照/弱参照を作れる（UE の TWeakObjectPtr の肝）。
//   ・つまり「AActor* を持っていて、そこから弱参照を作る」が書ける。
//
// 使い分け:
//   ・任意の型を共有したい           → TSharedPtr / TWeakPtr（SharedPtr.h）
//   ・エンジンの「オブジェクト」を扱う → FObject 継承 + TObjectPtr / TWeakObjectPtr
//
// 例:
//   class FEnemy : public FObject { public: void Hit(); };
//   TObjectPtr<FEnemy> e = NewObject<FEnemy>();    // 強参照（生かし続ける）
//   TWeakObjectPtr<FEnemy> w = e;                  // 弱参照（生死を監視するだけ）
//   e.Reset();                                     // 強参照ゼロ → FEnemy 破棄
//   if (w.IsValid()) w.Get()->Hit();               // → false。破棄済なので呼ばれない
#pragma once

#include "memory/SharedPtr.h"   // sp_detail::ControlBlock / InlineBlock を共有

namespace acs {

/** オブジェクトへの強参照ポインタの前方宣言。 */
template<typename T> class TObjectPtr;

/** オブジェクトへの弱参照ポインタの前方宣言。 */
template<typename T> class TWeakObjectPtr;

/**
 * 参照カウント管理されるエンジンオブジェクトの基底。
 *
 * @details
 * NewObject<T>() で生成すると制御ブロックが割り当てられ、その逆ポインタ (m_Cb) を
 * 自身に保持する。この逆ポインタのおかげで生ポインタ (T*) からでも強参照/弱参照を
 * 作れる (UE の TWeakObjectPtr の肝)。
 */
class FObject {
public:
    /** 派生オブジェクトを正しく破棄するための仮想デストラクタ。 */
    virtual ~FObject() noexcept = default;

protected:
    /** 基底を構築する (逆ポインタは NewObject 時に仕込まれる)。 */
    FObject() noexcept = default;

    /**
     * コピー構築。逆ポインタはコピーしない (各オブジェクトが自分の制御ブロックを指すため)。
     *
     * @param  コピー元 (引数名なし。逆ポインタは引き継がない)。
     */
    FObject(const FObject&) noexcept {}

    /**
     * コピー代入。逆ポインタは変更しない。
     *
     * @param  コピー元 (引数名なし。逆ポインタは引き継がない)。
     * @return 自身への参照。
     */
    FObject& operator=(const FObject&) noexcept { return *this; }

private:
    /** 自分の制御ブロックへの逆ポインタ (NewObject 経由でのみ設定される)。 */
    sp_detail::ControlBlock* m_Cb = nullptr;

    /** TObjectPtr が m_Cb へアクセスするための friend 宣言。 */
    template<typename U> friend class TObjectPtr;

    /** TWeakObjectPtr が m_Cb へアクセスするための friend 宣言。 */
    template<typename U> friend class TWeakObjectPtr;

    /** NewObjectIn が逆ポインタを仕込むための friend 宣言。 */
    template<typename U, typename... A> friend TObjectPtr<U> NewObjectIn(FAllocator&, A&&...) noexcept;
};

/**
 * FObject への強参照 (対象を生かし続ける所有ポインタ)。
 *
 * @details コピーで強参照カウントを +1、破棄で -1 し、0 で対象を破棄する。
 * @tparam T 所有する FObject 派生型。
 */
template<typename T>
class TObjectPtr {
public:
    /** 所有対象の要素型。 */
    using ElementType = T;

    /** 空の (何も所有しない) 強参照を構築する。 */
    TObjectPtr() noexcept = default;

    /**
     * nullptr から空の強参照を構築する。
     *
     * @param  nullptr リテラル (引数名なし)。
     */
    TObjectPtr(decltype(nullptr)) noexcept {}

    /**
     * 生ポインタから強参照を構築する。
     *
     * @details obj は NewObject 経由で生成され逆ポインタを持つこと。逆ポインタが無い
     * (制御ブロックが無い) 場合は空のまま構築する。逆ポインタがあれば強参照を +1 する。
     * @param obj 所有する対象 (FObject 継承必須。nullptr 可)。
     */
    explicit TObjectPtr(T* obj) noexcept {
        static_assert(IsBaseOfV<FObject, T>, "TObjectPtr<T>: T は FObject を継承していること");
        if (obj) {
            sp_detail::ControlBlock* const cb = static_cast<FObject*>(obj)->m_Cb;
            if (cb) { m_Ptr = obj; m_Cb = cb; m_Cb->AddStrong(); }
        }
    }

    /**
     * コピー構築。同じ対象を所有し強参照カウントを +1 する。
     *
     * @param o コピー元の強参照。
     */
    TObjectPtr(const TObjectPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddStrong(); }

    /**
     * ムーブ構築。o の所有を奪い o を空にする (カウントは増減しない)。
     *
     * @param o 所有の移動元 (この後は空になる)。
     */
    TObjectPtr(TObjectPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }

    /**
     * 派生 U から基底 T へアップキャストするコピー構築。
     *
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param o コピー元の強参照。
     */
    template<typename U> TObjectPtr(const TObjectPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddStrong(); }

    /**
     * 派生 U から基底 T へアップキャストするムーブ構築。
     *
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param o 所有の移動元 (この後は空になる)。
     */
    template<typename U> TObjectPtr(TObjectPtr<U>&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }

    /**
     * コピー代入。現在の所有を解放してから o と同じ対象を所有する。
     *
     * @param o コピー元の強参照。
     * @return 自身への参照。
     */
    TObjectPtr& operator=(const TObjectPtr& o) noexcept { TObjectPtr(o).Swap(*this); return *this; }

    /**
     * ムーブ代入。現在の所有を解放してから o の所有を奪う。
     *
     * @param o 所有の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TObjectPtr& operator=(TObjectPtr&& o)      noexcept { TObjectPtr(static_cast<TObjectPtr&&>(o)).Swap(*this); return *this; }

    /**
     * nullptr 代入。現在の所有を解放して空にする。
     *
     * @param  nullptr リテラル (引数名なし)。
     * @return 自身への参照。
     */
    TObjectPtr& operator=(decltype(nullptr))   noexcept { Reset(); return *this; }

    /** 破棄時に強参照カウントを 1 減らす (0 になれば対象を破棄)。 */
    ~TObjectPtr() noexcept { if (m_Cb) m_Cb->ReleaseStrong(); }

    /**
     * 所有する対象の生ポインタを返す。
     *
     * @return 保持中の生ポインタ (空なら nullptr)。
     */
    T*  Get()        const noexcept { return m_Ptr; }

    /**
     * 対象への参照を返す。
     *
     * @return 保持中の対象への参照 (空のとき呼ぶのは未定義)。
     */
    T&  operator*()  const noexcept { return *m_Ptr; }

    /**
     * 対象のメンバへアクセスするためのポインタを返す。
     *
     * @return 保持中の生ポインタ。
     */
    T*  operator->() const noexcept { return m_Ptr; }

    /**
     * 対象を保持しているかを返す。
     *
     * @return 非 null を保持していれば true。
     */
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }

    /**
     * 対象を保持しているかを返す。
     *
     * @return 非 null を保持していれば true。
     */
    bool IsValid() const noexcept { return m_Ptr != nullptr; }

    /**
     * 現在の強参照数を返す (デバッグ用)。
     *
     * @return 強参照カウント (空なら 0)。
     */
    u32  UseCount() const noexcept { return m_Cb ? m_Cb->StrongCount() : 0; }

    /** 所有を解放して空にする。 */
    void Reset() noexcept { TObjectPtr().Swap(*this); }

    /**
     * 別の強参照と中身を入れ替える。
     *
     * @param o 入れ替え相手。
     */
    void Swap(TObjectPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    /** 所有する対象 (空なら nullptr)。 */
    T*                       m_Ptr = nullptr;

    /** 参照カウント制御ブロック (空なら nullptr)。 */
    sp_detail::ControlBlock* m_Cb  = nullptr;

    /**
     * 既に +1 済みの強参照を採用する内部コンストラクタ (追加で +1 しない)。
     *
     * @details NewObjectIn / TWeakObjectPtr::Pin から使う。
     * @param p 採用する対象ポインタ。
     * @param cb 採用する制御ブロック (強参照は加算済み)。
     */
    TObjectPtr(T* p, sp_detail::ControlBlock* cb) noexcept : m_Ptr(p), m_Cb(cb) {}

    /** 別要素型の TObjectPtr から private メンバへアクセスするための friend 宣言。 */
    template<typename U> friend class TObjectPtr;

    /** TWeakObjectPtr が m_Ptr/m_Cb にアクセスするための friend 宣言。 */
    template<typename U> friend class TWeakObjectPtr;

    /** NewObjectIn が採用コンストラクタを使うための friend 宣言。 */
    template<typename U, typename... A> friend TObjectPtr<U> NewObjectIn(FAllocator&, A&&...) noexcept;
};

/**
 * 「強参照である」ことを明示したいとき用の TObjectPtr 別名 (UE の TStrongObjectPtr に相当)。
 *
 * @tparam T 所有する FObject 派生型。
 */
template<typename T> using TStrongObjectPtr = TObjectPtr<T>;

/**
 * FObject への弱参照 (生死を監視するが生存は延ばさない)。
 *
 * @details 制御ブロックの弱参照カウントだけを保持し、生ポインタ (T*) からも構築できる。
 * @tparam T 監視する FObject 派生型。
 */
template<typename T>
class TWeakObjectPtr {
public:
    /** 空の弱参照を構築する。 */
    TWeakObjectPtr() noexcept = default;

    /**
     * nullptr から空の弱参照を構築する。
     *
     * @param  nullptr リテラル (引数名なし)。
     */
    TWeakObjectPtr(decltype(nullptr)) noexcept {}

    /**
     * 生ポインタから弱参照を構築する。
     *
     * @details obj は NewObject 経由で生成され逆ポインタを持つこと。逆ポインタが無い場合は
     * 空のまま構築する。逆ポインタがあれば弱参照を +1 する。
     * @param obj 監視する対象 (FObject 継承必須。nullptr 可)。
     */
    explicit TWeakObjectPtr(T* obj) noexcept {
        static_assert(IsBaseOfV<FObject, T>, "TWeakObjectPtr<T>: T は FObject を継承していること");
        if (obj) {
            sp_detail::ControlBlock* const cb = static_cast<FObject*>(obj)->m_Cb;
            if (cb) { m_Ptr = obj; m_Cb = cb; m_Cb->AddWeak(); }
        }
    }

    /**
     * 強参照から弱参照を構築する。
     *
     * @param s 監視対象を所有している強参照。
     */
    TWeakObjectPtr(const TObjectPtr<T>& s) noexcept : m_Ptr(s.Get()), m_Cb(s.m_Cb) { if (m_Cb) m_Cb->AddWeak(); }

    /**
     * コピー構築。同じ対象を監視し弱参照カウントを +1 する。
     *
     * @param o コピー元の弱参照。
     */
    TWeakObjectPtr(const TWeakObjectPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddWeak(); }

    /**
     * ムーブ構築。o の監視を奪い o を空にする (カウントは増減しない)。
     *
     * @param o 監視の移動元 (この後は空になる)。
     */
    TWeakObjectPtr(TWeakObjectPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }

    /**
     * コピー代入。現在の監視を解放してから o と同じ対象を監視する。
     *
     * @param o コピー元の弱参照。
     * @return 自身への参照。
     */
    TWeakObjectPtr& operator=(const TWeakObjectPtr& o) noexcept { TWeakObjectPtr(o).Swap(*this); return *this; }

    /**
     * ムーブ代入。現在の監視を解放してから o の監視を奪う。
     *
     * @param o 監視の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TWeakObjectPtr& operator=(TWeakObjectPtr&& o)      noexcept { TWeakObjectPtr(static_cast<TWeakObjectPtr&&>(o)).Swap(*this); return *this; }

    /**
     * 強参照から代入し、その対象を監視する。
     *
     * @param s 監視対象を所有している強参照。
     * @return 自身への参照。
     */
    TWeakObjectPtr& operator=(const TObjectPtr<T>& s)  noexcept { TWeakObjectPtr(s).Swap(*this); return *this; }

    /** 破棄時に弱参照カウントを 1 減らす (0 になれば領域を解放)。 */
    ~TWeakObjectPtr() noexcept { if (m_Cb) m_Cb->ReleaseWeak(); }

    /**
     * 対象がまだ生きているかを返す。
     *
     * @return 制御ブロックがあり強参照が 1 以上なら true。
     */
    bool IsValid() const noexcept { return m_Cb && m_Cb->StrongCount() > 0; }

    /**
     * 対象が既に破棄されているかを返す。
     *
     * @return 死んでいれば true (IsValid の否定)。
     */
    bool IsStale() const noexcept { return !IsValid(); }

    /**
     * 生きていれば対象ポインタを返す (簡易アクセス)。
     *
     * @details 別スレッドが同時に破棄し得る状況では Pin() を使うこと。
     * @return 生きていれば対象ポインタ、死んでいれば nullptr。
     */
    T* Get() const noexcept { return IsValid() ? m_Ptr : nullptr; }

    /**
     * 生きていれば強参照を獲得して返す (破棄と競合しても安全)。
     *
     * @details TryAddStrong で昇格を試みる。
     * @return 生きていれば対象を所有する TObjectPtr、死んでいれば空。
     */
    TObjectPtr<T> Pin() const noexcept {
        if (m_Cb && m_Cb->TryAddStrong()) return TObjectPtr<T>(m_Ptr, m_Cb);
        return TObjectPtr<T>();
    }

    /** 監視を解放して空にする。 */
    void Reset() noexcept { TWeakObjectPtr().Swap(*this); }

    /**
     * 別の弱参照と中身を入れ替える。
     *
     * @param o 入れ替え相手。
     */
    void Swap(TWeakObjectPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    /** 監視対象 (空なら nullptr)。 */
    T*                       m_Ptr = nullptr;

    /** 参照カウント制御ブロック (空なら nullptr)。 */
    sp_detail::ControlBlock* m_Cb  = nullptr;

    /** 別要素型の TWeakObjectPtr から private メンバへアクセスするための friend 宣言。 */
    template<typename U> friend class TWeakObjectPtr;

    /** TObjectPtr が m_Ptr/m_Cb にアクセスするための friend 宣言。 */
    template<typename U> friend class TObjectPtr;
};

/**
 * 指定アロケータでオブジェクトを生成し強参照を返す。
 *
 * @details ControlBlock と T を 1 アロケーションに同居させ、生成した T に制御ブロックへの
 * 逆ポインタを仕込んでから強参照 1 を採用して返す。
 * @tparam T 生成する FObject 派生型。
 * @tparam Args T のコンストラクタ引数型。
 * @param a 確保・解放に使うアロケータ。
 * @param args T のコンストラクタへ転送する引数。
 * @return 生成したオブジェクトを所有する TObjectPtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TObjectPtr<T> NewObjectIn(FAllocator& a, Args&&... args) noexcept {
    static_assert(IsBaseOfV<FObject, T>, "NewObject<T>: T は FObject を継承していること");
    using Block = sp_detail::InlineBlock<T>;
    void* const mem = a.Alloc(sizeof(Block), alignof(Block), FSourceLoc::Current());
    if (!mem) return TObjectPtr<T>();
    auto* const blk = ::new (mem) Block();
    blk->alloc       = &a;
    blk->destroy_obj = &Block::DestroyObj;
    blk->free_self   = &Block::FreeSelf;
    T* const obj = ::new (blk->Ptr()) T(Forward<Args>(args)...);
    static_cast<FObject*>(obj)->m_Cb = blk;          // 逆ポインタを仕込む
    return TObjectPtr<T>(obj, blk);                  // strong=1 を採用
}

/**
 * デフォルトアロケータでオブジェクトを生成し強参照を返す。
 *
 * @tparam T 生成する FObject 派生型。
 * @tparam Args T のコンストラクタ引数型。
 * @param args T のコンストラクタへ転送する引数。
 * @return 生成したオブジェクトを所有する TObjectPtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TObjectPtr<T> NewObject(Args&&... args) noexcept {
    return NewObjectIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

} // namespace acs
