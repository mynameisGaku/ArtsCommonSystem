#pragma once
#include <cstdint>
#include <typeindex>
#include <unordered_map>
#include <mutex>

///<summary>
/// 各コンポーネント型に一意なIDを割り振るユーティリティ
///</summary>
///<author>藤本樂</author>
class ComponentTypeID
{
public:
    ///<summary>
    /// 任意の型Tに対応する一意なIDを返す。
    /// 複数回呼んでも同じIDを返す。
    ///</summary>
    /// <typeparam name="T">対象のコンポーネント型</typeparam>
    template<typename T>
    static uint32_t Get();

    ///<summary>
    /// 現在割り当てられた最大ID数を返す
    ///</summary>
    static uint32_t GetCount();

private:
    static std::unordered_map<std::type_index, uint32_t> s_TypeToIDMap;  // 型 -> ID
    static uint32_t s_NextID;                                            // 次に割り当てるID
    static std::mutex s_Mutex;                                           // スレッドセーフ用
};

#include "ComponentTypeID.inl"
