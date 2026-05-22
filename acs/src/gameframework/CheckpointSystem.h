// SPDX-License-Identifier: Apache-2.0
// GameFramework Genre Kit (Platformer) — CheckpointSystem
//
// プラットフォーマー (and 派生ジャンル) の心臓部である「チェックポイント =
// 死亡時に戻る復活ポイント」を 1 クラスにまとめた小型マネージャ。
// 配置済み checkpoint を string id で識別し、現在 active な checkpoint を
// 1 つ保持して TriggerRespawn() で復活先座標 + level index を引き出す。
//
// 想定する位置付け:
//   ・Pillar R/I (HealthSystem) との連携:
//     - HealthSystem の DeathCallback で「死亡 → CheckpointSystem.TriggerRespawn」
//       を叩くのが定型パターン。CheckpointSystem 自体は HP の状態は持たず、
//       「どこに復活させるか」だけを管理する責務に絞っている。
//   ・Pillar S (SaveSlot) との連携:
//     - 「現在 active な checkpoint id + unlocked id 群」をセーブする想定。
//       本クラス自体は I/O を持たず、外部から照会される。
//   ・Progression / EconomyDirector との違い:
//     - 進行系の累計値 (XP / 通貨) ではなく、ステージ内の「リスポーン拠点」を
//       1 つだけ active に保つ単純な座標マネージャ。
//
// 使い方:
//   CheckpointSystem cps;
//
//   // レベルロード時に 1 度ずつ配置を登録。
//   CheckpointInfo cp1{};
//   cp1.id           = "cp.stage1.start";
//   cp1.spawn_pos    = { 100.0f, 200.0f };
//   cp1.level_index  = 0;
//   cp1.sort_order   = 0;
//   cp1.one_way      = false;
//   cp1.requires_unlock = false;
//   cps.Register(cp1);
//
//   CheckpointInfo cp2{};
//   cp2.id           = "cp.stage1.mid";
//   cp2.spawn_pos    = { 500.0f, 200.0f };
//   cp2.level_index  = 0;
//   cp2.sort_order   = 1;
//   cp2.one_way      = true;     // ここまで来たら戻れない
//   cp2.requires_unlock = false;
//   cps.Register(cp2);
//
//   CheckpointInfo cp_secret{};
//   cp_secret.id     = "cp.stage1.secret";
//   cp_secret.spawn_pos = { 900.0f, 100.0f };
//   cp_secret.level_index = 0;
//   cp_secret.sort_order = 2;
//   cp_secret.one_way = false;
//   cp_secret.requires_unlock = true;   // 隠しスイッチで Unlock 必要
//   cps.Register(cp_secret);
//
//   // 通常進行: トリガに触れたら ActivateCheckpoint("cp.stage1.mid") を呼ぶ
//   cps.ActivateCheckpoint("cp.stage1.start");
//   cps.ActivateCheckpoint("cp.stage1.mid");
//
//   // 隠し: スイッチを踏んだら Unlock してから Activate
//   cps.UnlockCheckpoint("cp.stage1.secret");
//   cps.ActivateCheckpoint("cp.stage1.secret");
//
//   // 死亡時 (HealthSystem の DeathCallback 内)
//   acs::Vec2 pos; u32 lv;
//   if (cps.TriggerRespawn(pos, lv)) {
//       // pos に player を移動、lv のレベルをロードし直す
//   }
//
// 設計選択:
//   ・**CheckpointId は 24bit idx + 8bit gen の packed u32**: HealthId / PickupId
//     / ShapeId / NodeId と同パターン。Unregister 後の slot 再利用でも古い
//     handle は generation 不一致で弾かれる。0 は invalid 予約 (index 0 dummy)。
//   ・**string id を主キーに**: gameplay 設計者が触る Trigger アクタは「Activate
//     先 checkpoint id」を文字列で指定するのが一般的 (Tilemap / Unity prefab の
//     文化と整合)。handle 経由の Activate も提供して両対応。
//   ・**所有しない const char***: id は呼出側 (リソースバンドル or ステージ
//     データ) が保証する static lifetime の文字列リテラルを想定。STL <string>
//     禁止方針で、Manager 内ではコピーしない。
//   ・**one_way フラグ**: 一度通った後の back-trigger を無効化する用途。
//     現在 active な checkpoint の sort_order より小さい checkpoint への
//     Activate を弾く。「ボス前で戻れなくする」「リバースワープ封じ」等。
//     sort_order は同レベル内で一意に振る前提 (重複は順序未定義)。
//   ・**requires_unlock フラグ**: 隠し checkpoint / DLC 後アンロックを表現。
//     UnlockCheckpoint で unlocked リストに id を追加するまで ActivateCheckpoint
//     は false 返却で no-op。Unlock 状態は別 Array<const char*> で保持し、
//     ClearAll で初期化される (Save/Load 連携は外部から照会可能)。
//   ・**active checkpoint は 1 つだけ**: 「複数同時 active」は本クラスの責務外
//     (= ジャンルキットを超える概念)。最新の Activate が常に勝つ。
//   ・**Callback は関数ポインタ + user**: Progression / HealthSystem と同パターン。
//     Activate / Respawn の 2 系統を用意し、UI トースト演出と sound trigger を
//     ゲーム側で素直に分離できるようにする。
//   ・**LastSpawnLevelIndex**: TriggerRespawn の後でも level_index を取れる
//     ように getter を分離。レベルロード中に out_param が消えるエッジで使う。
//   ・**全 noexcept、非コピー・非ムーブ**: Game / Scene 単位で 1 個保持される
//     想定。Save/Load も id ベースで再現するので所有権移動は要らない。
//   ・**STL 不使用、<string> 禁止**: ACS 全体方針。文字列は const char* 非所有のみ。
//
// 範囲外 (将来 Phase で):
//   ・ボス挑戦専用 checkpoint (HP 回復 + 敵リセット) は GameFlow と組み合わせて
//     ゲーム側で表現する。本クラスは座標 + level index のみ。
//   ・SaveSlot 経由の永続化は外部 (SaveSlot<PlatformerSaveData>) で実装。
//     本クラスからは CurrentCheckpoint() / IsUnlocked() で id を引き出すだけ。
//   ・自動 checkpoint (移動量で勝手に発火) は Trigger 側の設計問題なので
//     CinematicsDirector / TriggerWorld2D 側で組む。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// ---- CheckpointId: チェックポイントの handle --------------------------------
// 32bit packed = 24bit index + 8bit generation。0 = invalid。
// HealthId / PickupId / ShapeId / NodeId と同パターン。
struct CheckpointId {
    u32 _packed = 0;

    constexpr CheckpointId() noexcept = default;
    constexpr CheckpointId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index()      const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept { return static_cast<u8>(_packed >> 24); }
    constexpr bool IsValid()    const noexcept { return _packed != 0; }

    constexpr bool operator==(CheckpointId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(CheckpointId o) const noexcept { return _packed != o._packed; }
};

// ---- CheckpointInfo: 1 チェックポイントの定義 -------------------------------
// id              : 検索 / Save/Load / トリガ参照のキー。文字列リテラル想定 (非所有)。
// spawn_pos       : 復活時のプレイヤー世界座標。
// level_index     : 復活時にロードするレベル / シーンの index。SceneManager で
//                   切替えるための鍵 (caller 解釈)。同一レベル内なら全 checkpoint
//                   が同じ値を持つ。
// sort_order      : レベル内での順序 (one_way 判定で使う)。小さいほど序盤、
//                   大きいほど終盤。同一レベル内で一意に振ることを推奨。
// one_way         : true = この checkpoint を Activate した後、より sort_order
//                   の小さい checkpoint への Activate を弾く (戻れない)。
//                   ボス前 / 強制ワープ後 / カットシーン後等の使用を想定。
// requires_unlock : true = UnlockCheckpoint で明示 unlock するまで ActivateCheckpoint
//                   が false を返して no-op。隠し / DLC / 解放鍵連動を表現。
struct CheckpointInfo {
    const char* id              = nullptr;
    Vec2        spawn_pos       = Vec2::Zero();
    u32         level_index     = 0;
    u32         sort_order      = 0;
    bool        one_way         = false;
    bool        requires_unlock = false;
};

// ---- ActivateCallback / RespawnCallback ------------------------------------
// `void(*)(void* user, const char* checkpoint_id, acs::Vec2 spawn_pos) noexcept;`
// Activate 成功時 / Respawn 引き出し時にそれぞれ 1 回呼ばれる。
// user は SetOnXxxCallback で渡したコンテキスト (Manager は所有しない)。
// checkpoint_id は登録時の id (リテラル) がそのまま渡る。
using ActivateCallback = void(*)(void* user, const char* checkpoint_id, Vec2 spawn_pos) noexcept;
using RespawnCallback  = void(*)(void* user, const char* checkpoint_id, Vec2 spawn_pos) noexcept;

// ---- CheckpointSystem ------------------------------------------------------
class CheckpointSystem {
public:
    CheckpointSystem()  noexcept = default;
    ~CheckpointSystem() noexcept = default;

    CheckpointSystem(const CheckpointSystem&)            = delete;
    CheckpointSystem& operator=(const CheckpointSystem&) = delete;
    CheckpointSystem(CheckpointSystem&&)                 = delete;
    CheckpointSystem& operator=(CheckpointSystem&&)      = delete;

    // ---- 配置登録 / 解除 ------------------------------------------------
    // 同 id の 2 重登録は invalid 返却 (黙って弾く)。info.id == nullptr も同様。
    // 成功時は新しい CheckpointId を返す (24bit idx + 8bit gen)。
    CheckpointId Register(const CheckpointInfo& info) noexcept;

    // checkpoint を解除。slot は再利用 (generation 進む)。invalid id は no-op。
    // 現 active checkpoint を解除した場合は active を invalid 化する
    // (TriggerRespawn は false を返すようになる)。
    void Unregister(CheckpointId id) noexcept;

    // ---- Activate (現在地点として記録) ----------------------------------
    // 指定 id を active 化。戻り値 true = 切替成功。
    //   ・id == nullptr / 未登録: false
    //   ・requires_unlock かつ unlocked リスト未登録: false
    //   ・現 active の one_way が true で、対象の sort_order が現 active のより小さい: false
    //   ・既に同 id が active: true (no-op だが成功扱い、callback は再発火しない)
    bool ActivateCheckpoint(const char* checkpoint_id) noexcept;

    // handle 経由の Activate。挙動は string 版と同じ。
    bool ActivateCheckpoint(CheckpointId id) noexcept;

    // ---- Unlock (隠し / DLC ones) --------------------------------------
    // requires_unlock な checkpoint を unlock する。
    // 既に unlocked / 未登録 / requires_unlock=false な id でも黙って受け付ける
    // (Save 復元で「unlock しただけ残しておきたい」要件があるため厳格化しない)。
    // id == nullptr は no-op。
    void UnlockCheckpoint(const char* checkpoint_id) noexcept;

    // 指定 id が unlocked リストに入っているか。requires_unlock=false な定義の
    // 場合は常に true を返す (= unlock 不要 = 常時 available)。
    // id == nullptr / 未登録 id は false。
    bool IsUnlocked(const char* checkpoint_id) const noexcept;

    // ---- 現在 active な checkpoint の照会 -------------------------------
    // 現 active CheckpointId。一度も Activate されていない / 解除された場合 invalid。
    CheckpointId CurrentCheckpoint() const noexcept;

    // 死亡時の復活先座標。active 無しなら Vec2::Zero() を返す
    // (= caller は TriggerRespawn / CurrentCheckpoint.IsValid() で先に判定すべし)。
    Vec2 CurrentSpawnPos() const noexcept;

    // 直近に active 化された checkpoint の level_index。一度も Activate されて
    // いない場合は 0 を返す (caller は CurrentCheckpoint() で先に判定すべし)。
    u32 LastSpawnLevelIndex() const noexcept;

    // ---- 死亡時呼ぶ復活先取得 ------------------------------------------
    // 戻り値 true = active checkpoint あり、out_pos / out_level_index を書き出す。
    // false = active 無し (caller は「タイトル戻り」or「レベル先頭」を選ぶ)。
    // 設定された RespawnCallback は true 時のみ発火する。
    bool TriggerRespawn(Vec2& out_pos, u32& out_level_index) const noexcept;

    // ---- 件数照会 -------------------------------------------------------
    u32 CheckpointCount() const noexcept;  // 登録総数 (active な slot のみ)
    u32 UnlockedCount()   const noexcept;  // unlocked リストの長さ

    // ---- 単一情報照会 --------------------------------------------------
    // 見つからなければ nullptr。返却ポインタは Register / Unregister / ClearAll
    // で無効化される可能性がある。
    const CheckpointInfo* FindCheckpoint(const char* checkpoint_id) const noexcept;

    // ---- 全 checkpoint の生バッファ ------------------------------------
    // out_count に「有効な checkpoint 数」 (= 連続して並ぶ要素数) を書き出して
    // 返す。返却バッファは内部 _scratch 配列を再利用するため、次の AllCheckpoints
    // / Register / Unregister / ClearAll で無効化される。
    // (内部の Slot 配列は穴あきだが、本 API は穴を詰めて返す)
    const CheckpointInfo* AllCheckpoints(u32& out_count) const noexcept;

    // ---- Callback ------------------------------------------------------
    // Activate 成功時 callback。cb == nullptr で解除。
    // 既に active な checkpoint を Activate しても再発火はしない (no-op 成功)。
    void SetOnActivateCallback(ActivateCallback cb, void* user) noexcept;

    // TriggerRespawn 成功時 callback。cb == nullptr で解除。
    void SetOnRespawnCallback(RespawnCallback cb, void* user) noexcept;

    // ---- 一括破棄 ------------------------------------------------------
    // 全 checkpoint / unlocked リスト / active 状態を消去。callback 設定は維持。
    void ClearAll() noexcept;

private:
    struct Slot {
        CheckpointInfo info{};
        bool           active = false;
        u8             gen    = 0;
    };

    // 未使用 slot を 1 つ確保し index を返す。index 0 は予約 (= invalid)。
    u32 AcquireSlot() noexcept;

    // id 文字列で _slots を線形検索。未検出は -1。id == nullptr も -1。
    // 戻り値は _slots への index (active && id 一致を満たす最初の要素)。
    isize FindIndexById(const char* id) const noexcept;

    // unlocked リストで線形検索。未検出は -1。
    isize FindUnlockedIndex(const char* id) const noexcept;

    // 内部アクセサ: id が有効なら slot 参照を返し、無効なら nullptr。
    Slot*       FindSlot(CheckpointId id) noexcept;
    const Slot* FindSlot(CheckpointId id) const noexcept;

    // Activate 共通ロジック (id / handle 版が両方ここを通る)。
    bool ActivateInternal(u32 slot_index) noexcept;

    Array<Slot>          _slots;
    Array<const char*>   _unlocked;       // unlock された checkpoint id (非所有)

    // 現在 active な checkpoint の handle。invalid = 一度も Activate されていない。
    CheckpointId         _current;

    // 直近に active 化された level_index と spawn_pos。Unregister で active slot
    // が消えても直近値として残しておく (デバッグ表示用)。
    u32                  _last_level_index = 0;
    Vec2                 _last_spawn_pos   = Vec2::Zero();

    // active な checkpoint 数 (= active=true な _slots の個数)。
    u32                  _checkpoint_count = 0;

    // AllCheckpoints が穴を詰めて返すための一時バッファ (mutable で const 関数から触る)。
    mutable Array<CheckpointInfo> _scratch;

    ActivateCallback     _on_activate      = nullptr;
    void*                _on_activate_user = nullptr;
    RespawnCallback      _on_respawn       = nullptr;
    void*                _on_respawn_user  = nullptr;
};

} // namespace acs::game
