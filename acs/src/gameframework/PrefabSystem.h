// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J Phase 2 — FPrefabSystem
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
//   FPrefabId enemy_id = prefabs.Register("Enemy", &SpawnEnemy);
//
//   // 3) spawn (FScene 側からは ID 経由 / Mod 側からは名前経由 が想定)
//   auto a = prefabs.Spawn(enemy_id);
//   auto b = prefabs.SpawnByName("Enemy");
//
// 設計選択 (Phase 2 = Pillar J Phase 2):
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
//   ・**slot 0 を invalid 予約**: `FPrefabId{}` (= packed == 0) がそのまま
//     IsValid() == false になる。Register 時に必ず index >= 1 を返す。
//   ・**線形走査の `TArray<FPrefabEntry>`**: 想定登録件数は数十〜数百 (1 タイトル
//     の prefab 数)、Register/Find は load 時に集中して走るので O(N) で十分。
//     ハッシュ化は実用上ボトルネックになった時点で検討。
//   ・**FNode2D は forward declare**: `TUniquePtr<FNode2D>` のメンバ宣言には full
//     type は不要 (TUniquePtr の宣言上の forward 互換、`.cpp` 側は触らない)。
//     factory 関数の中身は呼び出し側 cpp が `Node2D.h` を include する責務。
//   ・**非コピー / 非ムーブ**: 登録された factory ポインタを別所有者に渡す事故
//     を排除。プロジェクト中 FPrefabSystem は通常 FScene/FGame に 1 個。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"

namespace acs::game {

// FNode2D の full type はこのヘッダでは不要 (TUniquePtr<FNode2D> の宣言として
// 触るだけ)。factory 関数を実装する cpp 側で `gameframework/Node2D.h` を include すること。
class FNode2D;

/// Prefab を識別する packed 32bit handle (generational)。
/// layout: low24 = index, high8 = generation。`_packed == 0` が invalid。
/// `FNodeId` / `FShapeId` と完全に同じパターン (Phase 0 codify 済の規約)。
struct FPrefabId {
    u32 _packed = 0;

    constexpr FPrefabId() noexcept = default;
    constexpr FPrefabId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32 Index() const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8  Generation() const noexcept {
        return static_cast<u8>(_packed >> 24);
    }
    /// 0 = invalid。「登録済 slot に存在するか」は FPrefabSystem 側で別途検証する。
    bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(FPrefabId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(FPrefabId o) const noexcept { return _packed != o._packed; }
};

/// Prefab を実体化するファクトリ関数の signature。
/// `user_data` は Register 時に渡されたコンテキストポインタ (closure 代替)。
/// 返り値は新規 FNode2D ツリーの所有権。失敗時は空 `TUniquePtr` (= bool == false) を返してよい。
using PrefabFactoryFn = TUniquePtr<FNode2D>(*)(void* user_data) noexcept;

/// 名前付き Prefab レジストリ。テンプレート (= factory) を登録 → ID で spawn する。
/// 1 セッション内で通常数十〜数百件の登録を想定。線形走査ベース。
class FPrefabSystem {
public:
    FPrefabSystem() noexcept = default;
    ~FPrefabSystem() noexcept = default;

    // 非コピー / 非ムーブ: 登録された factory ポインタの所有移譲を抑止。
    FPrefabSystem(const FPrefabSystem&)            = delete;
    FPrefabSystem& operator=(const FPrefabSystem&) = delete;
    FPrefabSystem(FPrefabSystem&&)                 = delete;
    FPrefabSystem& operator=(FPrefabSystem&&)      = delete;

    /// 新規 Prefab を登録して `FPrefabId` を返す。
    /// `name` は永続文字列を渡すこと (string literal か別バッファ管理)、
    /// FPrefabSystem は複製せずポインタを保管する。`nullptr` や空文字、
    /// `factory == nullptr` は弾いて invalid を返す。
    /// 同名既存があっても新規 entry を作って別 ID を返す (上書きしない、
    /// FindByName は登録順で最初に見つけたものを返す)。
    FPrefabId Register(const char* name, PrefabFactoryFn factory, void* user_data = nullptr) noexcept;

    /// 名前で検索 (登録順で最初に一致したもの)。
    /// 一致しない / `name == nullptr` の時は invalid FPrefabId を返す。
    FPrefabId FindByName(const char* name) const noexcept;

    /// ID から FNode2D ツリーを spawn (factory を 1 回呼ぶ)。
    /// `id` が invalid / stale / factory が nullptr の場合は空 `TUniquePtr` を返す。
    TUniquePtr<FNode2D> Spawn(FPrefabId id) noexcept;

    /// 名前から spawn。内部で FindByName → Spawn と同等。
    TUniquePtr<FNode2D> SpawnByName(const char* name) noexcept;

    /// 登録解除。slot を再利用、generation を +1 して stale handle を無効化。
    /// id が invalid / stale なら no-op、`true`/`false` で実際に解除したかを返す。
    bool Unregister(FPrefabId id) noexcept;

    /// 現在 active な (= Unregister されていない) 登録数。
    u32 Count() const noexcept { return _active_count; }

    /// デバッグ用の名前取得。invalid / stale なら `"(unknown)"` を返す
    /// (呼び出し側が条件分岐せずログにそのまま流せるよう nullptr は返さない)。
    const char* GetName(FPrefabId id) const noexcept;

    /// 全登録を破棄。ID は全て stale 化される。
    void ClearAll() noexcept;

private:
    struct FPrefabEntry {
        const char*     name      = nullptr;
        PrefabFactoryFn factory   = nullptr;
        void*           user_data = nullptr;
        u8              gen       = 0;     // 0 = 未使用、1〜255 で循環
        bool            active    = false;
    };

    /// 未使用 slot を探して index を返す。空きが無ければ末尾に push。
    /// index 0 は invalid 予約なので返さない (最低 index 1)。
    u32 AcquireSlot() noexcept;

    TArray<FPrefabEntry> _entries;
    u32                _active_count = 0;
};

} // namespace acs::game
