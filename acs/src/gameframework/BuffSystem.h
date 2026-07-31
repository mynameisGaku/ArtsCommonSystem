// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * 同 owner に既に同 id の buff があるときの ApplyBuff の挙動。
 *
 * @details
 * バフ定義ごとに固定で、ApplyBuff 呼出側の都合では変えない。
 */
enum class EBuffStackPolicy : u8 {
    /** 既存があれば remaining_sec を duration_sec で上書きする (stack は据置)。 */
    Refresh,

    /** 既存があれば stack++ (max_stack で clamp)、remaining_sec も最新値で reset する。 */
    Stack,

    /** 既存があれば何もしない (= 「最初の 1 枚しか効かない」)。 */
    Ignore,
};

/**
 * バフ / デバフの大分類タグ。
 *
 * @details
 * 具体的なゲームロジックは呼出側が kind を見て切替える想定 (= ACS 側は値を保存
 * するだけで強制しない)。Custom はゲーム固有の独自種別のための拡張枠。
 */
enum class EBuffKind : u8 {
    /** 攻撃力上昇。 */
    AttackUp,

    /** 防御力上昇。 */
    DefenseUp,

    /** 移動・行動速度上昇。 */
    SpeedUp,

    /** 継続回復。 */
    Regen,

    /** 継続毒ダメージ。 */
    Poison,

    /** 継続燃焼ダメージ。 */
    Burn,

    /** 凍結 (行動阻害)。 */
    Freeze,

    /** スタン (行動不能)。 */
    Stun,

    /** ダメージ吸収シールド。 */
    Shield,

    /** ゲーム固有の独自種別 (拡張枠)。 */
    Custom,
};

/**
 * バフ 1 種類の定義。
 *
 * @details
 * RegisterBuff で registry に登録する静的データ。ApplyBuff / RemoveBuff の検索
 * キーは id (= 文字列リテラル想定の生 const char*)。
 */
struct FBuffDef {
    /** バフキー (ApplyBuff / RemoveBuff の検索キー)。文字列リテラル想定 (非所有)。 */
    const char*     id                = nullptr;

    /** 大分類タグ。呼出側のロジック分岐用。 */
    EBuffKind        kind              = EBuffKind::Custom;

    /** 効果持続秒。Refresh / Stack 時に remaining_sec の初期値となる。0 以下は ApplyBuff で発動しない。 */
    f32             duration_sec      = 0.0f;

    /** tick callback の間隔秒。0 以下なら callback を発火しない (= AttackUp 等の常駐型)。 */
    f32             tick_interval_sec = 0.0f;

    /** 効果量。呼出側が解釈する (= Poison なら 1 tick のダメージ、AttackUp なら攻撃倍率、等)。 */
    f32             magnitude         = 0.0f;

    /** 既存への ApplyBuff の挙動 (Refresh / Stack / Ignore)。 */
    EBuffStackPolicy stack_policy      = EBuffStackPolicy::Refresh;

    /** Stack policy のときの stack 上限 (1 以上推奨)。0 は defensive に 1 として扱う。 */
    u32             max_stack         = 1;

    /** UI 表示色や dispel 可否の判定に使う開発者宣言フラグ (Manager は強制せず保存のみ)。 */
    bool            is_debuff         = false;
};

/**
 * ある owner に現在掛かっている buff の実体。
 *
 * @details
 * FOwnerSlot の buff 配列に格納され、Tick で remaining_sec を減算しつつ進行する。
 */
struct FBuffInstance {
    /** Definition への参照 (FBuffDef::id と同 const char*)。 */
    const char* id            = nullptr;

    /** 残り秒数。Tick で減算され、0 以下になると expire される。 */
    f32         remaining_sec = 0.0f;

    /** 直近 tick からの累積秒。tick_interval_sec ごとに magnitude を発火し、その分減算する。 */
    f32         tick_accum    = 0.0f;

    /** 現在の重ねがけ数 (Stack policy のときのみ 2 以上になりうる)。 */
    u32         stack         = 0;
};

/**
 * 24bit index + 8bit gen を packed した owner の opaque handle。
 *
 * @details
 * m_Packed == 0 を invalid と定義 (gen は常に 1 以上で配る)。FNodeId /
 * FEmitterHandle / FSceneTimerHandle と同一規約。
 */
struct FBuffOwnerId {
    /** packed 表現 (上位 8bit = gen、下位 24bit = index)。0 は invalid。 */
    u32 m_Packed = 0u;

    /** index に割り当てるビット幅 (24)。 */
    static constexpr u32 kIndexBits = 24u;

    /** index 部を取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** 表現可能な最大 index (16777215)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /**
     * index と gen から packed handle を生成する。
     *
     * @param index owner slot のインデックス。
     * @param gen 世代番号 (1 以上)。
     * @return packed した FBuffOwnerId。
     */
    static FBuffOwnerId Pack(u32 index, u8 gen) noexcept {
        FBuffOwnerId o;
        o.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return o;
    }

    /**
     * index 部を取り出す。
     *
     * @return owner slot のインデックス。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * gen 部を取り出す。
     *
     * @return 世代番号。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }
};

/**
 * 複数 owner に複数の時間制限付きバフ / デバフを載せて tick で進行管理するマネージャ。
 *
 * @details
 * 「いつ、どの owner に、どの buff が、何 stack あるか、残り何秒か」だけを真実として
 * 保持する。実際のステータス計算 (= AttackUp が攻撃力に何倍を掛けるか) は呼出側が
 * AllBuffsOfOwner で列挙して行う。owner は generational handle で管理し、buff は owner
 * ごとの AoS 配列に持つ。非コピー・非ムーブ (AllBuffsOfOwner が生バッファを返すため)。
 */
class FBuffSystem {
public:
    /**
     * tick 発火時に呼ぶコールバック型 (Regen / Poison / Burn 用)。
     *
     * @details tick_interval_sec ごとに発火する。
     * @param user SetOnTickCallback で渡したコンテキスト (Manager は所有しない)。
     * @param owner 対象 owner (= キャラ)。
     * @param buff_id 発火した FBuffDef::id (= 文字列リテラル等の生 const char*)。
     * @param stack 発火時点の stack 数 (1 以上)。
     * @param magnitude FBuffDef::magnitude (= 1 tick あたりの効果量)。
     */
    using TickCallback = void(*)(void* user, FBuffOwnerId owner, const char* buff_id,
                                  u32 stack, f32 magnitude) noexcept;

    /**
     * バフ期限切れ時に呼ぶコールバック型。
     *
     * @details remaining_sec が 0 以下に達した自然終了時と、RemoveBuff() で明示的に
     * 外された時の両方で発火する。
     * @param user SetOnExpireCallback で渡したコンテキスト。
     * @param owner 対象 owner。
     * @param buff_id 期限切れになった FBuffDef::id。
     */
    using ExpireCallback = void(*)(void* user, FBuffOwnerId owner, const char* buff_id) noexcept;

    /** 空のシステムを構築する。 */
    FBuffSystem()  noexcept = default;

    /** 破棄する。 */
    ~FBuffSystem() noexcept = default;

    /** コピー禁止 (AllBuffsOfOwner が生バッファを返すため実体アドレスを固定する)。 */
    FBuffSystem(const FBuffSystem&)            = delete;

    /** コピー代入も禁止。 */
    FBuffSystem& operator=(const FBuffSystem&) = delete;

    /** ムーブ禁止 (内部実体アドレスを固定する)。 */
    FBuffSystem(FBuffSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FBuffSystem& operator=(FBuffSystem&&)      = delete;

    /**
     * バフ定義を registry に登録する。
     *
     * @details 同 id の 2 重登録は no-op (WARN)。def.id == nullptr も no-op。
     * max_stack == 0 は defensive に 1 として記録する。
     * @param def 登録するバフ定義。
     */
    void RegisterBuff(const FBuffDef& def) noexcept;

    /**
     * 新規 owner を発行する。
     *
     * @details slot が無ければ末尾追加、空き slot があれば再利用する。
     * @return 発行した owner handle。24bit index 上限 (16,777,215) 到達時は invalid handle。
     */
    FBuffOwnerId CreateOwner() noexcept;

    /**
     * owner を破棄する (= slot 解放 + gen 進める)。
     *
     * @details owner に掛かっていた全 buff は「強制クリア」扱いで除去するが、
     * ExpireCallback は発火しない (= 「キャラが消えた」と「効果が時間切れ」を意味的に
     * 区別するため)。invalid / stale / 範囲外 handle は no-op。
     * @param owner 破棄する owner handle。
     */
    void DestroyOwner(FBuffOwnerId owner) noexcept;

    /**
     * owner に buff_id を適用する。
     *
     * @details
     * 既存無しなら新規追加。既存有り + Refresh は remaining_sec を duration_sec で上書き、
     * Stack は stack++ (max_stack で clamp) + remaining_sec reset、Ignore は何もしない。
     * @param owner 適用先の owner handle。
     * @param buff_id 適用するバフの id。
     * @return 適用に成功すれば true。owner 無効/stale、未登録 id、duration_sec <= 0、Ignore 既存ありは false。
     */
    bool ApplyBuff(FBuffOwnerId owner, const char* buff_id) noexcept;

    /**
     * owner から buff_id を除去する。
     *
     * @details stack 数に関係なく完全に消す (= 1 個でも残さない)。除去成功時は
     * ExpireCallback を 1 回発火する。
     * @param owner 対象 owner handle。
     * @param buff_id 除去するバフの id。
     * @return 除去できたら true。owner 無効/stale、buff_id == nullptr、該当 buff 無しは false。
     */
    bool RemoveBuff(FBuffOwnerId owner, const char* buff_id) noexcept;

    /**
     * owner に掛かっている buff の総数を返す。
     *
     * @param owner 対象 owner handle。
     * @return buff 数 (= FBuffInstance の数)。owner 無効/stale は 0。
     */
    u32 BuffCountOnOwner(FBuffOwnerId owner) const noexcept;

    /**
     * owner に buff_id が掛かっているかを返す。
     *
     * @param owner 対象 owner handle。
     * @param buff_id 調べるバフの id。
     * @return 掛かっていれば true。owner 無効/stale、nullptr id は false。
     */
    bool HasBuff(FBuffOwnerId owner, const char* buff_id) const noexcept;

    /**
     * 指定 buff の現在 stack 数を返す。
     *
     * @param owner 対象 owner handle。
     * @param buff_id 調べるバフの id。
     * @return 現在の stack 数 (掛かっていなければ 0)。
     */
    u32 GetStack(FBuffOwnerId owner, const char* buff_id) const noexcept;

    /**
     * 指定 buff の残り秒を返す。
     *
     * @param owner 対象 owner handle。
     * @param buff_id 調べるバフの id。
     * @return 残り秒数 (掛かっていなければ 0.0f)。
     */
    f32 GetRemaining(FBuffOwnerId owner, const char* buff_id) const noexcept;

    /**
     * owner の全 buff の生バッファを返す。
     *
     * @details 返却ポインタは ApplyBuff / RemoveBuff / Tick / DestroyOwner / ClearAll で
     * 無効化される可能性がある (= 同フレ内で読み切ること)。
     * @param owner 対象 owner handle。
     * @param out_count buff 件数の書き出し先。
     * @return 先頭 FBuffInstance へのポインタ。owner 無効/stale は nullptr + out_count=0。
     */
    const FBuffInstance* AllBuffsOfOwner(FBuffOwnerId owner, u32& out_count) const noexcept;

    /**
     * owner に掛かっている全 buff を消す。
     *
     * @details ExpireCallback は発火しない (= DestroyOwner と同じ「強制クリア」セマンティクス)。
     * @param owner 対象 owner handle。owner 無効/stale は no-op。
     */
    void ClearAllOnOwner(FBuffOwnerId owner) noexcept;

    /**
     * tick コールバックを設定する。
     *
     * @param cb 設定するコールバック (nullptr で detach)。
     * @param user コールバックに渡すコンテキスト (Manager は所有しない)。
     */
    void SetOnTickCallback(TickCallback cb, void* user) noexcept;

    /**
     * 期限切れコールバックを設定する。
     *
     * @param cb 設定するコールバック (nullptr で detach)。
     * @param user コールバックに渡すコンテキスト (Manager は所有しない)。
     */
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    /**
     * 全 owner の全 buff を dt 秒進める。
     *
     * @details
     * remaining_sec を減算し、tick_interval_sec > 0 なら間隔を消化した回数分 TickCallback を
     * 発火 (1 フレで複数 tick 可)、remaining_sec <= 0 になった buff を swap-and-pop で除去して
     * ExpireCallback を発火する。
     * @param dt 経過秒。0 以下は no-op。
     */
    void Tick(f32 dt) noexcept;

    /** 全 owner + 全 buff + registry + コールバックを破棄する。 */
    void ClearAll() noexcept;

private:
    /**
     * 各 owner に紐付く内部 slot。
     *
     * @details in_use=false の slot は再利用される。gen は 1 以上で配り、0 は「未使用」を
     * 意味する (= packed == 0 と整合)。
     */
    struct FOwnerSlot {
        /** この owner に掛かっている buff の配列。 */
        TArray<FBuffInstance> buffs {};

        /** 世代番号 (1 以上で配る。0 は未使用 slot)。 */
        u8                  gen      = 0u;

        /** slot が使用中かどうか。 */
        bool                in_use   = false;
    };

    /**
     * buff_id から registry 内のインデックスを引く。
     *
     * @param buff_id 検索するバフの id。
     * @return registry index (未検出は ~0u)。
     */
    u32 FindBuffDefSlot(const char* buff_id) const noexcept;

    /**
     * owner handle から FOwnerSlot を解決する。
     *
     * @details gen 一致 + in_use + 範囲チェックを行う。
     * @param owner 解決する owner handle。
     * @return FOwnerSlot へのポインタ (失敗時は nullptr)。
     */
    FOwnerSlot*       ResolveOwner(FBuffOwnerId owner) noexcept;

    /**
     * owner handle から FOwnerSlot を解決する (const 版)。
     *
     * @param owner 解決する owner handle。
     * @return FOwnerSlot への const ポインタ (失敗時は nullptr)。
     */
    const FOwnerSlot* ResolveOwner(FBuffOwnerId owner) const noexcept;

    /**
     * FOwnerSlot 内で buff_id を持つ FBuffInstance のインデックスを引く。
     *
     * @param slot 検索対象の owner slot。
     * @param buff_id 検索するバフの id。
     * @return slot.buffs 内のインデックス (未検出は ~0u)。
     */
    static u32 FindBuffInstance(const FOwnerSlot& slot, const char* buff_id) noexcept;

    /** FBuffDef テーブル (id ベースで find)。 */
    TArray<FBuffDef>   m_Registry  {};

    /** FOwnerSlot 配列 (generational)。 */
    TArray<FOwnerSlot> m_Owners    {};

    /** tick コールバック (nullptr なら未設定)。 */
    TickCallback     m_OnTick        = nullptr;

    /** tick コールバックに渡すコンテキスト (非所有)。 */
    void*            m_OnTickUser   = nullptr;

    /** 期限切れコールバック (nullptr なら未設定)。 */
    ExpireCallback   m_OnExpire      = nullptr;

    /** 期限切れコールバックに渡すコンテキスト (非所有)。 */
    void*            m_OnExpireUser = nullptr;
};

} // namespace acs::game
