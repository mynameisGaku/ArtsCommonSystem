#pragma once
#include <application/base/ApplicationBase.h>

#define ECSTEST TRUE

namespace seecs { class ECS; }

class ECSTestApp : public ApplicationBase
{
public:
    ECSTestApp();
    ~ECSTestApp();
    /// <summary>
    /// アプリ起動時に呼び出されるメソッド
    /// </summary>
    void OnStart() override;
    /// <summary>
    /// アプリ更新時に呼び出されるメソッド
    /// </summary>
    void Update() override;
    /// <summary>
    /// アプリ描画時に呼び出されるメソッド
    /// </summary>
    void Draw() override;
    /// <summary>
    /// アプリ破棄時に呼び出されるメソッド
    /// </summary>
    void OnDestroy() override;

private:

    seecs::ECS* m_pECS;
    std::vector<seecs::EntityID> m_Entities;
    bool m_IsPush;
    bool m_IsPush2;
};