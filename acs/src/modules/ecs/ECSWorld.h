#pragma once

#include "EntityManager.h"
#include "ComponentManager.h"
#include "ArchetypeManager.h"
#include "SystemManager.h"
#include "ComponentTypeID.h"
#include <vector>

///<summary>
/// Entity/Component/Systemを統合管理するECSの中心クラス。
/// 外部からはこのクラスを通してすべて操作する。
///</summary>
///<author>藤本樂</author>
class ECSWorld
{
public:
    EntityID CreateEntity();
    void DestroyEntity(EntityID id);

    template<typename... Components>
    void AddComponents(EntityID id, Components&&... components);

    template<typename T>
    T* GetComponent(EntityID id);

    void RegisterSystem(std::function<void()> func);
    void RegisterSystem(std::shared_ptr<ISystem> system);
    void Update();
    int GetEntityCount() const;

    ///<summary>
    /// 指定されたEntityの持つコンポーネント型IDを取得する（ソート済みvector）
    ///</summary>
    std::vector<uint32_t> GetComponentTypes(EntityID id) const;

    ///<summary>
    /// 指定したComponentTypeIDをすべて持つEntityの一覧を取得する
    ///</summary>
    ///<param name="typeIDs">必要なComponentTypeIDの集合</param>
    std::vector<EntityID> QueryEntities(const std::vector<uint32_t>& typeIDs) const;

private:
    EntityManager     m_EntityManager;
    ComponentManager  m_ComponentManager;
    ArchetypeManager  m_ArchetypeManager;
    SystemManager     m_SystemManager;
};

#include "ECSWorld.inl"
