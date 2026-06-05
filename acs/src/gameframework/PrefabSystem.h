// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — FPrefabSystem
//
// 名前付きの FNode2D ツリーテンプレート (= 「Prefab」) を関数ポインタファクトリ
// として登録し、ID または名前から `acs::TUniquePtr<FNode2D>` を spawn する軽量
// レジストリ。Unity の Prefab / Unreal の Blueprint asset に相当する役割を、
// 「アセットファイル」ではなく「ファクトリ関数」で表現する設計。
//
// 使い方:
//   // 1) ファクトリ関数を書く (cpp 側で FNode2D の full type を include する)
//   static acs::TUniquePtr<acs::game::FNode2D> SpawnEnemy(void* /*user*/) noexcept {
//       auto n = acs::MakeUnique<acs::game::FNode2D>();
//       // 子ノード / コンポーネント / 初期値 ... を組み立てる
//       return n;
//   }
//
//   // 2) 登録
//   FPrefabSystem prefabs;
//   PrefabId enemy_id = prefabs.Register("Enemy", &SpawnEnemy);
//
//   // 3) spawn (Scene 側からは ID 経由 / Mod 側からは名前経由 が想定)
//   auto a = prefabs.Spawn(enemy_id);
//   auto b = prefabs.SpawnByName("Enemy");
//
// 設計選択 (Pillar J):
//   ・**`std::function` 不使用 / 関数ポインタ + `void* user_data`**: ACS 規約
//     (STL 不使用、heap 割り当てなしの callback)。closure を渡したい場合は
//     呼び出し側が context 構造体を `user_data` 経由で寄越す。
//   ・**`<string>` 禁止 / `const char*` で受ける**: 文字列の所有権はクライアント
//     側 (string literal か、別途寿命管理された永続バッファ)。FPrefabSystem は
//     ポインタを保管するだけで複製しない。これを忘れた使用は文字列が dangling
//     になり得るので、コメントで強く注意する。
//   ・**24bit idx + 8bit gen の packed handle**: `FNodeId` / `FShapeId` と完全に
//     同パターン。Unregister 後の slot 再利用で生まれる stale handle 検出に
//     generation を 1〜255 で循環させる (0 は「未使用 slot」予約)。
//   ・**slot 0 を invalid 予約**: `PrefabId{}` (= packed == 0) がそのまま
//     IsValid() == false になる。Register 時に必ず index >= 1 を返す。
//   ・**線形走査の `TArray<PrefabEntry>`**: 想定登録件数は数十〜数百 (1 タイトル
//     の prefab 数)、Register/Find は load 時に集中して走るので O(N) で十分。
//     ハッシュ化は実用上ボトルネックになった時点で検討。
//   ・**FNode2D は forward declare**: `TUniquePtr<FNode2D>` のメンバ宣言には full
//     type は不要 (TUniquePtr の宣言上の forward 互換、`.cpp` 側は触らない)。
//     factory 関数の中身は呼び出し側 cpp が `FNode2D.h` を include する責務。
//   ・**非コピー / 非ムーブ**: 登録された factory ポインタを別所有者に渡す事故
//     を排除。プロジェクト中 FPrefabSystem は通常 Scene/FGame に 1 個。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"

namespace acs::game {

// FNode2D の full type はこのヘッダでは不要 (TUniquePtr<FNode2D> の宣言として
// 触るだけ)。factory 関数を実装する cpp 側で `gameframework/FNode2D.h` を include すること。
class FNode2D;

/**
 * Prefab を識別する packed 32bit handle (generational)。
 *
 * @details
 * layout は low24 = index、high8 = generation。m_Packed == 0 が invalid。
 * FNodeId / FShapeId と完全に同じパターン。
 */
struct PrefabId {
    /** index (low24) と generation (high8) を詰めた 32bit 値。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な PrefabId を構築する。 */
    constexpr PrefabId() noexcept = default;

    /**
     * index と generation から PrefabId を構築する。
     *
     * @param index 24bit のスロット index。
     * @param gen 8bit の generation。
     */
    constexpr PrefabId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * スロット index を取り出す。
     *
     * @return low24 の index。
     */
    constexpr u32 Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を取り出す。
     *
     * @return high8 の generation。
     */
    constexpr u8  Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * handle が invalid でないかを返す。
     *
     * @details m_Packed == 0 のみ invalid。登録済 slot に存在するかは FPrefabSystem 側で別途検証する。
     * @return invalid でなければ true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 2 つの PrefabId が同一かを比較する。
     *
     * @param o 比較対象の PrefabId。
     * @return packed 値が等しければ true。
     */
    constexpr bool operator==(PrefabId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 2 つの PrefabId が異なるかを比較する。
     *
     * @param o 比較対象の PrefabId。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(PrefabId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * Prefab を実体化するファクトリ関数の signature。
 *
 * @details
 * user_data は Register 時に渡されたコンテキストポインタ (closure 代替)。
 * 返り値は新規 FNode2D ツリーの所有権で、失敗時は空 TUniquePtr (= bool == false) を返してよい。
 */
using PrefabFactoryFn = TUniquePtr<FNode2D>(*)(void* user_data) noexcept;

/**
 * 名前付き Prefab レジストリ。
 *
 * @details
 * テンプレート (= factory 関数) を登録し、ID または名前から FNode2D ツリーを spawn する。
 * 1 セッション内で通常数十〜数百件の登録を想定し、線形走査ベース。登録された factory
 * ポインタの所有移譲を抑止するため non-copy / non-move。
 */
class FPrefabSystem {
public:
    /** 空のレジストリを構築する。 */
    FPrefabSystem() noexcept = default;

    /** レジストリを破棄する (factory ポインタは保管するだけなので解放処理はない)。 */
    ~FPrefabSystem() noexcept = default;

    /** コピー禁止 (登録された factory ポインタの所有移譲を抑止)。 */
    FPrefabSystem(const FPrefabSystem&)            = delete;

    /** コピー代入も禁止。 */
    FPrefabSystem& operator=(const FPrefabSystem&) = delete;

    /** ムーブ禁止。 */
    FPrefabSystem(FPrefabSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FPrefabSystem& operator=(FPrefabSystem&&)      = delete;

    /**
     * 新規 Prefab を登録して PrefabId を返す。
     *
     * @details
     * name は永続文字列を渡すこと (string literal か別バッファ管理)。FPrefabSystem は複製せず
     * ポインタを保管する。nullptr / 空文字 / factory == nullptr は弾いて invalid を返す。
     * 同名既存があっても新規 entry を作って別 ID を返す (上書きしない。FindByName は登録順で
     * 最初に見つけたものを返す)。
     * @param name Prefab 名 (永続文字列、複製されない)。
     * @param factory Prefab を実体化するファクトリ関数。
     * @param user_data factory に渡すコンテキストポインタ (既定 nullptr)。
     * @return 登録した Prefab の PrefabId (バリデーション失敗時は invalid)。
     */
    PrefabId Register(const char* name, PrefabFactoryFn factory, void* user_data = nullptr) noexcept;

    /**
     * 名前で Prefab を検索する (登録順で最初に一致したもの)。
     *
     * @param name 検索する Prefab 名。
     * @return 一致した Prefab の PrefabId (一致しない / name == nullptr なら invalid)。
     */
    PrefabId FindByName(const char* name) const noexcept;

    /**
     * ID から FNode2D ツリーを spawn する (factory を 1 回呼ぶ)。
     *
     * @param id spawn する Prefab の PrefabId。
     * @return 生成した FNode2D ツリー (id が invalid / stale / factory が nullptr なら空 TUniquePtr)。
     */
    TUniquePtr<FNode2D> Spawn(PrefabId id) noexcept;

    /**
     * 名前から FNode2D ツリーを spawn する (内部で FindByName → Spawn 相当)。
     *
     * @param name spawn する Prefab 名。
     * @return 生成した FNode2D ツリー (見つからない場合は空 TUniquePtr)。
     */
    TUniquePtr<FNode2D> SpawnByName(const char* name) noexcept;

    /**
     * 登録を解除する。
     *
     * @details slot を再利用可能にし、generation を +1 して既存 handle を stale 化する。
     * @param id 解除する Prefab の PrefabId。
     * @return 実際に解除したら true、id が invalid / stale なら false (no-op)。
     */
    bool Unregister(PrefabId id) noexcept;

    /**
     * 現在 active な (= Unregister されていない) 登録数を返す。
     *
     * @return active な登録数。
     */
    u32 Count() const noexcept { return m_ActiveCount; }

    /**
     * デバッグ用に Prefab 名を取得する。
     *
     * @details 呼び出し側が条件分岐せずログにそのまま流せるよう nullptr は返さない。
     * @param id 名前を取得する Prefab の PrefabId。
     * @return Prefab 名 (invalid / stale なら "(unknown)")。
     */
    const char* GetName(PrefabId id) const noexcept;

    /** 全登録を破棄する (既存の ID は全て stale 化される)。 */
    void ClearAll() noexcept;

private:
    /**
     * 1 件の Prefab 登録を表すスロット。
     *
     * @details m_Entries 内に index 順で並び、index 0 は invalid 予約用の dummy。
     */
    struct PrefabEntry {
        /** Prefab 名 (永続文字列へのポインタ、複製しない)。 */
        const char*     name      = nullptr;

        /** Prefab を実体化するファクトリ関数。 */
        PrefabFactoryFn factory   = nullptr;

        /** factory に渡すコンテキストポインタ。 */
        void*           user_data = nullptr;

        /** generation (0 = 未使用、1〜255 で循環し stale handle を検出)。 */
        u8              gen       = 0;

        /** スロットが現在登録中かどうか。 */
        bool            active    = false;
    };

    /**
     * 未使用 slot を探して index を返す。
     *
     * @details 空きが無ければ末尾に push する。index 0 は invalid 予約なので返さない (最低 index 1)。
     * @return 確保したスロットの index。
     */
    u32 AcquireSlot() noexcept;

    /** 登録スロットの配列 (index 0 は dummy)。 */
    TArray<PrefabEntry> m_Entries;

    /** 現在 active な登録数。 */
    u32                m_ActiveCount = 0;
};

} // namespace acs::game
