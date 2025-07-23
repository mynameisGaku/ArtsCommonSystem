#pragma once
#include <cstdint>

///<summary>
/// Entityを一意に識別するためのID構造体。indexと世代を組み合わせて使用する。
///</summary>
///<author>藤本樂</author>
struct EntityID
{
    uint32_t index = 0;       // Entity配列上のインデックス
    uint32_t generation = 0;  // 世代番号。破棄→再利用を識別するために使う

    ///<summary>
    /// 同じEntityかどうかを比較する（indexと世代の一致）
    ///</summary>
    bool operator==(const EntityID& other) const
    {
        return index == other.index && generation == other.generation;
    }

    ///<summary>
    /// 異なるEntityかどうかを比較する
    ///</summary>
    bool operator!=(const EntityID& other) const
    {
        return !(*this == other);
    }

    ///<summary>
    /// 有効なEntityかどうか（generationが0より大きい）
    ///</summary>
    bool IsValid() const
    {
        return generation > 0;  // 無効IDは generation == 0 とみなす
    }

    ///<summary>
    /// 無効なEntityID（デフォルト無効）を返す
    ///</summary>
    static constexpr EntityID Invalid()
    {
        return EntityID{ 0, 0 };
    }
};
