// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs {

/** オブジェクトの確保元として使うアロケータの前方宣言。 */
class IAllocator;

/** オブジェクトへの強参照ポインタの前方宣言。 */
template<typename T> class TObjectPtr;

/** オブジェクトへの弱参照ポインタの前方宣言。 */
template<typename T> class TWeakObjectPtr;

namespace sp_detail {

/** AObject が逆参照する制御ブロックの前方宣言。 */
struct FControlBlock;

} // namespace sp_detail

/** 指定アロケータでAObject派生型を生成する関数の前方宣言。 */
template<typename T, typename... Args>
TObjectPtr<T> NewObjectIn(IAllocator& Allocator, Args&&... Arguments) noexcept;

/**
 * 参照カウント管理されるACSオブジェクトの基底。
 *
 * @details
 * NewObject<T>()で生成すると制御ブロックが割り当てられ、その逆ポインタを自身に保持する。
 * この逆ポインタにより、生ポインタからも強参照と弱参照を作れる。
 */
class AObject {
public:
    /** 派生オブジェクトを正しく破棄するための仮想デストラクタ。 */
    virtual ~AObject() noexcept = default;

protected:
    /** 基底を構築する。逆ポインタはNewObject時に設定される。 */
    AObject() noexcept = default;

    /**
     * コピー構築する。各オブジェクトが自分の制御ブロックを指すため逆ポインタはコピーしない。
     *
     * @param  コピー元。逆ポインタは引き継がない。
     */
    AObject(const AObject&) noexcept {}

    /**
     * コピー代入する。自身の逆ポインタは変更しない。
     *
     * @param  コピー元。逆ポインタは引き継がない。
     * @return 自身への参照。
     */
    AObject& operator=(const AObject&) noexcept { return *this; }

private:
    /** 自分の制御ブロックへの逆ポインタ。NewObject経由でのみ設定される。 */
    sp_detail::FControlBlock* m_Cb = nullptr;

    /** TObjectPtrが逆ポインタへアクセスするためのfriend宣言。 */
    template<typename U> friend class TObjectPtr;

    /** TWeakObjectPtrが逆ポインタへアクセスするためのfriend宣言。 */
    template<typename U> friend class TWeakObjectPtr;

    /** NewObjectInが逆ポインタを設定するためのfriend宣言。 */
    template<typename U, typename... Args>
    friend TObjectPtr<U> NewObjectIn(IAllocator& Allocator, Args&&... Arguments) noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FObject = AObject;

} // namespace acs
