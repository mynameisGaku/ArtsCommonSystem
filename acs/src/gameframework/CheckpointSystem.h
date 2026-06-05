// SPDX-License-Identifier: Apache-2.0
// GameFramework Genre Kit (Platformer) — FCheckpointSystem
//
// プラットフォーマー (and 派生ジャンル) の心臓部である「チェックポイント =
// 死亡時に戻る復活ポイント」を 1 クラスにまとめた小型マネージャ。
// 配置済み checkpoint を string id で識別し、現在 active な checkpoint を
// 1 つ保持して TriggerRespawn() で復活先座標 + level index を引き出す。
//
// 想定する位置付け:
//   ・Pillar R/I (FHealthSystem) との連携:
//     - FHealthSystem の DeathCallback で「死亡 → FCheckpointSystem.TriggerRespawn」
//       を叩くのが定型パターン。FCheckpointSystem 自体は HP の状態は持たず、
//       「どこに復活させるか」だけを管理する責務に絞っている。
//   ・Pillar S (FSaveSlot) との連携:
//     - 「現在 active な checkpoint id + unlocked id 群」をセーブする想定。
//       本クラス自体は I/O を持たず、外部から照会される。
//   ・FProgression / FEconomyDirector との違い:
//     - 進行系の累計値 (XP / 通貨) ではなく、ステージ内の「リスポーン拠点」を
//       1 つだけ active に保つ単純な座標マネージャ。
//
// 使い方:
//   FCheckpointSystem cps;
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
//   // 死亡時 (FHealthSystem の DeathCallback 内)
//   acs::FVec2 pos; u32 lv;
//   if (cps.TriggerRespawn(pos, lv)) {
//       // pos に player を移動、lv のレベルをロードし直す
//   }
//
// 設計選択:
//   ・**CheckpointId は 24bit idx + 8bit gen の packed u32**: FHealthId / PickupId
//     / FShapeId / FNodeId と同パターン。Unregister 後の slot 再利用でも古い
//     handle は generation 不一致で弾かれる。0 は invalid 予約 (index 0 dummy)。
//   ・**string id を主キーに**: gameplay 設計者が触る Trigger アクタは「Activate
//     先 checkpoint id」を文字列で指定するのが一般的 (FTilemap / Unity prefab の
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
//     は false 返却で no-op。Unlock 状態は別 TArray<const char*> で保持し、
//     ClearAll で初期化される (Save/Load 連携は外部から照会可能)。
//   ・**active checkpoint は 1 つだけ**: 「複数同時 active」は本クラスの責務外
//     (= ジャンルキットを超える概念)。最新の Activate が常に勝つ。
//   ・**FCallback は関数ポインタ + user**: FProgression / FHealthSystem と同パターン。
//     Activate / Respawn の 2 系統を用意し、UI トースト演出と sound trigger を
//     ゲーム側で素直に分離できるようにする。
//   ・**LastSpawnLevelIndex**: TriggerRespawn の後でも level_index を取れる
//     ように getter を分離。レベルロード中に out_param が消えるエッジで使う。
//   ・**全 noexcept、非コピー・非ムーブ**: FGame / Scene 単位で 1 個保持される
//     想定。Save/Load も id ベースで再現するので所有権移動は要らない。
//   ・**STL 不使用、<string> 禁止**: ACS 全体方針。文字列は const char* 非所有のみ。
//
// 範囲外 (将来 Phase で):
//   ・ボス挑戦専用 checkpoint (HP 回復 + 敵リセット) は FGameFlow と組み合わせて
//     ゲーム側で表現する。本クラスは座標 + level index のみ。
//   ・FSaveSlot 経由の永続化は外部 (FSaveSlot<PlatformerSaveData>) で実装。
//     本クラスからは CurrentCheckpoint() / IsUnlocked() で id を引き出すだけ。
//   ・自動 checkpoint (移動量で勝手に発火) は Trigger 側の設計問題なので
//     FCinematicsDirector / FTriggerWorld2D 側で組む。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * チェックポイントの generational handle。
 *
 * @details
 * 32bit packed = 24bit index + 8bit generation。0 = invalid。Unregister 後に slot
 * が再利用されても、古い handle は generation 不一致で弾かれる。FHealthId / PickupId
 * / FShapeId / FNodeId と同パターン。
 */
struct CheckpointId {
    /** packed 表現 (下位 24bit = index、上位 8bit = generation、0 で invalid)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な handle を構築する。 */
    constexpr CheckpointId() noexcept = default;

    /**
     * index と generation から handle を構築する。
     *
     * @param index slot インデックス (下位 24bit に格納)。
     * @param gen generation (上位 8bit に格納)。
     */
    constexpr CheckpointId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot インデックスを返す。
     *
     * @return 下位 24bit の index。
     */
    constexpr u32  Index()      const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation を返す。
     *
     * @return 上位 8bit の generation。
     */
    constexpr u8   Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    constexpr bool IsValid()    const noexcept { return m_Packed != 0; }

    /**
     * 等値比較する。
     *
     * @param o 比較対象の handle。
     * @return packed 表現が一致すれば true。
     */
    constexpr bool operator==(CheckpointId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等値比較する。
     *
     * @param o 比較対象の handle。
     * @return packed 表現が一致しなければ true。
     */
    constexpr bool operator!=(CheckpointId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * 1 つのチェックポイントの定義。
 *
 * @details id は呼出側が保証する static lifetime の文字列リテラルを想定 (非所有)。
 */
struct CheckpointInfo {
    /** 検索 / Save/Load / トリガ参照のキー。文字列リテラル想定 (非所有)。 */
    const char* id              = nullptr;

    /** 復活時のプレイヤー世界座標。 */
    FVec2        spawn_pos       = FVec2::Zero();

    /** 復活時にロードするレベル / シーンの index (caller 解釈、同一レベル内は同値)。 */
    u32         level_index     = 0;

    /** レベル内での順序 (one_way 判定に使う)。小さいほど序盤、一意に振ることを推奨。 */
    u32         sort_order      = 0;

    /** true なら Activate 後、より sort_order の小さい checkpoint への Activate を弾く (戻れない)。 */
    bool        one_way         = false;

    /** true なら UnlockCheckpoint で明示 unlock するまで ActivateCheckpoint が no-op (隠し / DLC)。 */
    bool        requires_unlock = false;
};

/**
 * Activate 成功時に呼ばれる callback の型。
 *
 * @param user SetOnActivateCallback で渡したコンテキスト (Manager は所有しない)。
 * @param checkpoint_id active 化された checkpoint の id (登録時のリテラル)。
 * @param spawn_pos その checkpoint の復活座標。
 */
using ActivateCallback = void(*)(void* user, const char* checkpoint_id, FVec2 spawn_pos) noexcept;

/**
 * TriggerRespawn 成功時に呼ばれる callback の型。
 *
 * @param user SetOnRespawnCallback で渡したコンテキスト (Manager は所有しない)。
 * @param checkpoint_id 復活元 checkpoint の id (登録時のリテラル)。
 * @param spawn_pos 復活座標。
 */
using RespawnCallback  = void(*)(void* user, const char* checkpoint_id, FVec2 spawn_pos) noexcept;

/**
 * プラットフォーマー系の「死亡時に戻る復活ポイント」を管理する小型マネージャ。
 *
 * @details
 * 配置済み checkpoint を string id で識別し、現在 active な 1 つだけを保持して
 * TriggerRespawn() で復活先座標 + level index を引き出す。HP 状態は持たず
 * 「どこに復活させるか」のみを管理する責務に絞る (FHealthSystem の DeathCallback
 * から叩く想定)。one_way / requires_unlock のフラグを Activate 時に評価し、不正な
 * 遷移は false 返却で弾く。文字列は const char* 非所有、I/O は持たない。
 */
class FCheckpointSystem {
public:
    /** 空の状態で構築する (checkpoint なし、active なし)。 */
    FCheckpointSystem()  noexcept = default;

    /** 破棄する (非所有データのみ保持のため特別な後始末なし)。 */
    ~FCheckpointSystem() noexcept = default;

    /** コピー禁止 (FGame / Scene 単位で 1 個保持する想定)。 */
    FCheckpointSystem(const FCheckpointSystem&)            = delete;

    /** コピー代入も禁止。 */
    FCheckpointSystem& operator=(const FCheckpointSystem&) = delete;

    /** ムーブ禁止。 */
    FCheckpointSystem(FCheckpointSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FCheckpointSystem& operator=(FCheckpointSystem&&)      = delete;

    /**
     * checkpoint を 1 件登録する。
     *
     * @details 同 id の 2 重登録、および info.id == nullptr は invalid 返却で黙って弾く。
     * @param info 登録する checkpoint 定義 (内部 slot にコピーされる)。
     * @return 新しい CheckpointId (24bit idx + 8bit gen)。失敗時は invalid。
     */
    CheckpointId Register(const CheckpointInfo& info) noexcept;

    /**
     * checkpoint を解除する。
     *
     * @details
     * slot は再利用され generation が進む。invalid id は no-op。現 active を解除した
     * 場合は active を invalid 化する (以後 TriggerRespawn は false を返す)。
     * @param id 解除する checkpoint の handle。
     */
    void Unregister(CheckpointId id) noexcept;

    /**
     * 指定 id の checkpoint を active 化する (string 版)。
     *
     * @details
     * 既に同 id が active な場合は no-op だが成功扱い (callback は再発火しない)。
     * @param checkpoint_id active 化する checkpoint の id。
     * @return 切替成功なら true。未登録 / nullptr、requires_unlock 未 unlock、
     *         現 active が one_way かつ対象の sort_order がより小さい場合は false。
     */
    bool ActivateCheckpoint(const char* checkpoint_id) noexcept;

    /**
     * handle 経由で checkpoint を active 化する。
     *
     * @details 挙動は string 版と同じ。
     * @param id active 化する checkpoint の handle。
     * @return 切替成功なら true (条件は string 版と同じ)。
     */
    bool ActivateCheckpoint(CheckpointId id) noexcept;

    /**
     * requires_unlock な checkpoint を unlock する。
     *
     * @details
     * 既に unlocked / 未登録 / requires_unlock=false な id でも黙って受け付ける
     * (Save 復元が Register より先に来てもよい設計)。id == nullptr は no-op。
     * @param checkpoint_id unlock する checkpoint の id。
     */
    void UnlockCheckpoint(const char* checkpoint_id) noexcept;

    /**
     * 指定 id が unlock 済みかを返す。
     *
     * @details requires_unlock=false な定義は常に true を返す (= unlock 不要 = 常時 available)。
     * @param checkpoint_id 照会する checkpoint の id。
     * @return unlock 済み (または unlock 不要) なら true。nullptr / 未登録 id は false。
     */
    bool IsUnlocked(const char* checkpoint_id) const noexcept;

    /**
     * 現在 active な checkpoint の handle を返す。
     *
     * @return 現 active な CheckpointId (一度も Activate されていない / 解除済みなら invalid)。
     */
    CheckpointId CurrentCheckpoint() const noexcept;

    /**
     * 現在の復活先座標を返す。
     *
     * @return active な checkpoint の spawn_pos (active 無しなら FVec2::Zero())。
     */
    FVec2 CurrentSpawnPos() const noexcept;

    /**
     * 直近に active 化された checkpoint の level_index を返す。
     *
     * @details レベルロード中に out_param が消えるエッジで使う。
     * @return 直近の level_index (一度も Activate されていなければ 0)。
     */
    u32 LastSpawnLevelIndex() const noexcept;

    /**
     * 死亡時に呼んで復活先を取得する。
     *
     * @details active checkpoint がある場合のみ RespawnCallback が発火する。
     * @param out_pos 復活座標の書き出し先。
     * @param out_level_index 復活先 level_index の書き出し先。
     * @return active checkpoint があり書き出した場合 true、active 無しなら false。
     */
    bool TriggerRespawn(FVec2& out_pos, u32& out_level_index) const noexcept;

    /**
     * 登録済 checkpoint の総数を返す。
     *
     * @return active な slot の数。
     */
    u32 CheckpointCount() const noexcept;

    /**
     * unlocked リストの長さを返す。
     *
     * @return unlock 済み id の数。
     */
    u32 UnlockedCount()   const noexcept;

    /**
     * 指定 id の checkpoint 定義へのポインタを返す。
     *
     * @details 返却ポインタは Register / Unregister / ClearAll で無効化されうる。
     * @param checkpoint_id 探す checkpoint の id。
     * @return 見つかった定義へのポインタ (見つからなければ nullptr)。
     */
    const CheckpointInfo* FindCheckpoint(const char* checkpoint_id) const noexcept;

    /**
     * 有効な checkpoint を穴詰めした生バッファを返す。
     *
     * @details
     * 内部 Slot 配列は穴あきだが、本 API は穴を詰めて連続バッファとして返す。
     * 返却バッファは内部 m_Scratch を再利用するため、次の AllCheckpoints / Register
     * / Unregister / ClearAll で無効化される。
     * @param out_count 有効な checkpoint 数の書き出し先。
     * @return 先頭要素へのポインタ (0 件なら out_count=0)。
     */
    const CheckpointInfo* AllCheckpoints(u32& out_count) const noexcept;

    /**
     * Activate 成功時 callback を設定する。
     *
     * @details 既に active な checkpoint を Activate しても再発火しない (no-op 成功)。
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetOnActivateCallback(ActivateCallback cb, void* user) noexcept;

    /**
     * TriggerRespawn 成功時 callback を設定する。
     *
     * @param cb 設定する callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetOnRespawnCallback(RespawnCallback cb, void* user) noexcept;

    /**
     * 全 checkpoint / unlocked リスト / active 状態を消去する。
     *
     * @details callback 設定は維持する。
     */
    void ClearAll() noexcept;

private:
    /** 1 checkpoint slot (定義 + active フラグ + generation)。 */
    struct Slot {
        /** checkpoint 定義。 */
        CheckpointInfo info{};

        /** この slot が使用中かどうか。 */
        bool           active = false;

        /** 再利用検出用 generation。 */
        u8             gen    = 0;
    };

    /**
     * 未使用 slot を 1 つ確保して index を返す。
     *
     * @details index 0 は invalid 予約 (dummy)。空きがなければ末尾に追加する。
     * @return 確保した slot の index (1 以上)。
     */
    u32 AcquireSlot() noexcept;

    /**
     * id 文字列で m_Slots を線形検索する。
     *
     * @param id 探す checkpoint の id。
     * @return active かつ id 一致の最初の slot index (未検出 / nullptr は -1)。
     */
    isize FindIndexById(const char* id) const noexcept;

    /**
     * unlocked リストを線形検索する。
     *
     * @param id 探す checkpoint の id。
     * @return 一致する要素の index (未検出は -1)。
     */
    isize FindUnlockedIndex(const char* id) const noexcept;

    /**
     * handle から slot 参照を取得する。
     *
     * @param id 解決する handle。
     * @return 有効なら slot へのポインタ (無効 / generation 不一致なら nullptr)。
     */
    Slot*       FindSlot(CheckpointId id) noexcept;

    /**
     * handle から const slot 参照を取得する。
     *
     * @param id 解決する handle。
     * @return 有効なら slot への const ポインタ (無効 / generation 不一致なら nullptr)。
     */
    const Slot* FindSlot(CheckpointId id) const noexcept;

    /**
     * Activate の共通ロジック (id / handle 版が両方ここを通る)。
     *
     * @details requires_unlock と現 active の one_way を評価し、不正遷移を弾く。
     * @param slot_index active 化対象の slot index (呼出側で active と確認済み)。
     * @return active 化成功なら true、拒否されたら false。
     */
    bool ActivateInternal(u32 slot_index) noexcept;

    /** checkpoint slot 配列 (index 0 は dummy)。 */
    TArray<Slot>          m_Slots;

    /** unlock された checkpoint id (非所有)。 */
    TArray<const char*>   m_Unlocked;

    /** 現在 active な checkpoint の handle (invalid = 一度も Activate されていない)。 */
    CheckpointId         m_Current;

    /** 直近に active 化された level_index (active slot が消えても履歴として残す)。 */
    u32                  m_LastLevelIndex = 0;

    /** 直近に active 化された spawn_pos (active slot が消えても履歴として残す)。 */
    FVec2                 m_LastSpawnPos   = FVec2::Zero();

    /** active な checkpoint 数 (= active=true な m_Slots の個数)。 */
    u32                  m_CheckpointCount = 0;

    /** AllCheckpoints が穴を詰めて返すための一時バッファ (const 関数から触るため mutable)。 */
    mutable TArray<CheckpointInfo> m_Scratch;

    /** Activate 成功時 callback (nullptr で未設定)。 */
    ActivateCallback     m_OnActivate      = nullptr;

    /** Activate callback に渡すコンテキスト。 */
    void*                m_OnActivateUser = nullptr;

    /** TriggerRespawn 成功時 callback (nullptr で未設定)。 */
    RespawnCallback      m_OnRespawn       = nullptr;

    /** Respawn callback に渡すコンテキスト。 */
    void*                m_OnRespawnUser  = nullptr;
};

} // namespace acs::game
