#include "ACSM_ECS_SystemManager.h"

void ACSM_ECS_SystemManager::RegisterSystem(std::shared_ptr<IACSM_ECS_System> system)
{
    m_ClassSystems.push_back(system);
}

void ACSM_ECS_SystemManager::RegisterSystem(std::function<void()> func)
{
    m_Systems.push_back(func);
}

void ACSM_ECS_SystemManager::UpdateAll()
{
    std::vector<std::future<void>> futures;

    // ラムダ・関数型のSystemを並列実行
    for (auto& sys : m_Systems)
    {
        futures.push_back(std::async(std::launch::async, sys));
    }

    // クラス型Systemも同様に並列実行
    for (auto& classSys : m_ClassSystems)
    {
        futures.push_back(std::async(std::launch::async, [&]() {
            classSys->Update();
            }));
    }

    // 全スレッド終了を待つ
    for (auto& f : futures)
    {
        f.get();
    }
}