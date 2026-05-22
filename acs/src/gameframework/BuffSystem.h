// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R / I — BuffSystem (状態異常 / バフ / デバフ管理)
//
// RPG / アクション / ローグライク等で頻出する「複数 owner (= キャラ) に対して
// 複数の時間制限付き効果 (バフ / デバフ) を載せて、tick で進行 + 期限切れ
// 通知をする」マネージャ。`EffectSystem` (Pillar I Phase 1) が「画面の Flash /
// Shake のような視覚演出」だったのに対し、本クラスは「キャラに紐付くゲームロジ
// ック上の状態」を担当する。実際のステータス計算 (= AttackUp バフが攻撃力に
// 何倍を掛けるか) は呼出側 (= キャラクタコンポーネント) が `AllBuffsOfOwner()`
// で列挙して自分で行う設計 — 本クラスは「いつ、どの owner に、どの buff が、
// 何 stack あるか、残り何秒か」だけを真実として保持する。
//
// 設計選択 (Phase R/I — BuffSystem):
//   ・**BuffOwnerId は 24bit index + 8bit gen の packed handle**:
//     `NodeId` / `EmitterHandle` / `TimerHandle` と同一規約。`_packed == 0` を
//     invalid とし、gen は常に 1 以上で配る (0 は「未使用 slot」を意味する)。
//     これにより DestroyOwner 後の stale handle を gen 不一致で確実に弾ける。
//   ・**owner ごとに sparse な buff 配列**: OwnerSlot 内に `Array<BuffInstance>`
//     を持つ。owner 数は数百、各 owner の buff 数は通常 1〜10 程度を想定。
//     線形検索で十分。SoA にして「全 owner 横断で同じ buff_id を集める」用途は
//     現状無いので、AoS の単純さを優先。
//   ・**registry は `Array<BuffDef>`**: BuffDef はゲーム起動時に一括 Register
//     される静的データ想定。id は const char* で文字列リテラルを参照する想定
//     (`<string>` 禁止、所有しない、長寿命を caller が保証)。
//   ・**BuffStackPolicy 3 種**:
//       - Refresh : 既存があれば remaining_sec を duration_sec で上書き (stack は据置)。
//                   毒系の「重ねがけで時間延長」のような感覚。
//       - Stack   : 既存があれば stack++ (max_stack で clamp)、remaining_sec も
//                   reset (= 強化系で重ねたら最新タイマで進める方が UX 上自然)。
//       - Ignore  : 既存があれば何もしない (= 「最初の 1 枚しか効かない」系)。
//     どれを採るかはバフ定義ごとに固定 (= ApplyBuff 呼出側の都合で変える物では無い)。
//   ・**tick_interval_sec > 0 で tick callback を発火**: Regen / Poison / Burn の
//     ように「N 秒ごとに HP を増減する」用途。tick_interval_sec <= 0 のバフは
//     「一発掛けっぱなしで終わるまで持続」(= AttackUp / DefenseUp / Shield 等)
//     と解釈し、tick callback は発火しない。
//   ・**callback は 1 個固定 + user pointer**: STL `<functional>` 禁止のため、
//     C 関数ポインタ + void*。複数 listener が必要なら呼出側で fan-out。
//     tick / expire は別々の callback を持ち、それぞれ独立に attach/detach 可能。
//     callback は内部 mutation 中 (Tick 内) に発火するので、コールバック中に
//     ApplyBuff / RemoveBuff / DestroyOwner を呼ぶのは非推奨 (= UB の温床)。
//     必要なら呼出側で「あとで実行するキュー」を持ってフレーム境界で処理すること。
//   ・**Tick は dt > 0 のみ進める**: 負 dt / 0 dt は no-op。dt が大きい場合
//     (= フレーム droplet 等) でも tick_interval_sec を複数回踏むことがあり、
//     その回数分 callback を発火する (= 1 フレで 3 回毒ダメージが入ることもある)。
//     これは「frame skip でダメージが消える」より「正しく被弾する」方が
//     ゲームロジック上素直という判断。
//   ・**expire は Tick の最後にまとめて発火**: ループ中に Array を圧縮すると
//     インデックスがずれて bug の温床になる。残寿命 <= 0 になった buff を
//     swap-and-pop で除去しつつ、その buff の id を一時バッファに記録 → 全
//     除去完了後にコールバックを呼ぶ流れ。コールバック中に owner や buff が
//     変化しても安全。
//   ・**非コピー・非ムーブ**: 内部 Array<OwnerSlot> がさらに Array<BuffInstance>
//     を持つ二段ネスト構造で、ポインタ参照や AllBuffsOfOwner で生バッファを
//     返す API があるため。ムーブで実体アドレスが変わると外部参照が破綻する。
//   ・**全 noexcept、STL 不使用、`<string>` 禁止**: ACS 規約。失敗は bool / 哨兵で表現。
//
// 使い方:
//   BuffSystem bs;
//
//   // 1) バフ定義を起動時に一括登録 (id は文字列リテラル想定)
//   bs.RegisterBuff({
//       /*id*/             "poison.snake",
//       /*kind*/           BuffKind::Poison,
//       /*duration_sec*/   8.0f,
//       /*tick_interval*/  1.0f,        // 1 秒ごとに tick callback
//       /*magnitude*/      5.0f,        // 1 tick で 5 ダメージ
//       /*stack_policy*/   BuffStackPolicy::Stack,
//       /*max_stack*/      5,
//       /*is_debuff*/      true,
//   });
//
//   // 2) owner を発行 (= キャラ初期化時)
//   BuffOwnerId player = bs.CreateOwner();
//
//   // 3) tick / expire コールバックで実ロジックを橋渡し
//   bs.SetOnTickCallback(&MyOnBuffTick, &game_ctx);    // HP 増減等
//   bs.SetOnExpireCallback(&MyOnBuffExpire, &game_ctx);
//
//   // 4) ゲーム中に発動
//   bs.ApplyBuff(player, "poison.snake");
//
//   // 5) 毎フレ
//   bs.Tick(dt);
//
//   // 6) AttackUp 等の「持続中の効果倍率」を毎フレ取得する想定:
//   u32 n = 0;
//   const BuffInstance* list = bs.AllBuffsOfOwner(player, n);
//   for (u32 i = 0; i < n; ++i) { /* 計算 */ }
//
// 範囲外 (将来拡張):
//   ・「他の特定 buff と共存できない (Stun は Freeze を消す)」等の相互作用 → 上位
//     ロジックで AllBuffsOfOwner + RemoveBuff を組合せて実現する。
//   ・アイコン / 表示色 / 説明文 → 呼出側の UI 層で別 table を引く。
//   ・永続化 → Pillar J Serialize に委譲。本クラスは起動毎にリセット。
//   ・サーバ側検証 → Pillar V Backend に委譲。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- BuffStackPolicy --------------------------------------------------------
// 同 owner に既に同 id の buff があるときの ApplyBuff の挙動。
//   Refresh : remaining_sec を duration_sec で上書き (stack は据置)。
//   Stack   : stack++ (max_stack で clamp)、remaining_sec も最新値で reset。
//   Ignore  : 何もしない (= 「最初の 1 枚しか効かない」)。
enum class BuffStackPolicy : u8 {
    Refresh,
    Stack,
    Ignore,
};

// ---- BuffKind ---------------------------------------------------------------
// バフ / デバフの大分類タグ。具体的なゲームロジックは呼出側が `kind` を見て
// 切替える想定 (= ACS 側は値を保存するだけで強制しない)。`Custom` はゲーム
// 固有の独自種別のための拡張枠。
enum class BuffKind : u8 {
    AttackUp,
    DefenseUp,
    SpeedUp,
    Regen,
    Poison,
    Burn,
    Freeze,
    Stun,
    Shield,
    Custom,
};

// ---- BuffDef: バフ 1 種類の定義 ---------------------------------------------
// id                  : バフキー (ApplyBuff / RemoveBuff の検索キー)。文字列リテラル想定 (非所有)。
// kind                : 大分類タグ。呼出側のロジック分岐用。
// duration_sec        : 効果持続秒。Refresh / Stack 時に remaining_sec の初期値となる。
//                       0 以下は「登録自体は受理するが ApplyBuff で発動しない」扱い。
// tick_interval_sec   : tick callback の間隔秒。0 以下なら callback を発火しない
//                       (= AttackUp 等の常駐型)。
// magnitude           : 効果量。呼出側が解釈する (= Poison なら 1 tick のダメージ、
//                       AttackUp なら攻撃倍率、Shield なら吸収量、等)。
// stack_policy        : 既存への ApplyBuff の挙動 (Refresh / Stack / Ignore)。
// max_stack           : Stack policy のときの stack 上限 (1 以上推奨)。0 を渡された場合は
//                       defensive に 1 として扱う。Refresh / Ignore のときは無視される。
// is_debuff           : UI 表示色 (赤 / 青) や「dispel 可能か」の判定で使う、開発者宣言フラグ。
//                       Manager は強制せず、値を保存して照会できるようにするだけ。
struct BuffDef {
    const char*     id                = nullptr;
    BuffKind        kind              = BuffKind::Custom;
    f32             duration_sec      = 0.0f;
    f32             tick_interval_sec = 0.0f;
    f32             magnitude         = 0.0f;
    BuffStackPolicy stack_policy      = BuffStackPolicy::Refresh;
    u32             max_stack         = 1;
    bool            is_debuff         = false;
};

// ---- BuffInstance: ある owner に現在掛かっている buff の実体 ---------------
// id            : Definition への参照 (BuffDef::id と同 const char*)。
// remaining_sec : 残り秒数。Tick で減算され、0 以下になると expire される。
// tick_accum    : 直近 tick からの累積秒。tick_interval_sec >= 1 ごとに magnitude
//                 を発火し、その分減算する。
// stack         : 現在の重ねがけ数 (Stack policy のときのみ 2 以上になりうる)。
struct BuffInstance {
    const char* id            = nullptr;
    f32         remaining_sec = 0.0f;
    f32         tick_accum    = 0.0f;
    u32         stack         = 0;
};

// ---- BuffOwnerId: 24bit index + 8bit gen を packed した opaque handle ------
// `_packed == 0` を invalid と定義 (gen は常に 1 以上で配る)。`NodeId` /
// `EmitterHandle` / `TimerHandle` と同一規約。
struct BuffOwnerId {
    u32 _packed = 0u;

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215

    bool IsValid() const noexcept { return _packed != 0u; }

    static BuffOwnerId Pack(u32 index, u8 gen) noexcept {
        BuffOwnerId o;
        o._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return o;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }
};

// ---- BuffSystem ------------------------------------------------------------
class BuffSystem {
public:
    // tick callback. tick_interval_sec ごとに発火 (Regen / Poison / Burn 用)。
    //   user      : SetOnTickCallback で渡したコンテキスト (Manager は所有しない)
    //   owner     : 対象 owner (= キャラ)
    //   buff_id   : 発火した BuffDef::id (= 文字列リテラル等の生 const char*)
    //   stack     : 発火時点の stack 数 (1 以上)
    //   magnitude : BuffDef::magnitude (= 1 tick あたりの効果量)
    using TickCallback = void(*)(void* user, BuffOwnerId owner, const char* buff_id,
                                  u32 stack, f32 magnitude) noexcept;

    // 期限切れ callback. remaining_sec が 0 以下に達した時 (= 自然終了) と
    // RemoveBuff() で明示的に外された時の両方で発火する。
    //   user      : SetOnExpireCallback で渡したコンテキスト
    //   owner     : 対象 owner
    //   buff_id   : 期限切れになった BuffDef::id
    using ExpireCallback = void(*)(void* user, BuffOwnerId owner, const char* buff_id) noexcept;

    BuffSystem()  noexcept = default;
    ~BuffSystem() noexcept = default;

    // 非コピー・非ムーブ: 内部 Array<OwnerSlot> + AllBuffsOfOwner が生バッファを
    // 返す API のため。ムーブで実体アドレスが変わると外部参照が破綻する。
    BuffSystem(const BuffSystem&)            = delete;
    BuffSystem& operator=(const BuffSystem&) = delete;
    BuffSystem(BuffSystem&&)                 = delete;
    BuffSystem& operator=(BuffSystem&&)      = delete;

    // ---- バフ定義 ---------------------------------------------------------
    // id ごとに registry に追加。同 id の 2 重登録は no-op (WARN)。
    // `def.id == nullptr` も no-op。`max_stack == 0` は defensive に 1 として記録する。
    void RegisterBuff(const BuffDef& def) noexcept;

    // ---- owner 管理 -------------------------------------------------------
    // 新規 owner を発行。slot が無ければ末尾追加、空き slot があれば再利用。
    // 24bit index 上限 (16,777,215) に達した場合は invalid handle を返す。
    BuffOwnerId CreateOwner() noexcept;

    // owner を破棄 (= slot 解放 + gen 進める)。owner に掛かっていた全 buff は
    // 「強制クリア」扱いで除去するが、ExpireCallback は発火しない (= 「キャラが
    // 消えた」のと「効果が時間切れになった」を意味的に区別するため)。
    // invalid / stale / 範囲外 handle は no-op。
    void DestroyOwner(BuffOwnerId owner) noexcept;

    // ---- バフの適用 / 除去 ------------------------------------------------
    // owner に buff_id を適用する。
    //   ・owner が無効/stale  : false
    //   ・buff_id が未登録    : false
    //   ・BuffDef::duration_sec <= 0  : false (= 即時消滅するので意味が無い)
    //   ・既存無し                    : 新規追加、true
    //   ・既存有り + Refresh          : remaining_sec を duration_sec で上書き、true
    //   ・既存有り + Stack            : stack++ (max_stack で clamp)、remaining_sec も reset、true
    //                                   (max_stack に達していて clamp で増えない場合も true)
    //   ・既存有り + Ignore           : 何もしない、false
    bool ApplyBuff(BuffOwnerId owner, const char* buff_id) noexcept;

    // owner から buff_id を除去。stack 数に関係なく完全に消す (= 1 個でも残さない)。
    // 除去成功時は ExpireCallback を 1 回発火する。
    // owner 無効/stale / buff_id == nullptr / 該当 buff 無し は false。
    bool RemoveBuff(BuffOwnerId owner, const char* buff_id) noexcept;

    // ---- 照会 -------------------------------------------------------------
    // owner に掛かっている buff の総数 (= BuffInstance の数)。
    // owner 無効/stale は 0。
    u32 BuffCountOnOwner(BuffOwnerId owner) const noexcept;

    // owner に buff_id が掛かっているか。owner 無効/stale / nullptr id は false。
    bool HasBuff(BuffOwnerId owner, const char* buff_id) const noexcept;

    // 指定 buff の現在 stack 数 (掛かっていなければ 0)。
    u32 GetStack(BuffOwnerId owner, const char* buff_id) const noexcept;

    // 指定 buff の残り秒 (掛かっていなければ 0.0f)。
    f32 GetRemaining(BuffOwnerId owner, const char* buff_id) const noexcept;

    // owner の全 buff の生バッファを返す。`out_count` に件数を書き出す。
    // 返却ポインタは ApplyBuff / RemoveBuff / Tick / DestroyOwner / ClearAll で
    // 無効化される可能性がある (= 同フレ内で読み切ること)。
    // owner 無効/stale は nullptr + out_count=0。
    const BuffInstance* AllBuffsOfOwner(BuffOwnerId owner, u32& out_count) const noexcept;

    // owner に掛かっている全 buff を消す。ExpireCallback は発火しない
    // (= DestroyOwner と同じ「強制クリア」セマンティクス)。
    // owner 無効/stale は no-op。
    void ClearAllOnOwner(BuffOwnerId owner) noexcept;

    // ---- コールバック -----------------------------------------------------
    // nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnTickCallback(TickCallback cb, void* user) noexcept;
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    // ---- driver -----------------------------------------------------------
    // 全 owner の全 buff を dt 秒進める:
    //   1) remaining_sec -= dt
    //   2) tick_interval_sec > 0 なら tick_accum -= interval を出来るだけ消化し、
    //      その回数分 TickCallback を発火 (1 フレで複数 tick 可)
    //   3) remaining_sec <= 0 になった buff を swap-and-pop で除去 + ExpireCallback
    //
    // dt <= 0 は no-op。
    void Tick(f32 dt) noexcept;

    // ---- 全リセット ------------------------------------------------------
    // 全 owner + 全 buff + registry + コールバックを破棄。
    void ClearAll() noexcept;

private:
    // ---- 内部 owner slot ------------------------------------------------
    // 各 owner に紐付く buff 配列。`in_use=false` の slot は再利用される。
    // gen は 1 以上で配り、0 は「未使用」を意味する (= packed == 0 と整合)。
    struct OwnerSlot {
        Array<BuffInstance> buffs {};
        u8                  gen      = 0u;
        bool                in_use   = false;
    };

    // ---- 検索 ----------------------------------------------------------
    // buff_id → registry index。未検出は ~0u。
    u32 FindBuffDefSlot(const char* buff_id) const noexcept;

    // owner handle → OwnerSlot* (gen 一致 + in_use + 範囲チェック)。
    // 失敗時は nullptr。const / non-const 二口。
    OwnerSlot*       ResolveOwner(BuffOwnerId owner) noexcept;
    const OwnerSlot* ResolveOwner(BuffOwnerId owner) const noexcept;

    // OwnerSlot 内の buff_id → index。未検出は ~0u。
    static u32 FindBuffInstance(const OwnerSlot& slot, const char* buff_id) noexcept;

    // ---- 状態 ----------------------------------------------------------
    Array<BuffDef>   _registry  {};  // BuffDef テーブル (id ベースで find)
    Array<OwnerSlot> _owners    {};  // OwnerSlot 配列 (generational)

    TickCallback     _on_tick        = nullptr;
    void*            _on_tick_user   = nullptr;
    ExpireCallback   _on_expire      = nullptr;
    void*            _on_expire_user = nullptr;
};

} // namespace acs::game
