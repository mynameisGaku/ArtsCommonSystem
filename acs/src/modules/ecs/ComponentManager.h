#pragma once

#include "EntityID.h"
#include "IComponentPool.h"
#include "ComponentTypeID.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <type_traits>

///<summary>
/// 各コンポーネント型に対応するストレージ（SoA）を管理するクラス。
/// EntityごとのComponentの追加・取得・削除を行う。
///</summary>
///<author>藤本樂</author>
class ComponentManager
{
public:
    ///<summary>
    /// コンポーネントをEntityに追加する
    ///</summary>
    /// <param name="entity">追加対象のEntity</param>
    /// <param name="component">追加するComponent</param>
    template<typename T>
    void AddComponent(EntityID entity, const T& component);

    ///<summary>
    /// 指定のEntityが持つComponentへのポインタを取得する
    ///</summary>
    /// <param name="entity">取得対象のEntity</param>
    template<typename T>
    T* GetComponent(EntityID entity);

    ///<summary>
    /// 指定のEntityからComponentを削除する
    ///</summary>
    /// <param name="entity">削除対象のEntity</param>
    template<typename T>
    void RemoveComponent(EntityID entity);

    ///<summary>
    /// 指定のComponent型の総数を返す
    ///</summary>
    template<typename T>
    size_t GetComponentCount() const;

private:
    std::unordered_map<uint32_t, std::unique_ptr<IComponentPool>> m_Pools;  // ComponentTypeID -> プール
};

#include "ComponentManager.inl"
