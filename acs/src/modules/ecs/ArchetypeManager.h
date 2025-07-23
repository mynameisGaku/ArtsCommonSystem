#pragma once

#include "EntityID.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>

///<summary>
/// Componentの構成が同じEntity群をまとめて管理するための仕組み。
/// Archetype単位にSoAストレージを確保し、Entityの追加・削除・クエリを管理する。
///</summary>
///<author>藤本樂</author>
class ArchetypeManager
{
public:
    ///<summary>
    /// Entityにコンポーネント構成を割り当て、Archetypeを決定する
    ///</summary>
    void AssignArchetype(EntityID entity, std::vector<uint32_t> typeIDs);

    ///<summary>
    /// 指定したComponent型を含むすべてのEntityを取得する
    ///</summary>
    std::vector<EntityID> QueryEntities(const std::vector<uint32_t>& typeIDs) const;

    ///<summary>
    /// EntityがどのArchetypeに属しているかを取得する
    ///</summary>
    std::vector<uint32_t> GetComponentTypes(EntityID entity) const;

    ///<summary>
    /// EntityをArchetypeから除去する
    ///</summary>
    void RemoveEntity(EntityID entity);

private:
    using ArchetypeKey = std::vector<uint32_t>;

    struct ArchetypeKeyHash
    {
        size_t operator()(const ArchetypeKey& key) const
        {
            size_t h = 0;
            for (uint32_t id : key)
            {
                h ^= std::hash<uint32_t>{}(id)+0x9e3779b9 + (h << 6) + (h >> 2);
            }
            return h;
        }
    };

    struct ArchetypeKeyEqual
    {
        bool operator()(const ArchetypeKey& a, const ArchetypeKey& b) const
        {
            return a == b;
        }
    };

    struct Archetype
    {
        std::vector<EntityID> entities;  // 所属するEntity一覧
    };

    std::unordered_map<ArchetypeKey, std::unique_ptr<Archetype>, ArchetypeKeyHash, ArchetypeKeyEqual> m_Archetypes;   // 構成→Entity群
    std::unordered_map<uint32_t, ArchetypeKey> m_EntityToArchetype;  // Entity.index → Archetype構成
};
