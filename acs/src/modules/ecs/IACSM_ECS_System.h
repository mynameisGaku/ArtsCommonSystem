#pragma once

///<summary>
/// すべてのSystemが継承すべき共通インターフェース
///</summary>
///<author>藤本樂</author>
class IACSM_ECS_System
{
public:
    virtual ~IACSM_ECS_System() {}

    ///<summary>
    /// 毎フレーム実行される処理
    ///</summary>
    virtual void Update() = 0;
};
