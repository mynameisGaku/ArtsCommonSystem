#pragma once

#include "Pch.h"

#include "ACSM_ECS_EntityID.h"

///<summary>
/// EntityIDの生成・破棄・再利用を管理するクラス。
/// Entityの有効性チェックや世代管理も担う。
///</summary>
///<author>藤本樂</author>
class ACSM_ECS_EntityManager
{
public:
    ///<summary>
    /// EntityManagerを初期化するコンストラクタ
    ///</summary>
    ACSM_ECS_EntityManager();

    ///<summary>
    /// 新しいEntityを生成する
    ///</summary>
    ACSM_ECS_EntityID CreateEntity();

    ///<summary>
    /// 指定されたEntityを破棄し、再利用可能にする
    ///</summary>
    /// <param name="id">削除対象のEntityID</param>
    void DestroyEntity(ACSM_ECS_EntityID id);

    ///<summary>
    /// 指定されたEntityがまだ有効かどうかを返す
    ///</summary>
    /// <param name="id">判定するEntityID</param>
    bool IsAlive(ACSM_ECS_EntityID id) const;

    ///<summary>
    /// 全てのEntity情報を破棄・初期化する
    ///</summary>
    void Clear();

    ///<summary>
    /// 現在有効なEntityの数を返す
    ///</summary>
    size_t GetAliveCount() const;

private:
    std::vector<uint32_t> m_Generations;        // index -> 現在の世代番号（世代管理用）
    std::queue<uint32_t>  m_FreeIndices;        // 再利用可能なEntity index（削除済み）
};
