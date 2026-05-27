// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — FProjectileSystem (弾丸 / 投射物 プール + 衝突判定)
//
// 弾丸 / ロケット / 矢 / 魔法弾 / 手榴弾 などの「飛んでいって何かに当たる」系
// オブジェクトを固定容量プールで管理する。FWeaponSystem.FireCallback と組み合わせ
// て発射 → 飛翔 → 命中 / 寿命切れ までを一元的に扱えるよう設計してある。
//
// 設計選択:
//   ・**FProjectileId は 24bit idx + 8bit gen の packed u32**: FHealthId / FNodeId /
//     FEmitterHandle と同じ規約。slot 再利用後の stale 参照は IsValid + 内部 gen
//     一致で検出する。_packed == 0 を invalid と定義 (gen は常に 1 以上で配る)。
//   ・**pool は固定容量**: Init(max_concurrent) で確保した後はリサイズしない。
//     Spawn は inactive な slot を線形探索し、満杯なら invalid を返す。これに
//     より realtime ループでのフレーム落ちを防ぐ (= 最悪ケース上限を制限する
//     保守的ポリシー)。一般的に max_concurrent は 256〜512 で十分。
//   ・**def は名前で登録 → Spawn で参照**: ParticleEmitterDef のように個別に値
//     をコピーするのではなく、ProjectileDef を「弾種」として事前 RegisterDef、
//     Spawn 時に def_id (const char*) で名前引きする。これにより
//       - 多数の弾を発射しても per-instance memory が小さい (def はポインタで参照)
//       - 同種の弾の挙動を一括変更できる (テスト / バランス調整時に有利)
//       - FWeaponSystem / EnemyAI / Player を疎結合に保てる (def_id 文字列のみで連携)
//     名前比較は ConstStringPointerEquals (アドレス一致優先) を使うが、同一文字
//     リテラルなら基本同アドレスにマージされるためコストは O(1) 期待。
//   ・**HitTest は callback で外部委譲**: FProjectileSystem 自身は FCollisionWorld2D /
//     FTriggerWorld2D / 独自空間検索のどれを使うかを知らない。HitTestFn を登録し、
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
//     は将来の Phase で別レイヤに分離する。
//   ・**owner_id / damage は instance に持つ**: def には共通パラメータのみを置き、
//     誰が撃ったか / どれだけのダメージか は spawn 毎に変化する値として instance
//     側に保持する。HitCallback でこれらを取り出し FHealthSystem.ApplyDamage に
//     繋ぐ想定。
//   ・**非コピー・非ムーブ**: 固定 pool の生ポインタを AllAlive で外部に返すため、
//     ムーブで実体アドレスが変わると外部参照が破綻する。明示的に削除。
//   ・**全 noexcept / STL 不使用 / <string> 禁止**: ACS 規約。失敗は invalid id /
//     no-op で表現。文字列は const char* + アドレス一致比較で扱う。
//
// 使い方:
//   FProjectileSystem ps;
//   ps.Init(256);
//
//   ProjectileDef bullet{};
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
//   // hit test を登録 (FCollisionWorld2D 経由など):
//   ps.SetHitTestFn([](void* user, const ProjectileInstance& p,
//                      u32& out_target, f32& out_dmg) noexcept -> bool {
//       auto* cw = static_cast<FCollisionWorld2D*>(user);
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
// 範囲外 (Phase 2+ で):
//   ・チャージ shot / spread (n-way) - FWeaponSystem 側が複数 Spawn する形で対応
//   ・3D 弾道 (現状 FVec2 のみ)
//   ・反射 / 跳弾 (壁衝突で velocity 反転) - HitCallback 内で manual に再 Spawn
//   ・複雑な誘導 (proportional navigation など)
//   ・テクスチャ参照 / アニメーション - 描画は user 側、本クラスは姿勢だけ管理
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// ---------------------------------------------------------------------------
// EProjectileKind — 弾種ヒント (VFX / SE 振り分け用、本クラスでは挙動に使わない)
// ---------------------------------------------------------------------------
// 描画側 / SE 側がアセット切り替えのキーとして利用する。本クラスは値を保持して
// HitCallback / ExpireCallback で伝えるのみ。
enum class EProjectileKind : u8 {
    Bullet    = 0,
    Rocket    = 1,
    Arrow     = 2,
    MagicBolt = 3,
    Grenade   = 4,
    BeamPulse = 5,
    Custom    = 6,
};

// ---------------------------------------------------------------------------
// ProjectileDef — 弾種毎の挙動パラメータ (RegisterDef で登録)
// ---------------------------------------------------------------------------
// `id`             : 弾種の識別子 (string literal 推奨、Spawn 時の名前引きキー)。
// `kind`           : VFX / SE 振り分け用ヒント (挙動には影響しない)。
// `speed`          : 初速度の大きさ (参考値、実際の初速は Spawn の velocity が優先)。
// `lifetime_sec`   : 最大寿命 (秒)。これを超えると expire callback 経由で despawn。
// `radius`         : 当たり判定半径 (HitTestFn が使うかどうかは user 次第)。
// `gravity_y`      : 重力加速度 (正値=下方向)。0 で直進、Grenade / Arrow は >0。
// `pierces`        : true = 複数 hit で消えない。false なら max_pierces は無視され 0 扱い。
// `max_pierces`    : pierces=true 時の追加貫通回数。max_pierces=2 → 3 体目で消滅。
// `homing`         : true = HitTestFn 後に SetHomingTarget で指定した位置に向き補正。
// `homing_strength`: 向き補正の強さ [0,1] (毎フレーム単位ベクトル LERP の係数)。
struct ProjectileDef {
    const char*    id              = nullptr;
    EProjectileKind kind            = EProjectileKind::Bullet;
    f32            speed           = 0.0f;
    f32            lifetime_sec    = 0.0f;
    f32            radius          = 0.0f;
    f32            gravity_y       = 0.0f;
    bool           pierces         = false;
    u32            max_pierces     = 0u;
    bool           homing          = false;
    f32            homing_strength = 0.0f;
};

// ---------------------------------------------------------------------------
// FProjectileId — 24bit index + 8bit gen を packed した opaque handle
// ---------------------------------------------------------------------------
// `_packed == 0` を invalid と定義 (gen は常に 1 以上で配る)。slot 再利用後の
// stale 参照は IsValid + 内部 gen 一致で検出する。
struct FProjectileId {
    u32 _packed = 0u;

    bool IsValid() const noexcept { return _packed != 0u; }

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215

    static FProjectileId Pack(u32 index, u8 gen) noexcept {
        FProjectileId h;
        h._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }
};

// ---------------------------------------------------------------------------
// ProjectileInstance — 個別弾の生データ (AllAlive で外部に渡される)
// ---------------------------------------------------------------------------
// `def_id` は RegisterDef で登録した string literal をそのまま指す (= 描画側で
// 弾種別アセットを引くキー)。owner_id / damage は spawn 毎に変化する値。
struct ProjectileInstance {
    const char* def_id      = nullptr;
    FVec2        position    {0.0f, 0.0f};
    FVec2        velocity    {0.0f, 0.0f};
    f32         elapsed_sec = 0.0f;   // 出生からの経過時間 (lifetime チェック用)
    u32         hit_count   = 0u;     // 当てた回数 (max_pierces+1 で despawn)
    u32         owner_id    = 0u;     // 撃った主体の識別子 (FNode2D::Id 等)
    f32         damage      = 0.0f;   // 1 hit 当たりのダメージ量
};

// HitTest コールバック型: alive な projectile 1 個に対して呼ばれる。
//   user            : SetHitTestFn で渡した context (FCollisionWorld2D 等)
//   proj            : 判定対象の projectile (位置 / 速度 / damage 参照可)
//   out_hit_target_id: hit したターゲットの識別子 (FHealthId.Index 等を想定)
//   out_damage_dealt: 実際に与えたダメージ (耐性計算後の最終値、HitCallback に渡る)
//   戻り値           : true = 命中した、false = 命中なし
// true 返却で hit_count++、hit_count >= 上限なら despawn + ExpireCallback も発火しない
// (= HitCallback のみで終わる)。
using HitTestFn = bool(*)(void* user, const ProjectileInstance& proj,
                          u32& out_hit_target_id, f32& out_damage_dealt) noexcept;

// 命中コールバック型: HitTestFn が true を返した直後に発火。
//   def_id          : 弾種識別子 (描画 / SE 振り分け用)
//   target_id       : HitTestFn の out_hit_target_id
//   damage          : HitTestFn の out_damage_dealt
// この中で FHealthSystem.ApplyDamage / VFX 生成 / SE 再生 を行う想定。
using HitCallback = void(*)(void* user, FProjectileId proj_id, const char* def_id,
                            u32 target_id, f32 damage) noexcept;

// 寿命切れコールバック型: lifetime_sec を超えた projectile に対して発火。
// 「命中で消えた弾」では発火しない (= HitCallback 経由のみ)。
using ExpireCallback = void(*)(void* user, FProjectileId proj_id, const char* def_id) noexcept;

// ---------------------------------------------------------------------------
// FProjectileSystem — 固定容量 pool + per-frame Tick
// ---------------------------------------------------------------------------
class FProjectileSystem {
public:
    FProjectileSystem() noexcept = default;
    ~FProjectileSystem() noexcept = default;

    // 非コピー・非ムーブ: AllAlive() / GetInstance() が内部 buffer の生ポインタを
    // 返すため、ムーブで実体アドレスが変わると外部参照が破綻する。
    FProjectileSystem(const FProjectileSystem&)            = delete;
    FProjectileSystem& operator=(const FProjectileSystem&) = delete;
    FProjectileSystem(FProjectileSystem&&)                 = delete;
    FProjectileSystem& operator=(FProjectileSystem&&)      = delete;

    // 初期化。max_concurrent で pool 上限を確定 (再 Init は no-op)。
    // 0 を渡した場合は default の 256 を採用 (誤呼出し防御)。
    void Init(u32 max_concurrent = 256u) noexcept;

    // 弾種を登録。def.id == nullptr / lifetime_sec <= 0 は無視。
    // 既に同じ id (アドレス一致 or 文字列一致) が登録済みなら上書き更新。
    void RegisterDef(const ProjectileDef& def) noexcept;

    // 発射。pool 満杯なら invalid を返す。def_id が未登録なら invalid を返す。
    FProjectileId Spawn(const char* def_id, FVec2 pos, FVec2 velocity,
                       u32 owner_id, f32 damage) noexcept;

    // 強制 despawn。stale handle は no-op。ExpireCallback は発火しない
    // (= 強制削除はサイレント、これは Destroy/Cancel 系の慣例に従う)。
    void Despawn(FProjectileId id) noexcept;

    // 現在 alive な projectile 数。
    u32 AliveCount() const noexcept { return _alive_count; }

    // pool 容量 (Init で確定)。
    u32 MaxCount() const noexcept { return _capacity; }

    // 個別 instance の参照取得。stale handle は nullptr。
    const ProjectileInstance* GetInstance(FProjectileId id) const noexcept;

    // 全 pool の生ポインタ。out_count には pool 全体サイズが入る (active かどうかは
    // 内部判定で gate されているので、user 側は alive_count 個までだけアクセスする
    // か、別 API を使うのが安全)。本 API では「描画用に AliveCount 個を取り出す」
    // ことが想定されており、AllAlive は AllAlive 専用 buffer を介して連続的に
    // alive 個だけを返す。
    const ProjectileInstance* AllAlive(u32& out_count) const noexcept;

    // dt 秒進める。各 projectile に対して:
    //   1) velocity に gravity を加算 (semi-implicit Euler)
    //   2) homing なら homing_target 方向に向き補正
    //   3) position += velocity * dt
    //   4) HitTestFn を呼んで命中チェック → 命中なら hit_count++ / HitCallback
    //      命中で max_pierces 超過なら despawn (ExpireCallback 不発火)
    //   5) elapsed_sec += dt → lifetime 超過なら despawn + ExpireCallback
    // dt <= 0 は no-op。
    void Tick(f32 dt) noexcept;

    // HitTest コールバック登録。fn == nullptr で登録解除。
    void SetHitTestFn(HitTestFn fn, void* user) noexcept;

    // 命中コールバック登録。cb == nullptr で登録解除。
    void SetOnHitCallback(HitCallback cb, void* user) noexcept;

    // 寿命切れコールバック登録。cb == nullptr で登録解除。
    void SetOnExpireCallback(ExpireCallback cb, void* user) noexcept;

    // homing 弾の追従目標位置を設定。homing=false の def を持つ弾に呼んでも
    // no-op (= 内部で homing フラグを再確認)。stale handle も no-op。
    void SetHomingTarget(FProjectileId id, FVec2 target_pos) noexcept;

    // 全 projectile を即座に破棄。callback / def 登録は維持。pool 容量も維持。
    // ExpireCallback は発火しない (= サイレント全消去)。
    void ClearAll() noexcept;

private:
    // pool の 1 slot。active=false で空き扱い。gen は generational handle 用。
    struct Slot {
        ProjectileInstance inst         {};
        FVec2               homing_tgt   {0.0f, 0.0f};
        bool               has_homing_target = false;
        bool               active       = false;
        u8                 gen          = 0u;
    };

    // 登録済 def の保持 (固定容量ではなく可変、id ポインタで識別)。
    TArray<ProjectileDef> _defs;
    // pool 本体 + alive 番号バッファ (AllAlive 用、Tick 終端で再構築)。
    TArray<Slot>                _slots;
    TArray<ProjectileInstance>  _alive_snapshot;  // AllAlive 専用 (連続配列で返す)

    u32 _capacity     = 0u;
    u32 _alive_count  = 0u;
    u32 _snapshot_dirty_size = 0u;  // _alive_snapshot に詰まっている個数

    HitTestFn      _hit_test_fn       = nullptr;
    void*          _hit_test_user     = nullptr;
    HitCallback    _on_hit            = nullptr;
    void*          _on_hit_user       = nullptr;
    ExpireCallback _on_expire         = nullptr;
    void*          _on_expire_user    = nullptr;

    // def_id (const char*) で登録 def を引く。見つからなければ nullptr。
    const ProjectileDef* FindDef(const char* id) const noexcept;
    // 内部: スロット引き (stale/inactive で nullptr)。
    Slot*       FindSlot(FProjectileId id) noexcept;
    const Slot* FindSlot(FProjectileId id) const noexcept;
    // 空き slot 取得。pool 満杯なら kInvalidIdx。
    u32 AcquireSlot() noexcept;
    // alive snapshot 再構築 (Tick 終端 / Spawn / Despawn 直後に呼ぶ)。
    void RebuildAliveSnapshot() noexcept;

    static constexpr u32 kInvalidIdx = 0xFFFFFFFFu;
};

} // namespace acs::game
