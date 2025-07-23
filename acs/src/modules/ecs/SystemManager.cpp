#include "SystemManager.h"

void SystemManager::RegisterSystem(std::shared_ptr<ISystem> system)
{
    m_ClassSystems.push_back(system);
}

void SystemManager::RegisterSystem(std::function<void()> func)
{
    m_Systems.push_back(func);
}

void SystemManager::UpdateAll()
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