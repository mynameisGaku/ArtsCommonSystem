// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS ECS — FEntityCommandBuffer (構造変更の遅延記録・一括適用)
// -----------------------------------------------------------------------------
// Query::Each / EachParallel の反復中は、fn へ渡した Comps& 参照が同型の Add/Remove で
// dangling するため、直接構造変更すると use-after-free になり得る (Query.h の契約参照)。
// 本バッファへ Destroy / Add<T> / Remove<T> を記録しておき、反復後に Flush() で World へ
// 記録順に適用することで安全に構造変更できる (AAA ECS の command buffer 相当)。
//
// 例:
//   FEntityCommandBuffer cmd(world);
//   world.Query<Health>().Each([&](EntityId e, Health& h) {
//       if (h.value <= 0) cmd.Destroy(e);       // 反復中は記録だけ (参照は無効化しない)
//   });
//   cmd.Flush();                                 // 反復後にまとめて適用
//
// 設計:
//   ・Add<T> の値は New<T> で安定アドレスにヒープ退避するため、再配置による自己参照
//     破壊が起きない。適用時は World へムーブし、退避値を破棄する。
//   ・Create は Each 中でも安全 (dense へ追記するだけで既存参照を無効化しない) ため
//     遅延不要。新規エンティティへ即 Add したい場合のみ、Create は即時・Add は本バッファへ。
//   ・記録の確保が OOM した場合は該当操作を落として HasOverflowed()=true にする
//     (Flush は残りを適用するが不完全であることを呼び出し側が検知できる)。
// =============================================================================
#pragma once

#include "ecs/World.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "container/Array.h"
#include "foundation/Move.h"

namespace acs {

/**
 * ECS の構造変更 (Destroy / Add<T> / Remove<T>) を記録し、Flush() で World へ一括適用する。
 *
 * @details Query 反復中に安全へ構造変更するための遅延バッファ。記録順に適用する。
 * 非コピー (退避値とコマンド列を所有するため)。
 */
class FEntityCommandBuffer {
public:
    /**
     * 適用先 World と退避用アロケータを束ねて構築する。
     *
     * @param world 適用先の World (参照を保持)。
     * @param alloc Add 値の退避とコマンド列に使うアロケータ。
     */
    explicit FEntityCommandBuffer(World& world, FAllocator& alloc = DefaultAllocator()) noexcept
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
    void Destroy(EntityId e) noexcept
    {
        Command c{};
        c.kind = ECommandKind::Destroy;
        c.entity = e;
        if (!m_Commands.TryPushBack(c)) {
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
    void Remove(EntityId e) noexcept
    {
        Command c{};
        c.kind = ECommandKind::Remove;
        c.entity = e;
        c.apply = &ApplyRemove<T>;
        if (!m_Commands.TryPushBack(c)) {
            m_bOverflowed = true;
        }
    }

    /**
     * T コンポーネントの追加を記録する (値は安定アドレスへ退避)。
     *
     * @tparam T 追加するコンポーネント型。
     * @param e 追加先のエンティティ。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void Add(EntityId e, T value) noexcept
    {
        T* const stored = New<T>(*m_Alloc, Move(value));
        if (!stored) {
            m_bOverflowed = true;
            return;
        }
        Command c{};
        c.kind = ECommandKind::Add;
        c.entity = e;
        c.value = stored;
        c.apply = &ApplyAdd<T>;
        c.destroy = &DestroyValue<T>;
        if (!m_Commands.TryPushBack(c)) {
            DestroyValue<T>(*m_Alloc, stored);
            m_bOverflowed = true;
        }
    }

    /**
     * 空エンティティの生成を記録する (Flush 時に World::Create が走る)。
     *
     * @details 逐次 Each 中の Create は即時でも安全 (World.h 参照) だが、EachParallel 中は
     * World::Create がスレッドセーフでないため本記録を使う。生成される EntityId は
     * Flush 時に確定するので、事前に参照したい用途には使えない。
     */
    void Create() noexcept
    {
        Command c{};
        c.kind = ECommandKind::Create;
        if (!m_Commands.TryPushBack(c)) {
            m_bOverflowed = true;
        }
    }

    /**
     * 「生成 + T を付与」を記録する (値は安定アドレスへ退避、Flush 時に生成)。
     *
     * @details 弾やパーティクル等の並列スポーンに使う。複数コンポーネントを同一エンティティへ
     * 付けたい場合は Flush 後に World 側で組み立てるか、T を集約構造体にすること。
     * @tparam T 生成と同時に付与するコンポーネント型。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void CreateWith(T value) noexcept
    {
        T* const stored = New<T>(*m_Alloc, Move(value));
        if (!stored) {
            m_bOverflowed = true;
            return;
        }
        Command c{};
        c.kind = ECommandKind::Create;
        c.value = stored;
        c.apply = &ApplyAdd<T>;
        c.destroy = &DestroyValue<T>;
        if (!m_Commands.TryPushBack(c)) {
            DestroyValue<T>(*m_Alloc, stored);
            m_bOverflowed = true;
        }
    }

    /**
     * 記録した全操作を記録順に World へ適用し、バッファを空にする。
     *
     * @details Add は退避値を World へムーブしてから退避値を破棄する。適用後は Size()==0。
     */
    void Flush() noexcept
    {
        for (usize i = 0; i < m_Commands.Size(); ++i) {
            Command& c = m_Commands[i];
            switch (c.kind) {
            case ECommandKind::Destroy:
                m_World->Destroy(c.entity);
                break;
            case ECommandKind::Remove:
                c.apply(*m_World, c.entity, nullptr);
                break;
            case ECommandKind::Add:
                c.apply(*m_World, c.entity, c.value);   // 退避値を World へムーブ
                c.destroy(*m_Alloc, c.value);           // ムーブ済み退避値を破棄
                break;
            case ECommandKind::Create: {
                const EntityId created = m_World->Create();
                if (c.apply != nullptr) {                // CreateWith: 退避値を付与
                    c.apply(*m_World, created, c.value);
                    c.destroy(*m_Alloc, c.value);
                }
                break;
            }
            }
        }
        m_Commands.Clear();
    }

    /**
     * 記録を適用せず破棄する (退避した Add / CreateWith 値も解放する)。
     */
    void Clear() noexcept
    {
        for (usize i = 0; i < m_Commands.Size(); ++i) {
            Command& c = m_Commands[i];
            if (c.destroy != nullptr) {                  // Add / CreateWith の退避値
                c.destroy(*m_Alloc, c.value);
            }
        }
        m_Commands.Clear();
    }

    /** 記録済み操作数を返す。 */
    usize Size() const noexcept
    {
        return m_Commands.Size();
    }

    /** 記録が空なら true。 */
    bool IsEmpty() const noexcept
    {
        return m_Commands.IsEmpty();
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
        Destroy,
        Add,
        Remove,
        Create,   // apply==nullptr なら空生成、非 null なら CreateWith (生成 + 付与)
    };

    /** 1 つの遅延コマンド。value / thunk は種別に応じて使う。 */
    struct Command {
        ECommandKind kind = ECommandKind::Destroy;
        EntityId entity{};
        void* value = nullptr;                                      // Add のみ (退避した T)
        void (*apply)(World&, EntityId, void*) noexcept = nullptr;  // Add / Remove
        void (*destroy)(FAllocator&, void*) noexcept = nullptr;     // Add のみ
    };

    /** 退避した T を World へムーブ追加する型消去 thunk。 */
    template<typename T>
    static void ApplyAdd(World& world, EntityId e, void* value) noexcept
    {
        world.Add<T>(e, Move(*static_cast<T*>(value)));
    }

    /** World から T を除去する型消去 thunk。 */
    template<typename T>
    static void ApplyRemove(World& world, EntityId e, void* /*value*/) noexcept
    {
        world.Remove<T>(e);
    }

    /** 退避した T を破棄して領域を返す型消去 thunk。 */
    template<typename T>
    static void DestroyValue(FAllocator& alloc, void* value) noexcept
    {
        Delete(alloc, static_cast<T*>(value));
    }

    /** 適用先 World。バッファより長く生存する。 */
    World* m_World = nullptr;

    /** 退避とコマンド列に使うアロケータ。 */
    FAllocator* m_Alloc = nullptr;

    /** 記録順のコマンド列。 */
    TArray<Command> m_Commands;

    /** OOM で記録を落としたら true。 */
    bool m_bOverflowed = false;
};

} // namespace acs
