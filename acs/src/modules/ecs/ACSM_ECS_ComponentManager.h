#pragma once

#include "Pch.h"

#include "ACSM_ECS_EntityID.h"
#include "ACSM_ECS_IComponentPool.h"

///<summary>
/// 各コンポーネント型に対応するストレージ（SoA）を管理するクラス。
/// EntityごとのComponentの追加・取得・削除を行う。
///</summary>
///<author>藤本樂</author>
class ACSM_ECS_ComponentManager
{
public:
    ///<summary>
    /// コンポーネントをEntityに追加する
    ///</summary>
    /// <param name="entity">追加対象のEntity</param>
    /// <param name="component">追加するComponent</param>
    template<typename T>
    void AddComponent(ACSM_ECS_EntityID entity, const T& component);

    ///<summary>
    /// 指定のEntityが持つComponentへのポインタを取得する
    ///</summary>
    /// <param name="entity">取得対象のEntity</param>
    template<typename T>
    T* GetComponent(ACSM_ECS_EntityID entity);

    ///<summary>
    /// 指定のEntityからComponentを削除する
    ///</summary>
    /// <param name="entity">削除対象のEntity</param>
    template<typename T>
    void RemoveComponent(ACSM_ECS_EntityID entity);

    ///<summary>
    /// 指定のComponent型の総数を返す
    ///</summary>
    template<typename T>
    size_t GetComponentCount() const;

private:
    std::unordered_map<uint32_t, std::unique_ptr<ACSM_ECS_IComponentPool>> m_Pools;  // ACSM_ECS_ComponentTypeID -> プール
};

#include "ACSM_ECS_ComponentManager.inl"
