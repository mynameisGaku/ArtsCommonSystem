// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B Phase 4 — FNodePool (FNode2D の generational pool)
//
// シーン全体で唯一の `FNode2D*` レジストリ。`FNode2D` インスタンス自体は親の
// `_children` (TUniquePtr<FNode2D>) が所有し続け、本 pool は **参照のみ** を
// 保持して安定した `FNodeId` を発行する。同時に発行済 `FNodeId` の **stale 検出**
// (= 既に Unregister されたハンドル) を提供する。
//
// 使い方:
//   FNodePool pool;
//   pool.Init(/*initial_capacity=*/512);
//
//   // FNode2D を新規生成して scene tree に attach した直後に登録:
//   FNode2D* enemy_ptr = scene.Root().AddChild(MakeUnique<EnemyNode>()).Get();
//   FNodeId enemy_id = pool.RegisterExistingNode(enemy_ptr);
//   // enemy_ptr->Id() == enemy_id が成立する (RegisterExistingNode 内で _SetId 済)
//
//   // 後で stale 検査付きで取り出し:
//   if (FNode2D* p = pool.Get(enemy_id)) {
//       p->Local().position += FVec2{1, 0};
//   }
//
//   // 破棄時:
//   pool.Unregister(enemy_id);   // slot 解放 + gen++ → 古い handle は invalid 化
//   enemy_ptr->Destroy();        // scene tree からは別途 reap される
//
// 設計選択 (Phase 4 = Pillar B Phase 4):
//   ・**non-owning**: 所有権は FNode2D の親 (=TUniquePtr<FNode2D>) 側にあり、本 pool
//     は raw ポインタだけ持つ。TPool の破棄や ClearAll は FNode2D を delete しない。
//   ・**Slot = {ptr, gen, active}**: FCollisionWorld2D::Slot と同じパターン。
//     index 0 は予約 (= invalid handle と一致させる)、有効 slot は 1..N。
//   ・**free_indices stack**: 空き slot を O(1) で再利用。Unregister 時 push、
//     RegisterExistingNode 時 pop。stack が空なら slot を新規 PushBack。
//   ・**generation 0 はスキップ**: gen++ がラップアラウンドで 0 に戻った場合、
//     FNodeId(idx, 0) は IsValid() == false になってしまうため、ラップ時は 1 に
//     強制する (FCollisionWorld2D と完全に同じ挙動)。
//   ・**24bit index = 16,777,216 slot 上限**: FNodeId の pack 仕様に従い、これを
//     超える RegisterExistingNode は invalid FNodeId を返す (拒否)。実用上 1 シーン
//     で 16M Node を生成することはまずあり得ないが安全策として明示拒否。
//   ・**IdOf は線形探索**: ポインタ → FNodeId の逆引きは利用頻度が低い (基本は
//     RegisterExistingNode の戻り値を保持する) ため、専用 hash は持たない。
//     真に必要なら呼び出し側で THashMap<FNode2D*, FNodeId> を別途持てば良い。
//   ・**非コピー・非ムーブ**: pool 自体は固定オブジェクトとして scene が所有する想定。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約に厳格準拠。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class FNode2D;   // forward decl — full include は .cpp 側 (FNode2D::_SetId 呼出のため)

/// FNode2D 群を pool で管理し、安定した FNodeId を発行 + stale 検出する。
/// 所有権は持たない (FNode2D は親の TUniquePtr が所有)。
class FNodePool {
public:
    FNodePool()  noexcept = default;
    ~FNodePool() noexcept = default;

    FNodePool(const FNodePool&)            = delete;
    FNodePool& operator=(const FNodePool&) = delete;
    FNodePool(FNodePool&&)                 = delete;
    FNodePool& operator=(FNodePool&&)      = delete;

    /// 初期容量を予約 (再 alloc 回避用)。複数回呼出可、縮小はしない。
    /// initial_capacity 0 や負の値はそのまま受けて、初回 Register まで何もしない。
    void Init(u32 initial_capacity = 256) noexcept;

    /// 既存の生 FNode2D を pool に登録、新しい FNodeId を発行して node->_SetId する。
    /// node == nullptr、または slot 数が 16M に達した場合は invalid FNodeId を返す
    /// (この場合 node の Id は変更しない)。
    FNodeId RegisterExistingNode(FNode2D* node) noexcept;

    /// slot を free 化し、対応 FNode2D の Id を invalid にリセット、generation を進める。
    /// 既に invalid / stale な id は何もしない (二重 Unregister は安全)。
    void Unregister(FNodeId id) noexcept;

    /// id が指す slot が active かつ generation が一致するか。
    bool IsValid(FNodeId id) const noexcept;

    /// id 経由で FNode2D* を取り出す。stale / invalid なら nullptr。
    FNode2D* Get(FNodeId id) const noexcept;

    /// node ポインタから FNodeId を逆引き (線形探索 O(N))。
    /// node == nullptr、または pool に存在しなければ invalid FNodeId を返す。
    FNodeId IdOf(FNode2D* node) const noexcept;

    /// 現在 active な slot 数。
    u32 ActiveCount() const noexcept { return _active_count; }

    /// 現在の slot 数 (内部配列長 - 1、index 0 予約分を除外)。
    /// 物理的に確保された FNode2D 個数の上限ではなく、過去に到達した最大値の指標。
    u32 Capacity() const noexcept {
        const u32 sz = static_cast<u32>(_slots.Size());
        return sz > 0 ? sz - 1u : 0u;   // index 0 予約分を引く
    }

    /// 全 slot を一括 free。各登録済 FNode2D の Id を invalid にリセットする。
    /// FNode2D 自体は削除しない (所有していないため)。free_indices stack もクリア。
    /// gen は維持する (= 同じ slot を再利用した時、ClearAll 前の handle は確実に stale)。
    void ClearAll() noexcept;

private:
    struct Slot {
        FNode2D* ptr    = nullptr;   // 非所有 (FNode2D の所有は親 TUniquePtr 側)
        u8      gen    = 0;          // 0 は予約 (= invalid handle と一致)。有効 slot は 1..255
        bool    active = false;
    };

    /// 空き slot を 1 つ取得 (free stack → 末尾追加の順)。index 0 は予約。
    /// 16M 上限に到達した場合は 0 を返す (= 呼出側で invalid FNodeId 化)。
    u32 AcquireSlot() noexcept;

    /// 24bit index 上限 (FNodeId pack 仕様に合わせる)。
    static constexpr u32 kMaxIndex = 0x00FFFFFFu;   // = 16,777,215

    TArray<Slot> _slots;          // index 0 は dummy (= invalid 用予約)
    TArray<u32>  _free_indices;   // LIFO stack。pop → 再利用 slot、empty → 末尾追加
    u32         _active_count = 0;
};

} // namespace acs::game
