#pragma once

#include "EntityID.h"

///<summary>
/// コンポーネントプール共通インターフェース。あらゆる型のストレージを抽象化する。
///</summary>
///<author>藤本樂</author>
class IComponentPool
{
public:
    virtual ~IComponentPool() {}

    ///<summary>
    /// 指定Entityのコンポーネントを削除する
    ///</summary>
    virtual void Remove(EntityID id) = 0;

    ///<summary>
    /// コンポーネント数を返す
    ///</summary>
    virtual size_t GetCount() const = 0;
};
