#pragma once
#include <application/base/ApplicationBase.h>

#define ECSTEST TRUE

class ACSM_ECS_ECSWorld;

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

    ACSM_ECS_ECSWorld* m_pECSWorld;
    bool m_IsPush;
};