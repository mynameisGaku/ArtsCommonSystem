#pragma once

#include "Pch.h"

#include "ACSM_ECS_ArchetypeManager.h"
#include "ACSM_ECS_ComponentManager.h"
#include "ACSM_ECS_EntityID.h"
#include "ACSM_ECS_EntityManager.h"
#include "ACSM_ECS_SystemManager.h"
#include "IACSM_ECS_System.h"

///<summary>
/// Entity/Component/Systemを統合管理するECSの中心クラス。
/// 外部からはこのクラスを通してすべて操作する。
///</summary>
///<author>藤本樂</author>
class ACSM_ECS_ECSWorld
{
public:
    ACSM_ECS_EntityID CreateEntity();
    void DestroyEntity(ACSM_ECS_EntityID id);

    template<typename... Components>
    void AddComponents(ACSM_ECS_EntityID id, Components&&... components);

    template<typename T>
    T* GetComponent(ACSM_ECS_EntityID id);

    void RegisterSystem(std::function<void()> func);
    void RegisterSystem(std::shared_ptr<IACSM_ECS_System> system);
    void Update();
    int GetEntityCount() const;

    ///<summary>
    /// 指定されたEntityの持つコンポーネント型IDを取得する（ソート済みvector）
    ///</summary>
    std::vector<uint32_t> GetComponentTypes(ACSM_ECS_EntityID id) const;

    ///<summary>
    /// 指定したComponentTypeIDをすべて持つEntityの一覧を取得する
    ///</summary>
    ///<param name="typeIDs">必要なComponentTypeIDの集合</param>
    std::vector<ACSM_ECS_EntityID> QueryEntities(const std::vector<uint32_t>& typeIDs) const;

private:
    ACSM_ECS_EntityManager     m_EntityManager;
    ACSM_ECS_ComponentManager  m_ComponentManager;
    ACSM_ECS_ArchetypeManager  m_ArchetypeManager;
    ACSM_ECS_SystemManager     m_SystemManager;
};

#include "ACSM_ECS_ECSWorld.inl"
