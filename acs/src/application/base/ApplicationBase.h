#pragma once

/// <summary>
/// アプリケーションの基底
/// </summary>
class ApplicationBase
{
public:
    ApplicationBase()           {}
    virtual ~ApplicationBase()  {}
    /// <summary>
    /// アプリ起動時に呼び出されるメソッド
    /// </summary>
    virtual void OnStart()          {}
    /// <summary>
    /// アプリ更新時に呼び出されるメソッド
    /// </summary>
    virtual void Update()           {}
    /// <summary>
    /// アプリ描画時に呼び出されるメソッド
    /// </summary>
    virtual void Draw()             {}
    /// <summary>
    /// アプリ破棄時に呼び出されるメソッド
    /// </summary>
    virtual void OnDestroy()        {}
};