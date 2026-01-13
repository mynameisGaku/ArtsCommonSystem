#pragma once

/// <summary>
/// アプリケーションのインターフェース
/// </summary>
__interface IApplication
{
public:
    /// <summary>
    /// アプリ起動時に呼び出されるメソッド
    /// </summary>
    virtual void OnBoot()           {}
    /// <summary>
    /// アプリ更新開始時に呼び出されるメソッド
    /// </summary>
    virtual void OnStart()          {}
    /// <summary>
    /// アプリ更新時に呼び出されるメソッド
    /// </summary>
    virtual void OnUpdate()           {}
    /// <summary>
    /// アプリ描画時に呼び出されるメソッド
    /// </summary>
    virtual void OnDraw()             {}
    /// <summary>
    /// アプリ破棄時に呼び出されるメソッド
    /// </summary>
    virtual void OnDestroy()        {}
};