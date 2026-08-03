// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ecs/World.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "container/Array.h"
#include "foundation/Move.h"

namespace acs {

/**
 * ECS の構造変更 (Destroy / Add<T> / Remove<T>) を記録し、Flush() で CWorld へ一括適用する。
 *
 * @details Query 反復中に安全へ構造変更するための遅延バッファ。記録順に適用する。
 * 非コピー (退避値とコマンド列を所有するため)。
 */
class FEntityCommandBuffer {
public:
    /**
     * 適用先 CWorld と退避用アロケータを束ねて構築する。
     *
     * @param world 適用先の CWorld (参照を保持)。
     * @param alloc Add 値の退避とコマンド列に使うアロケータ。
     */
    explicit FEntityCommandBuffer(CWorld& world, IAllocator& alloc = DefaultAllocator()) noexcept
        : m_World(&world), m_Alloc(&alloc), m_Commands(alloc)
    {
    }

    /** 未適用の記録があれば退避値を解放してから破棄する。 */
    ~FEntityCommandBuffer() noexcept
    {
        Clear();
    }

    FEntityCommandBuffer(const FEntityCommandBuffer&) = delete;
    FEntityCommandBuffer& operator=(const FEntityCommandBuffer&) = delete;

    /**
     * エンティティ破棄を記録する。
     *
     * @param e 破棄するエンティティ。
     */
    void Destroy(FEntityId e) noexcept
    {
        /** 追加する遅延コマンド。 */
        FCommand c{};
        c.kind = ECommandKind::Destroy;
        c.entity = e;
        if (!m_Commands.TryAdd(c)) {
            m_bOverflowed = true;
        }
    }

    /**
     * T コンポーネントの除去を記録する。
     *
     * @tparam T 除去するコンポーネント型。
     * @param e 除去対象のエンティティ。
     */
    template<typename T>
    void Remove(FEntityId e) noexcept
    {
        /** 追加する遅延コマンド。 */
        FCommand c{};
        c.kind = ECommandKind::Remove;
        c.entity = e;
        c.apply = &ApplyRemove<T>;
        if (!m_Commands.TryAdd(c)) {
            m_bOverflowed = true;
        }
    }

    /**
     * T コンポーネントの追加を記録する (小型の単純な値はコマンド内へ保持)。
     *
     * @tparam T 追加するコンポーネント型。
     * @param e 追加先のエンティティ。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void Add(FEntityId e, T value) noexcept
    {
        /** 追加する遅延コマンド。 */
        FCommand c{};
        c.kind = ECommandKind::Add;
        c.entity = e;
        c.apply = &ApplyAdd<T>;
        if (!StoreValue(c, Move(value))) return;
        if (!m_Commands.TryAdd(c)) {
            DestroyStoredValue(c);
            m_bOverflowed = true;
        }
    }

    /**
     * 空エンティティの生成を記録する (Flush 時に CWorld::Create が走る)。
     *
     * @details 逐次 Each 中の Create は即時でも安全 (World.h 参照) だが、EachParallel 中は
     * CWorld::Create がスレッドセーフでないため本記録を使う。生成される FEntityId は
     * Flush 時に確定するので、事前に参照したい用途には使えない。
     */
    void Create() noexcept
    {
        /** 追加する遅延コマンド。 */
        FCommand c{};
        c.kind = ECommandKind::Create;
        if (!m_Commands.TryAdd(c)) {
            m_bOverflowed = true;
        }
    }

    /**
     * 「生成 + T を付与」を記録する (小型の単純な値は内部保持、Flush 時に生成)。
     *
     * @details 弾やパーティクル等の並列スポーンに使う。複数コンポーネントを同一エンティティへ
     * 付けたい場合は Flush 後に CWorld 側で組み立てるか、T を集約構造体にすること。
     * @tparam T 生成と同時に付与するコンポーネント型。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void CreateWith(T value) noexcept
    {
        /** 追加する遅延コマンド。 */
        FCommand c{};
        c.kind = ECommandKind::Create;
        c.apply = &ApplyAdd<T>;
        if (!StoreValue(c, Move(value))) return;
        if (!m_Commands.TryAdd(c)) {
            DestroyStoredValue(c);
            m_bOverflowed = true;
        }
    }

    /**
     * 記録した全操作を記録順に CWorld へ適用し、バッファを空にする。
     *
     * @details Add は退避値を CWorld へムーブしてから退避値を破棄する。適用後は Size()==0。
     */
    void Flush() noexcept
    {
        /** 適用するコマンド位置。 */
        for (usize i = 0; i < m_Commands.Num(); ++i) {
            /** 現在適用するコマンド。 */
            FCommand& c = m_Commands[i];
            switch (c.kind) {
            case ECommandKind::Destroy:
                m_World->Destroy(c.entity);
                break;
            case ECommandKind::Remove:
                c.apply(*m_World, c.entity, nullptr);
                break;
            case ECommandKind::Add:
                c.apply(*m_World, c.entity, c.Value());
                DestroyStoredValue(c);
                break;
            case ECommandKind::Create: {
                /** 新しく生成したエンティティ。 */
                const FEntityId created = m_World->Create();
                // CreateWith の場合だけ保持値を付与する。
                if (c.apply != nullptr) {
                    c.apply(*m_World, created, c.Value());
                    DestroyStoredValue(c);
                }
                break;
            }
            }
        }
        m_Commands.Reset();
    }

    /**
     * 記録を適用せず破棄する (退避した Add / CreateWith 値も解放する)。
     */
    void Clear() noexcept
    {
        /** 破棄するコマンド位置。 */
        for (usize i = 0; i < m_Commands.Num(); ++i) {
            /** 現在破棄するコマンド。 */
            FCommand& c = m_Commands[i];
            DestroyStoredValue(c);
        }
        m_Commands.Reset();
    }

    /** 記録済み操作数を返す。 */
    usize Size() const noexcept
    {
        return m_Commands.Num();
    }

    /** 記録が空なら true。 */
    bool IsEmpty() const noexcept
    {
        return m_Commands.IsEmpty();
    }

    /**
     * 予想コマンド数を一括予約する。成功後、その件数まではコマンド配列を再確保しない。
     *
     * @param command_count 予約するコマンド数。
     * @return 予約できれば true。確保失敗時は false。
     */
    bool TryReserve(usize command_count) noexcept
    {
        if (m_Commands.TryReserve(command_count)) return true;
        m_bOverflowed = true;
        return false;
    }

    /**
     * OOM で記録を落としたことがあるかを返す。
     *
     * @return 一度でも記録に失敗していれば true (Flush の適用が不完全)。
     */
    bool HasOverflowed() const noexcept
    {
        return m_bOverflowed;
    }

private:
    /** 遅延コマンドの種別。 */
    enum class ECommandKind : u8 {
        /** エンティティを破棄する。 */
        Destroy,
        /** コンポーネントを追加する。 */
        Add,
        /** コンポーネントを除去する。 */
        Remove,
        /** apply が空なら空生成し、設定済みならコンポーネントも付与する。 */
        Create,
    };

    /** 1 つの遅延コマンド。value / thunk は種別に応じて使う。 */
    struct FCommand {
        /** コマンド内へ直接保持できる最大バイト数。 */
        static constexpr usize kInlineValueBytes = 16u;
        /** 実行する操作種別。 */
        ECommandKind kind = ECommandKind::Destroy;
        /** 操作対象のエンティティ。 */
        FEntityId entity{};
        /** 大型または非単純な値の退避先。 */
        void* value = nullptr;
        /** 追加または除去を型消去して適用する関数。 */
        void (*apply)(CWorld&, FEntityId, void*) noexcept = nullptr;
        /** ヒープへ退避した値を型消去して破棄する関数。 */
        void (*destroy)(IAllocator&, void*) noexcept = nullptr;
        /** 小規模値を確保せず保持する領域。 */
        alignas(16) byte inline_value[kInlineValueBytes]{};
        /** 小規模値領域を使用中なら true。 */
        bool inline_value_used = false;

        /** 保持方式にかかわらず値の先頭を返す。 */
        void* Value() noexcept
        {
            return inline_value_used ? static_cast<void*>(inline_value) : value;
        }
    };

    /** 値を内部領域またはヒープへ保持し、失敗時は overflow 状態にする。 */
    template<typename T>
    bool StoreValue(FCommand& command, T&& value) noexcept
    {
        if constexpr (IsTriviallyCopyableV<T> && IsTriviallyDestructibleV<T> && sizeof(T) <= FCommand::kInlineValueBytes && alignof(T) <= 16u) {
            MemCopy(command.inline_value, &value, sizeof(T));
            command.inline_value_used = true;
            return true;
        } else {
            /** ヒープへ退避した値。 */
            T* const stored = New<T>(*m_Alloc, Move(value));
            if (!stored) {
                m_bOverflowed = true;
                return false;
            }
            command.value = stored;
            command.destroy = &DestroyValue<T>;
            return true;
        }
    }

    /** コマンドがヒープへ退避した値だけを破棄し、保持状態を初期化する。 */
    void DestroyStoredValue(FCommand& command) noexcept
    {
        if (!command.inline_value_used && command.destroy != nullptr) {
            command.destroy(*m_Alloc, command.value);
        }
        command.value = nullptr;
        command.destroy = nullptr;
        command.inline_value_used = false;
    }

    /** 退避した T を CWorld へムーブ追加する型消去 thunk。 */
    template<typename T>
    static void ApplyAdd(CWorld& world, FEntityId e, void* value) noexcept
    {
        world.Add<T>(e, Move(*static_cast<T*>(value)));
    }

    /** CWorld から T を除去する型消去 thunk。 */
    template<typename T>
    static void ApplyRemove(CWorld& world, FEntityId e, void* /*value*/) noexcept
    {
        world.Remove<T>(e);
    }

    /** 退避した T を破棄して領域を返す型消去 thunk。 */
    template<typename T>
    static void DestroyValue(IAllocator& alloc, void* value) noexcept
    {
        Delete(alloc, static_cast<T*>(value));
    }

    /** 適用先 CWorld。バッファより長く生存する。 */
    CWorld* m_World = nullptr;

    /** 退避とコマンド列に使うアロケータ。 */
    IAllocator* m_Alloc = nullptr;

    /** 記録順のコマンド列。 */
    TArray<FCommand> m_Commands;

    /** OOM で記録を落としたら true。 */
    bool m_bOverflowed = false;
};

} // namespace acs
