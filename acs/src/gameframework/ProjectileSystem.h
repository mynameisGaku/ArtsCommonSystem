// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — CProjectileSystem (弾丸 / 投射物 プール + 衝突判定)
//
// 弾丸 / ロケット / 矢 / 魔法弾 / 手榴弾 などの「飛んでいって何かに当たる」系
// オブジェクトを固定容量プールで管理する。CWeaponSystem.FireCallback と組み合わせ
// て発射 → 飛翔 → 命中 / 寿命切れ までを一元的に扱えるよう設計してある。
//
// 設計選択:
//   ・**FProjectileId は 24bit idx + 8bit gen の packed u32**: FHealthId / FNodeId /
//     FEmitterHandle と同じ規約。slot 再利用後の stale 参照は IsValid + 内部 gen
//     一致で検出する。m_Packed == 0 を invalid と定義 (gen は常に 1 以上で配る)。
//   ・**pool は固定容量**: Init(max_concurrent) で確保した後はリサイズしない。
//     Spawn は inactive な slot を線形探索し、満杯なら invalid を返す。これに
//     より realtime ループでのフレーム落ちを防ぐ (= 最悪ケース上限を制限する
//     保守的ポリシー)。一般的に max_concurrent は 256〜512 で十分。
//   ・**def は名前で登録 → Spawn で参照**: FParticleEmitterDef のように個別に値
//     をコピーするのではなく、FProjectileDef を「弾種」として事前 RegisterDef、
//     Spawn 時に def_id (const char*) で名前引きする。これにより
//       - 多数の弾を発射しても per-instance memory が小さい (def はポインタで参照)
//       - 同種の弾の挙動を一括変更できる (テスト / バランス調整時に有利)
//       - CWeaponSystem / EnemyAI / Player を疎結合に保てる (def_id 文字列のみで連携)
//     名前比較は ConstStringPointerEquals (アドレス一致優先) を使うが、同一文字
//     リテラルなら基本同アドレスにマージされるためコストは O(1) 期待。
//   ・**HitTest は callback で外部委譲**: CProjectileSystem 自身は CCollisionWorld2D /
//     CTriggerWorld2D / 独自空間検索のどれを使うかを知らない。HitTestFn を登録し、
//     Tick 内で各 alive projectile に対して呼び出す。true 返却で「命中した」と判
//     定し、hit_count を進める。これにより:
//       - テスト時は dummy fn で常に false を返せる (= headless 動作可)
//       - 異なるシーン / ステージで異なる衝突空間を使い分けられる
//   ・**hit_count >= max_pierces+1 で despawn**: pierces=false なら max_pierces は
//     強制的に 0 として扱い、最初の命中で消滅。pierces=true なら max_pierces 個
//     まで貫通し、+1 個目の命中で消滅 (= max_pierces=2 → 3 体目で消える)。
//   ・**homing は単純な向き補正**: Tick 内で homing_target 方向の単位ベクトルと
//     現在 velocity の単位ベクトルを homing_strength で線形補間。物理的には正確で
//     ないが、見た目には十分な追従感が出る。複雑な誘導 (proportional navigation)
//     は別レイヤに分離する。
//   ・**owner_id / damage は instance に持つ**: def には共通パラメータのみを置き、
//     誰が撃ったか / どれだけのダメージか は spawn 毎に変化する値として instance
//     側に保持する。HitCallback でこれらを取り出し CHealthSystem.ApplyDamage に
//     繋ぐ想定。
//   ・**非コピー・非ムーブ**: 固定 pool の生ポインタを AllAlive で外部に返すため、
//     ムーブで実体アドレスが変わると外部参照が破綻する。明示的に削除。
//   ・**全 noexcept / STL 不使用 / <string> 禁止**: ACS 規約。失敗は invalid id /
//     no-op で表現。文字列は const char* + アドレス一致比較で扱う。
//
// 使い方:
//   CProjectileSystem ps;
//   ps.Init(256);
//
//   FProjectileDef bullet{};
//   bullet.id            = "bullet_9mm";
//   bullet.kind          = EProjectileKind::Bullet;
//   bullet.speed         = 800.0f;
//   bullet.lifetime_sec  = 1.5f;
//   bullet.radius        = 2.0f;
//   bullet.gravity_y     = 0.0f;
//   bullet.pierces       = false;
//   bullet.max_pierces   = 0;
//   bullet.homing        = false;
//   ps.RegisterDef(bullet);
//
//   // 発射:
//   FProjectileId pid = ps.Spawn("bullet_9mm",
//                                /*pos=*/player_pos,
//                                /*vel=*/FVec2{800.0f, 0.0f},
//                                /*owner=*/player_node_id,
//                                /*damage=*/15.0f);
//
//   // hit test を登録 (CCollisionWorld2D 経由など):
//   ps.SetHitTestFn([](void* user, const FProjectileInstance& p,
//                      u32& out_target, f32& out_dmg) noexcept -> bool {
//       auto* cw = static_cast<CCollisionWorld2D*>(user);
//       // ... overlap check ... out_target = enemy_node_id; out_dmg = p.damage;
//       return hit_found;
//   }, &collision_world);
//
//   // hit / expire コールバック:
//   ps.SetOnHitCallback([](void* user, FProjectileId id, const char* def_id,
//                          u32 target, f32 dmg) noexcept {
//       // ApplyDamage / VFX / SE
//   }, &my_game);
//
//   // 毎フレーム:
//   ps.Tick(dt);
//
// 範囲外:
//   ・チャージ shot / spread (n-way) - CWeaponSystem 側が複数 Spawn する形で対応
//   ・3D 弾道 (現状 FVec2 のみ)
//   ・反射 / 跳弾 (壁衝突で velocity 反転) - HitCallback 内で manual に再 Spawn
//   ・複雑な誘導 (proportional navigation など)
//   ・テクスチャ参照 / アニメーション - 描画は user 側、本クラスは姿勢だけ管理
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 弾種ヒント (VFX / SE 振り分け用)。
 *
 * @details 描画側 / SE 側がアセット切り替えのキーとして利用する。本クラスは挙動には
 * 使わず、値を保持して HitCallback / ExpireCallback で伝えるのみ。
 */
enum class EProjectileKind : u8 {
    /** 通常弾。 */
    Bullet    = 0,

    /** ロケット。 */
    Rocket    = 1,

    /** 矢。 */
    Arrow     = 2,

    /** 魔法弾。 */
    MagicBolt = 3,

    /** 手榴弾。 */
    Grenade   = 4,

    /** ビームパルス。 */
    BeamPulse = 5,

    /** その他 (ユーザー定義)。 */
    Custom    = 6,
};

/**
 * 弾種毎の挙動パラメータ (RegisterDef で登録)。
 *
 * @details 1 つの弾種を「def」として事前登録し、Spawn 時に id で名前引きする。多数の弾を
 * 発射しても per-instance memory が小さく、同種の弾の挙動を一括変更できる。
 */
struct FProjectileDef {
    /** 弾種の識別子 (string literal 推奨、Spawn 時の名前引きキー)。 */
    const char*    id              = nullptr;

    /** VFX / SE 振り分け用ヒント (挙動には影響しない)。 */
    EProjectileKind kind            = EProjectileKind::Bullet;

    /** 初速度の大きさ (参考値、実際の初速は Spawn の velocity が優先)。 */
    f32            speed           = 0.0f;

    /** 最大寿命 (秒)。これを超えると ExpireCallback 経由で despawn。 */
    f32            lifetime_sec    = 0.0f;

    /** 当たり判定半径 (HitTestFn が使うかは user 次第)。 */
    f32            radius          = 0.0f;

    /** 重力加速度 (正値=下方向)。0 で直進、Grenade / Arrow は >0。 */
    f32            gravity_y       = 0.0f;

    /** true=複数 hit で消えない。false なら max_pierces は無視され 0 扱い。 */
    bool           pierces         = false;

    /** pierces=true 時の追加貫通回数 (max_pierces=2 → 3 体目で消滅)。 */
    u32            max_pierces     = 0u;

    /** true=SetHomingTarget で指定した位置に向き補正する。 */
    bool           homing          = false;

    /** 向き補正の強さ [0,1] (毎フレーム単位ベクトル LERP の係数)。 */
    f32            homing_strength = 0.0f;
};

/**
 * 24bit index + 8bit gen を packed した opaque な弾ハンドル。
 *
 * @details m_Packed == 0 を invalid と定義する (gen は常に 1 以上で配る)。slot 再利用後の
 * stale 参照は IsValid + 内部 gen 一致で検出する。
 */
struct FProjectileId {
    /** packed 値 (上位 8bit=gen、下位 24bit=index、0 で invalid)。 */
    u32 m_Packed = 0u;

    /**
     * 有効なハンドルかを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index フィールドのビット幅。 */
    static constexpr u32 kIndexBits = 24u;

    /** index フィールドのビットマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** index の最大値 (16777215)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * index と gen を packed して FProjectileId を作る。
     *
     * @param index slot index ([0, kMaxIndex])。
     * @param gen generation 値 (1 以上)。
     * @return packed した FProjectileId。
     */
    static FProjectileId Pack(u32 index, u8 gen) noexcept {
        FProjectileId h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * slot index を取り出す。
     *
     * @return 下位 24bit の index。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * generation 値を取り出す。
     *
     * @return 上位 8bit の gen。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }
};

/**
 * 個別弾の生データ (AllAlive で外部に渡される)。
 *
 * @details def_id は RegisterDef で登録した string literal をそのまま指す (描画側で弾種別
 * アセットを引くキー)。owner_id / damage は spawn 毎に変化する値。
 */
struct FProjectileInstance {
    /** 弾種識別子 (RegisterDef した string literal を指す、非所有)。 */
    const char* def_id      = nullptr;

    /** 現在位置。 */
    FVec2        position    {0.0f, 0.0f};

    /** 現在速度。 */
    FVec2        velocity    {0.0f, 0.0f};

    /** 出生からの経過時間 (秒、lifetime チェック用)。 */
    f32         elapsed_sec = 0.0f;

    /** 当てた回数 (max_pierces+1 で despawn)。 */
    u32         hit_count   = 0u;

    /** 撃った主体の識別子 (ANode::Id 等)。 */
    u32         owner_id    = 0u;

    /** 1 hit 当たりのダメージ量。 */
    f32         damage      = 0.0f;
};

/**
 * 命中判定コールバック型。
 *
 * @details Tick 内で alive な projectile 1 個ごとに呼ばれる。true を返すと hit_count++、
 * 上限に達すれば despawn する (このとき ExpireCallback は発火しない)。引数は
 * user (SetHitTestFn で渡した context)、proj (判定対象)、out_hit_target_id (hit した
 * ターゲット識別子)、out_damage_dealt (実際に与えたダメージ、HitCallback へ渡る)。
 */
using HitTestFn = bool(*)(void* user, const FProjectileInstance& proj,
                          u32& out_hit_target_id, f32& out_damage_dealt) noexcept;

/**
 * 命中コールバック型。
 *
 * @details HitTestFn が true を返した直後に発火する。この中で CHealthSystem.ApplyDamage /
 * VFX 生成 / SE 再生 を行う想定。引数は user、proj_id、def_id (弾種識別子)、
 * target_id (HitTestFn の out_hit_target_id)、damage (out_damage_dealt)。
 */
using HitCallback = void(*)(void* user, FProjectileId proj_id, const char* def_id,
                            u32 target_id, f32 damage) noexcept;

/**
 * 寿命切れコールバック型。
 *
 * @details lifetime_sec を超えた projectile に対して発火する。命中で消えた弾では発火しない
 * (= HitCallback 経由のみ)。
 */
using ExpireCallback = void(*)(void* user, FProjectileId proj_id, const char* def_id) noexcept;

/**
 * 固定容量 pool で弾丸 / 投射物を管理し、毎フレーム Tick で飛翔・命中・寿命を処理する。
 *
 * @details
 * Init(max_concurrent) で確保した固定容量プールに inactive slot を線形探索して Spawn し、
 * 満杯なら invalid を返す。弾種は FProjectileDef を RegisterDef し Spawn 時に def_id で
 * 名前引きする。命中判定は HitTestFn で外部委譲し、Tick 内で各 alive 弾に対して呼ぶ。
 * hit_count が貫通上限に達するか lifetime を超えると despawn する。AllAlive() が内部
 * buffer の生ポインタを返すため非コピー・非ムーブ。全 noexcept、STL 不使用。
 */
class CProjectileSystem {
public:
    /** 空状態で構築する (pool は Init で確保)。 */
    CProjectileSystem() noexcept = default;

    /** 破棄する (pool は TArray が解放)。 */
    ~CProjectileSystem() noexcept = default;

    /** コピー禁止 (内部 buffer の生ポインタを外部に返すため)。 */
    CProjectileSystem(const CProjectileSystem&)            = delete;

    /** コピー代入も禁止。 */
    CProjectileSystem& operator=(const CProjectileSystem&) = delete;

    /** ムーブ禁止 (実体アドレスが変わると外部参照が破綻するため)。 */
    CProjectileSystem(CProjectileSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CProjectileSystem& operator=(CProjectileSystem&&)      = delete;

    /**
     * pool を確保して初期化する。
     *
     * @details 再 Init は no-op (固定容量ポリシー)。0 を渡した場合は default の 256 を採用。
     * @param max_concurrent 同時に存在できる弾の上限 (既定 256)。
     */
    void Init(u32 max_concurrent = 256u) noexcept;

    /**
     * 弾種を登録する。
     *
     * @details def.id == nullptr / lifetime_sec <= 0 は無視。既に同 id (アドレス一致 or
     * 文字列一致) が登録済みなら上書き更新。
     * @param def 登録する弾種パラメータ。
     */
    void RegisterDef(const FProjectileDef& def) noexcept;

    /**
     * 弾を発射する。
     *
     * @details owner_id / damage は instance にそのまま保持される。
     * @param def_id 発射する弾種の id (未登録なら invalid を返す)。
     * @param pos 初期位置。
     * @param velocity 初速度。
     * @param owner_id 撃った主体の識別子。
     * @param damage 1 hit 当たりのダメージ量。
     * @return 発射した弾のハンドル (pool 満杯 / def 未登録なら invalid)。
     */
    FProjectileId Spawn(const char* def_id, FVec2 pos, FVec2 velocity,
                       u32 owner_id, f32 damage) noexcept;

    /**
     * 弾を強制的に消滅させる。
     *
     * @details stale handle は no-op。ExpireCallback は発火しない (Destroy/Cancel 系の慣例)。
     * @param id 消滅させる弾のハンドル。
     */
    void Despawn(FProjectileId id) noexcept;

    /**
     * 現在 alive な弾数を返す。
     *
     * @return alive な弾の数。
     */
    u32 AliveCount() const noexcept { return m_AliveCount; }

    /**
     * pool 容量を返す。
     *
     * @return Init で確定した pool 上限。
     */
    u32 MaxCount() const noexcept { return m_Capacity; }

    /**
     * 個別弾の instance を取得する。
     *
     * @param id 取得する弾のハンドル。
     * @return instance へのポインタ (stale handle なら nullptr)。
     */
    const FProjectileInstance* GetInstance(FProjectileId id) const noexcept;

    /**
     * alive な弾を連続バッファで返す (描画ループ用)。
     *
     * @details 内部 snapshot buffer に alive 個だけを詰めて返す。snapshot は Spawn /
     * Despawn / Tick で dirty 化し、ここで lazy に再構築する。
     * @param out_count alive 弾の数を書き出す先。
     * @return alive instance 配列の先頭 (alive 0 件なら nullptr)。
     */
    const FProjectileInstance* AllAlive(u32& out_count) const noexcept;

    /**
     * 全 alive 弾を dt 秒分だけ進める。
     *
     * @details 各弾に対し (1) gravity 加算 (semi-implicit Euler) → (2) homing 向き補正 →
     * (3) position 更新 → (4) HitTestFn で命中チェック (命中で hit_count++ / HitCallback、
     * 貫通上限超過で despawn) → (5) lifetime 超過で despawn + ExpireCallback、を行う。
     * @param dt 経過秒 (<= 0 は no-op)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 命中判定コールバックを登録する。
     *
     * @param fn 登録する HitTestFn (nullptr で登録解除)。
     * @param user fn に渡す context。
     */
    void SetHitTestFn(HitTestFn fn, void* user) noexcept;

    /**
     * 命中コールバックを登録する。
     *
     * @param cb 登録する HitCallback (nullptr で登録解除)。
     * @param user cb に渡す user data。
     */
    void SetOnHitCallback(HitCallback cb, void* user) noexcept;

    /**
     * 寿命切れコールバックを登録する。
     *
     * @param cb 登録する ExpireCallback (nullptr で登録解除)。
     * @param user cb に渡す user data。
     */
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    /**
     * homing 弾の追従目標位置を設定する。
     *
     * @details homing=false の def を持つ弾に呼んでも no-op (内部で homing フラグを再確認)。
     * stale handle も no-op。
     * @param id 対象の弾ハンドル。
     * @param target_pos 追従する目標位置。
     */
    void SetHomingTarget(FProjectileId id, FVec2 target_pos) noexcept;

    /**
     * 全弾を即座に破棄する。
     *
     * @details callback / def 登録と pool 容量は維持。ExpireCallback は発火しない
     * (サイレント全消去)。
     */
    void ClearAll() noexcept;

private:
    /**
     * pool の 1 slot。
     *
     * @details active=false で空き扱い。gen は generational handle 用。
     */
    struct FSlot {
        /** 弾の生データ。 */
        FProjectileInstance inst         {};

        /** homing の追従目標位置。 */
        FVec2               homing_tgt   {0.0f, 0.0f};

        /** homing 目標が設定済みかのフラグ。 */
        bool               has_homing_target = false;

        /** slot が使用中 (alive) かのフラグ。 */
        bool               active       = false;

        /** generation 値 (stale handle 検出用、Spawn 毎に +1)。 */
        u8                 gen          = 0u;
    };

    /** 登録済み弾種 def (可変長、id ポインタで識別)。 */
    TArray<FProjectileDef> m_Defs;

    /** pool 本体 (固定容量の slot 配列)。 */
    TArray<FSlot>                m_Slots;

    /** AllAlive 専用の連続スナップショット buffer。 */
    TArray<FProjectileInstance>  m_AliveSnapshot;

    /** pool 容量 (Init で確定)。 */
    u32 m_Capacity     = 0u;

    /** 現在 alive な弾数。 */
    u32 m_AliveCount  = 0u;

    /** m_AliveSnapshot に詰まっている個数 (m_AliveCount と一致で cache hit)。 */
    u32 m_SnapshotDirtySize = 0u;

    /** 命中判定コールバック (nullptr で未登録)。 */
    HitTestFn      m_HitTestFn       = nullptr;

    /** 命中判定コールバックに渡す context。 */
    void*          m_HitTestUser     = nullptr;

    /** 命中コールバック (nullptr で未登録)。 */
    HitCallback    m_OnHit            = nullptr;

    /** 命中コールバックに渡す user data。 */
    void*          m_OnHitUser       = nullptr;

    /** 寿命切れコールバック (nullptr で未登録)。 */
    ExpireCallback m_OnExpire         = nullptr;

    /** 寿命切れコールバックに渡す user data。 */
    void*          m_OnExpireUser    = nullptr;

    /**
     * def_id で登録済み弾種を引く。
     *
     * @param id 引く弾種の id。
     * @return def へのポインタ (見つからなければ nullptr)。
     */
    const FProjectileDef* FindDef(const char* id) const noexcept;

    /**
     * ハンドルから slot を引く (非 const 版)。
     *
     * @param id 引く弾のハンドル。
     * @return slot へのポインタ (stale / inactive なら nullptr)。
     */
    FSlot*       FindSlot(FProjectileId id) noexcept;

    /**
     * ハンドルから slot を引く (const 版)。
     *
     * @param id 引く弾のハンドル。
     * @return slot への const ポインタ (stale / inactive なら nullptr)。
     */
    const FSlot* FindSlot(FProjectileId id) const noexcept;

    /**
     * 空き slot index を取得する。
     *
     * @return 確保した slot index (pool 満杯なら kInvalidIdx)。
     */
    u32 AcquireSlot() noexcept;

    /** alive snapshot を再構築する (Tick 終端 / Spawn / Despawn 直後に呼ぶ)。 */
    void RebuildAliveSnapshot() noexcept;

    /** AcquireSlot が満杯を表すために返す番兵 index。 */
    static constexpr u32 kInvalidIdx = 0xFFFFFFFFu;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FProjectileSystem = CProjectileSystem;

} // namespace acs::game
