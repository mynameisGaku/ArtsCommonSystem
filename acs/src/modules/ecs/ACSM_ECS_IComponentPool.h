#pragma once

#include "Pch.h"

#include "ACSM_ECS_EntityID.h"

///<summary>
/// コンポーネントプール共通インターフェース。あらゆる型のストレージを抽象化する。
///</summary>
///<author>藤本樂</author>
class ACSM_ECS_IComponentPool
{
public:
    virtual ~ACSM_ECS_IComponentPool() {}

    ///<summary>
    /// 指定Entityのコンポーネントを削除する
    ///</summary>
    virtual void Remove(ACSM_ECS_EntityID id) = 0;

    ///<summary>
    /// コンポーネント数を返す
    ///</summary>
    virtual size_t GetCount() const = 0;
};
