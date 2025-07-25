#pragma once

#include "Pch.h"

#include "IACSM_ECS_System.h"

///<summary>
/// 登録された全Systemを制御・実行するマネージャ。依存関係の無いSystemは並列で実行される。
///</summary>
///<author>藤本樂</author>
class ACSM_ECS_SystemManager
{
public:
    ///<summary>
    /// ISystem継承クラスを登録する
    ///</summary>
    /// <param name="system">ISystem継承のSystem</param>
    void RegisterSystem(std::shared_ptr<IACSM_ECS_System> system);

    ///<summary>
    /// 任意のSystem（関数やラムダをラップしたもの）を登録する
    ///</summary>
    /// <param name="func">実行すべきSystem処理</param>
    void RegisterSystem(std::function<void()> func);

    ///<summary>
    /// 登録された全Systemを順に（または並列に）実行する
    ///</summary>
    void UpdateAll();

private:
    std::vector<std::function<void()>> m_Systems;  // 登録済みSystem群private:
    std::vector<std::shared_ptr<IACSM_ECS_System>> m_ClassSystems;  // クラス型Systemも保持
};
