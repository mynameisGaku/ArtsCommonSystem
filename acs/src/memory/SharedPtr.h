// SPDX-License-Identifier: Apache-2.0
// ACS Memory — TSharedPtr<T> / TWeakPtr<T> / TSharedFromThis<T>
//   （std::shared_ptr / std::weak_ptr / enable_shared_from_this 代替）
//
// ・TSharedPtr<T> … 共有所有。コピーで強参照カウントを atomic 増加、破棄で減少、
//   0 で対象を破棄する。MakeShared 経由なら FControlBlock と T を 1 アロケーション
//   に同居させる（make_shared 相当）。
// ・TWeakPtr<T>   … 弱参照。対象の生存を延ばさない。Lock() で生きていれば
//   TSharedPtr を得る（循環参照を断ち切る用途）。
// ・TSharedFromThis<T> … メンバ関数内で自分自身の TSharedPtr を作りたいときの基底。
//
// 旧名 TRc / MakeRc は memory/Rc.h に互換エイリアスとして残してある（非推奨）。
//
// 例:
//   auto p = MakeShared<FMesh>(args...);   // TSharedPtr<FMesh>
//   TWeakPtr<FMesh> w = p;                 // 弱参照
//   if (auto s = w.Lock()) s->Render();    // 生きていれば使える
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "threading/Atomic.h"
#include "foundation/TypeTraits.h"

namespace acs {

/** 共有所有スマートポインタの前方宣言。 */
template<typename T> class TSharedPtr;

/** 弱参照スマートポインタの前方宣言。 */
template<typename T> class TWeakPtr;

/** shared-from-this 基底の前方宣言。 */
template<typename T> class TSharedFromThis;

namespace sp_detail {

/**
 * 全 TSharedPtr/TWeakPtr 共通の参照カウント制御ブロック。
 *
 * @details
 * strong は強参照 (TSharedPtr) の数で、0 になったら T を破棄する。weak は弱参照
 * (TWeakPtr) の数 + 「強参照グループ全体」を表す 1 で、0 になったら制御ブロック自体
 * (と同居する T の領域) を解放する。この 2 段構えにより T が破棄されても TWeakPtr が
 * 残っている間は制御ブロックが生き続け、Lock()/Expired() を安全に呼べる。
 */
struct FControlBlock {
    /** 強参照 (TSharedPtr) の数。0 で T を破棄する。 */
    TAtomic<u32> strong {1};

    /** 弱参照の数 + 強参照グループ全体を表す 1。0 で領域を解放する。 */
    TAtomic<u32> weak   {1};

    /** 解放に使うアロケータ。 */
    FAllocator*  alloc  = nullptr;

    /** T のデストラクタを実行する関数 (型消去された破棄フック)。 */
    void (*destroy_obj)(FControlBlock*) noexcept = nullptr;

    /** ブロック全体を解放する関数 (型消去された解放フック)。 */
    void (*free_self)  (FControlBlock*) noexcept = nullptr;

    /**
     * 強参照カウントを 1 減らし、0 になったら T を破棄する。
     *
     * @details 強参照が 1→0 になった時点で destroy_obj で T を破棄し (領域は残す)、
     * 強参照グループが保持していた弱参照 1 を ReleaseWeak で返す。
     */
    void ReleaseStrong() noexcept {
        if (strong.FetchSub(1) == 1) {     // 強参照 1→0
            destroy_obj(this);             //   T を破棄（領域はまだ残す）
            ReleaseWeak();                 //   強参照グループが保持していた弱参照を返す
        }
    }

    /**
     * 弱参照カウントを 1 減らし、0 になったら領域を解放する。
     *
     * @details 弱参照が 1→0 になった時点で free_self で制御ブロックと T の領域を解放する。
     */
    void ReleaseWeak() noexcept {
        if (weak.FetchSub(1) == 1) free_self(this);  // 弱参照 1→0: 領域解放
    }

    /**
     * 弱参照から強参照への昇格を試みる。
     *
     * @details 対象がまだ生きていれば (strong != 0) CAS で strong を +1 する。
     * 破棄と競合しても安全 (Lock/Pin の実装に使う)。
     * @return 昇格できれば true、既に破棄済みなら false。
     */
    bool TryAddStrong() noexcept {
        u32 s = strong.Load(EMemoryOrder::Acquire);
        while (s != 0) {
            if (s == ~u32(0)) return false;
            if (strong.CompareExchange(s, s + 1)) return true;  // 失敗時 s は実値に更新される
        }
        return false;
    }

    /** 弱参照カウントを wrap させずに 1 増やす。 */
    bool TryAddWeak() noexcept
    {
        u32 Count = weak.Load(EMemoryOrder::Acquire);
        while (Count != 0u) {
            if (Count == ~u32(0)) return false;
            if (weak.CompareExchange(Count, Count + 1u)) return true;
        }
        return false;
    }

    /**
     * 現在の強参照数を返す。
     *
     * @return 強参照カウントの現在値。
     */
    u32 StrongCount() const noexcept { return strong.Load(EMemoryOrder::Acquire); }
};

/**
 * FControlBlock と T を 1 ブロックに同居させるインライン版 (MakeShared 用)。
 *
 * @details 制御ブロックの直後に T 用の生ストレージを置き、1 アロケーションで両者を確保する。
 * @tparam T 同居させる対象の型。
 */
template<typename T>
struct TInlineBlock : FControlBlock {
    /** T を構築する生ストレージ (T のアライメントを満たす)。 */
    alignas(T) byte storage[sizeof(T)];

    /**
     * 同居している T へのポインタを返す。
     *
     * @return storage 上に構築された T へのポインタ。
     */
    T* Ptr() noexcept { return reinterpret_cast<T*>(&storage[0]); }

    /**
     * destroy_obj に登録する T 破棄フック。
     *
     * @details トリビアル破棄可能型では ~T() を省略する。
     * @param cb 破棄対象の制御ブロック (TInlineBlock にダウンキャストする)。
     */
    static void DestroyObj(FControlBlock* cb) noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>)
            static_cast<TInlineBlock*>(cb)->Ptr()->~T();
    }

    /**
     * free_self に登録する領域解放フック。
     *
     * @param cb 解放対象の制御ブロック (TInlineBlock にダウンキャストする)。
     */
    static void FreeSelf(FControlBlock* cb) noexcept {
        auto* self = static_cast<TInlineBlock*>(cb);
        FAllocator* const a = self->alloc;
        a->Free(self);
    }
};

/**
 * T が TSharedFromThis<T> を継承していれば weak-this を仕込む内部フック。
 *
 * @tparam T 生成された対象の型。
 * @param sp 生成直後の共有ポインタ。
 */
template<typename T> void HookSharedFromThis(const TSharedPtr<T>& sp) noexcept;

} // namespace sp_detail

/**
 * 対象を共有所有する参照カウント式スマートポインタ (std::shared_ptr 代替)。
 *
 * @details
 * コピーで強参照カウントを atomic 増加、破棄で減少し、0 で対象を破棄する。MakeShared
 * 経由なら FControlBlock と T を 1 アロケーションに同居させる。
 * @tparam T 共有所有する対象の型。
 */
template<typename T>
class TSharedPtr {
public:
    /** 共有対象の要素型。 */
    using ElementType = T;

    /** 空の (何も所有しない) 共有ポインタを構築する。 */
    TSharedPtr() noexcept = default;

    /**
     * nullptr から空の共有ポインタを構築する。
     *
     * @param  nullptr リテラル (引数名なし)。
     */
    TSharedPtr(decltype(nullptr)) noexcept {}

    /**
     * コピー構築。同じ対象を共有し強参照カウントを +1 する。
     *
     * @param o コピー元の共有ポインタ。
     */
    TSharedPtr(const TSharedPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb && !m_Cb->TryAddStrong()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * ムーブ構築。o の所有を奪い o を空にする (カウントは増減しない)。
     *
     * @param o 所有の移動元 (この後は空になる)。
     */
    TSharedPtr(TSharedPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }

    /**
     * 派生 U から基底 T へアップキャストするコピー構築。
     *
     * @details 同じ制御ブロックを共有し強参照カウントを +1 する。
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param o コピー元の共有ポインタ。
     */
    template<typename U>
    TSharedPtr(const TSharedPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb && !m_Cb->TryAddStrong()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * 派生 U から基底 T へアップキャストするムーブ構築。
     *
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param o 所有の移動元 (この後は空になる)。
     */
    template<typename U>
    TSharedPtr(TSharedPtr<U>&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }

    /**
     * コピー代入。現在の所有を解放してから o と同じ対象を共有する。
     *
     * @param o コピー元の共有ポインタ。
     * @return 自身への参照。
     */
    TSharedPtr& operator=(const TSharedPtr& o) noexcept { TSharedPtr(o).Swap(*this); return *this; }

    /**
     * ムーブ代入。現在の所有を解放してから o の所有を奪う。
     *
     * @param o 所有の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TSharedPtr& operator=(TSharedPtr&& o)      noexcept { TSharedPtr(static_cast<TSharedPtr&&>(o)).Swap(*this); return *this; }

    /**
     * nullptr 代入。現在の所有を解放して空にする。
     *
     * @param  nullptr リテラル (引数名なし)。
     * @return 自身への参照。
     */
    TSharedPtr& operator=(decltype(nullptr))   noexcept { Reset(); return *this; }

    /** 破棄時に強参照カウントを 1 減らす (0 になれば対象を破棄)。 */
    ~TSharedPtr() noexcept { if (m_Cb) m_Cb->ReleaseStrong(); }

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
    u32 UseCount() const noexcept { return m_Cb ? m_Cb->StrongCount() : 0; }

    /** 所有を解放して空にする。 */
    void Reset() noexcept { TSharedPtr().Swap(*this); }

    /**
     * 別の共有ポインタと中身を入れ替える。
     *
     * @param o 入れ替え相手。
     */
    void Swap(TSharedPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::FControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    /** 所有する対象 (空なら nullptr)。 */
    T*                       m_Ptr = nullptr;

    /** 参照カウント制御ブロック (空なら nullptr)。 */
    sp_detail::FControlBlock* m_Cb  = nullptr;

    /**
     * 既に +1 済みの強参照を採用する内部コンストラクタ (追加で +1 しない)。
     *
     * @details MakeShared / TWeakPtr::Lock から使う。
     * @param p 採用する対象ポインタ。
     * @param cb 採用する制御ブロック (強参照は加算済み)。
     */
    TSharedPtr(T* p, sp_detail::FControlBlock* cb) noexcept : m_Ptr(p), m_Cb(cb) {}

    /** 別要素型の TSharedPtr から private メンバへアクセスするための friend 宣言。 */
    template<typename U> friend class TSharedPtr;

    /** TWeakPtr が m_Ptr/m_Cb にアクセスするための friend 宣言。 */
    template<typename U> friend class TWeakPtr;

    /** MakeSharedIn が採用コンストラクタを使うための friend 宣言。 */
    template<typename U, typename... A> friend TSharedPtr<U> MakeSharedIn(FAllocator&, A&&...) noexcept;
};

/**
 * 対象の生存を延ばさない弱参照スマートポインタ (std::weak_ptr 代替)。
 *
 * @details
 * 制御ブロックの弱参照カウントだけを保持し、対象自体は生かさない。Lock() で
 * 生きていれば強参照 (TSharedPtr) を得られる (循環参照を断ち切る用途)。
 * @tparam T 監視する対象の型。
 */
template<typename T>
class TWeakPtr {
public:
    /** 空の弱参照を構築する。 */
    TWeakPtr() noexcept = default;

    /**
     * 共有ポインタから弱参照を構築する。
     *
     * @param s 監視対象を共有しているポインタ。
     */
    TWeakPtr(const TSharedPtr<T>& s) noexcept : m_Ptr(s.m_Ptr), m_Cb(s.m_Cb) {
        if (m_Cb && !m_Cb->TryAddWeak()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * コピー構築。同じ対象を監視し弱参照カウントを +1 する。
     *
     * @param o コピー元の弱参照。
     */
    TWeakPtr(const TWeakPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb && !m_Cb->TryAddWeak()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * ムーブ構築。o の監視を奪い o を空にする (カウントは増減しない)。
     *
     * @param o 監視の移動元 (この後は空になる)。
     */
    TWeakPtr(TWeakPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }

    /**
     * 派生 U の共有ポインタから基底 T の弱参照を構築する。
     *
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param s 監視対象を共有しているポインタ。
     */
    template<typename U>
    TWeakPtr(const TSharedPtr<U>& s) noexcept : m_Ptr(s.m_Ptr), m_Cb(s.m_Cb) {
        if (m_Cb && !m_Cb->TryAddWeak()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * 派生 U の弱参照から基底 T の弱参照を構築する。
     *
     * @tparam U 元の要素型 (U* が T* へ変換可能であること)。
     * @param o コピー元の弱参照。
     */
    template<typename U>
    TWeakPtr(const TWeakPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb && !m_Cb->TryAddWeak()) {
            m_Ptr = nullptr;
            m_Cb = nullptr;
        }
    }

    /**
     * コピー代入。現在の監視を解放してから o と同じ対象を監視する。
     *
     * @param o コピー元の弱参照。
     * @return 自身への参照。
     */
    TWeakPtr& operator=(const TWeakPtr& o)      noexcept { TWeakPtr(o).Swap(*this); return *this; }

    /**
     * ムーブ代入。現在の監視を解放してから o の監視を奪う。
     *
     * @param o 監視の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TWeakPtr& operator=(TWeakPtr&& o)           noexcept { TWeakPtr(static_cast<TWeakPtr&&>(o)).Swap(*this); return *this; }

    /**
     * 共有ポインタから代入し、その対象を監視する。
     *
     * @param s 監視対象を共有しているポインタ。
     * @return 自身への参照。
     */
    TWeakPtr& operator=(const TSharedPtr<T>& s) noexcept { TWeakPtr(s).Swap(*this); return *this; }

    /** 破棄時に弱参照カウントを 1 減らす (0 になれば領域を解放)。 */
    ~TWeakPtr() noexcept { if (m_Cb) m_Cb->ReleaseWeak(); }

    /**
     * 対象が既に破棄されているかを返す。
     *
     * @return 制御ブロックが無いか強参照が 0 なら true。
     */
    bool Expired() const noexcept { return !m_Cb || m_Cb->StrongCount() == 0; }

    /**
     * 対象がまだ生きているかを返す。
     *
     * @return 生きていれば true (Expired の否定)。
     */
    bool IsValid() const noexcept { return !Expired(); }

    /**
     * 生きていれば強参照 (TSharedPtr) を取得する。
     *
     * @details TryAddStrong で昇格を試み、破棄と競合しても安全に取得する。
     * @return 生きていれば対象を共有する TSharedPtr、死んでいれば空。
     */
    TSharedPtr<T> Lock() const noexcept {
        if (m_Cb && m_Cb->TryAddStrong()) return TSharedPtr<T>(m_Ptr, m_Cb);
        return TSharedPtr<T>();
    }

    /** 監視を解放して空にする。 */
    void Reset() noexcept { TWeakPtr().Swap(*this); }

    /**
     * 別の弱参照と中身を入れ替える。
     *
     * @param o 入れ替え相手。
     */
    void Swap(TWeakPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::FControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    /** 監視対象 (空なら nullptr)。 */
    T*                       m_Ptr = nullptr;

    /** 参照カウント制御ブロック (空なら nullptr)。 */
    sp_detail::FControlBlock* m_Cb  = nullptr;

    /** 別要素型の TWeakPtr から private メンバへアクセスするための friend 宣言。 */
    template<typename U> friend class TWeakPtr;

    /** TSharedPtr が m_Ptr/m_Cb にアクセスするための friend 宣言。 */
    template<typename U> friend class TSharedPtr;
};

/**
 * メンバ関数内で自分自身の TSharedPtr/TWeakPtr を作れるようにする基底 (enable_shared_from_this 相当)。
 *
 * @details
 * T を public 継承させると AsShared()/AsWeak() が使える。必ず MakeShared 経由で生成すること
 * (生ポインタからの構築では weak-this が仕込まれず機能しない)。
 * @tparam T 派生する自分自身の型 (CRTP)。
 */
template<typename T>
class TSharedFromThis {
public:
    /**
     * 自分自身を指す強参照を返す。
     *
     * @return 自分自身を共有する TSharedPtr (MakeShared 未経由なら空)。
     */
    TSharedPtr<T> AsShared() noexcept { return m_WeakThis.Lock(); }

    /**
     * 自分自身を指す弱参照を返す。
     *
     * @return 自分自身を監視する TWeakPtr (MakeShared 未経由なら空)。
     */
    TWeakPtr<T>   AsWeak()   const noexcept { return m_WeakThis; }

protected:
    /** 基底を構築する (weak-this は MakeShared 時に仕込まれる)。 */
    TSharedFromThis() noexcept = default;

    /**
     * コピー構築。weak-this はコピーしない (各オブジェクトが自身を指すため)。
     *
     * @param  コピー元 (引数名なし。weak-this は引き継がない)。
     */
    TSharedFromThis(const TSharedFromThis&) noexcept {}

    /**
     * コピー代入。weak-this は変更しない。
     *
     * @param  コピー元 (引数名なし。weak-this は引き継がない)。
     * @return 自身への参照。
     */
    TSharedFromThis& operator=(const TSharedFromThis&) noexcept { return *this; }

    /** 基底のデストラクタ。 */
    ~TSharedFromThis() noexcept = default;

private:
    /** 自分自身を指す弱参照 (MakeShared 時に HookSharedFromThis が仕込む)。 */
    mutable TWeakPtr<T> m_WeakThis;

    /** HookSharedFromThis が m_WeakThis を書き換えるための friend 宣言。 */
    template<typename U> friend void sp_detail::HookSharedFromThis(const TSharedPtr<U>&) noexcept;
};

namespace sp_detail {
/**
 * T が TSharedFromThis<T> を継承していれば生成直後に weak-this を仕込む。
 *
 * @tparam T 生成された対象の型。
 * @param sp 生成直後の共有ポインタ。
 */
template<typename T>
void HookSharedFromThis(const TSharedPtr<T>& sp) noexcept {
    if constexpr (IsBaseOfV<TSharedFromThis<T>, T>) {
        static_cast<TSharedFromThis<T>*>(sp.Get())->m_WeakThis = sp;
    }
}
} // namespace sp_detail

/**
 * 指定アロケータで T を構築し共有ポインタを返す。
 *
 * @details FControlBlock と T を 1 アロケーションに同居させ、生成直後に
 * TSharedFromThis 継承時のみ weak-this を仕込む。
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param a 確保・解放に使うアロケータ。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を共有所有する TSharedPtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TSharedPtr<T> MakeSharedIn(FAllocator& a, Args&&... args) noexcept {
    using Block = sp_detail::TInlineBlock<T>;
    void* const mem = a.Alloc(sizeof(Block), alignof(Block), FSourceLoc::Current());
    if (!mem) return TSharedPtr<T>();
    auto* const blk = ::new (mem) Block();
    blk->alloc       = &a;
    blk->destroy_obj = &Block::DestroyObj;
    blk->free_self   = &Block::FreeSelf;
    ::new (blk->Ptr()) T(Forward<Args>(args)...);
    TSharedPtr<T> sp(blk->Ptr(), blk);          // strong=1 を採用
    sp_detail::HookSharedFromThis(sp);          // TSharedFromThis 継承時のみ weak-this を仕込む
    return sp;
}

/**
 * デフォルトアロケータで T を構築し共有ポインタを返す (make_shared 相当)。
 *
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を共有所有する TSharedPtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TSharedPtr<T> MakeShared(Args&&... args) noexcept {
    return MakeSharedIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

} // namespace acs
