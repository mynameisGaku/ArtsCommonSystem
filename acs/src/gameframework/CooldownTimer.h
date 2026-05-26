// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCooldownTimer (スキル/アビリティ cooldown 管理)
//
// 複数の cooldown (スキル / アビリティ / 弾薬リロード等) を同時追跡する軽量
// マネージャ。各 cooldown は `label` (デバッグ / UI 表示用) と `duration_sec`
// を持ち、Tick で remaining を減算、0 到達で charged 状態になる。TryUse は
// charged のときだけ true を返して即時 reload を開始する (= remaining=duration)。
//
// 使い方:
//   class Player : public FNode2D {
//       acs::game::FCooldownTimer _cd;
//       acs::game::FCooldownId    _fireball;
//       acs::game::FCooldownId    _dash;
//
//       void OnEnter() noexcept override {
//           _fireball = _cd.Register("fireball", 3.0f);
//           _dash     = _cd.Register("dash",     0.8f);
//           _cd.SetOnReadyCallback(&Player::OnReady, this);
//           _cd.ForceReady(_fireball); // 初期は即撃てる
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _cd.Tick(dt);
//           if (input.JustPressed(EKey::Z) && _cd.TryUse(_fireball)) {
//               SpawnFireball();
//           }
//       }
//       static void OnReady(void* self, acs::game::FCooldownId id,
//                           const char* label) noexcept {
//           // HUD に "Ready!" を点滅させる等
//       }
//   };
//
// 設計:
//   ・**FCooldownId**: 24bit index + 8bit generation (FSceneTimer / FCollisionWorld2D
//     と同じパターン)。Unregister 後の slot 再利用で stale 検出可能。
//   ・**charged 状態**: `remaining <= 0` かつ `charged == true`。Tick 内で
//     `remaining` が 0 を跨いだ瞬間に `charged` が false→true に遷移し、
//     ReadyCallback が発火する。1 回の Tick 内で 1 回だけ。
//   ・**TryUse**: charged のとき remaining=duration、charged=false を立てて
//     true を返す。reload 中 (charged=false) は何もせず false。
//   ・**Register 直後**: charged=false、remaining=duration (= reload 中扱い)。
//     即時使用したい場合は ForceReady で明示。
//   ・**ReadyCallback**: 関数ポインタ (std::function 不使用)。設定は単一スロット。
//   ・**非コピー・非ムーブ**、全 noexcept、STL 不使用。
//   ・Tick 中に Register / Unregister が呼ばれても安全 (slot index アクセスで回す)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 24bit index + 8bit generation を packed した cooldown handle。
// _packed == 0 を invalid と定義 (gen は常に 1 以上)。
struct FCooldownId {
    u32 _packed = 0u;

    bool IsValid() const noexcept { return _packed != 0u; }

    // pack/unpack ヘルパ (manager 内部用)
    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215 個

    static FCooldownId Pack(u32 index, u8 gen) noexcept {
        FCooldownId h;
        h._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }

    constexpr bool operator==(FCooldownId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(FCooldownId o) const noexcept { return _packed != o._packed; }
};

// UI / デバッグ表示用の snapshot 型 (内部 FSlot とは別)
struct FCooldownState {
    const char* label    = nullptr; // Register に渡した文字列ポインタ (寿命は caller 管理)
    f32         duration = 0.0f;    // 設定された cooldown 長 (sec)
    f32         remaining = 0.0f;   // 残り時間 (sec)。0 以下で charged
    bool        charged  = false;   // 使用可能か
};

// charged 遷移時に呼ばれる callback。
// user: SetOnReadyCallback で渡した不透明ポインタ。
// id  : charged になった cooldown。
// label: Register に渡したラベル (デバッグ / HUD 用)。
using ReadyCallback = void(*)(void* user, FCooldownId id, const char* label) noexcept;

class FCooldownTimer {
public:
    FCooldownTimer() noexcept = default;
    ~FCooldownTimer() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ)
    FCooldownTimer(const FCooldownTimer&)            = delete;
    FCooldownTimer& operator=(const FCooldownTimer&) = delete;
    FCooldownTimer(FCooldownTimer&&)                 = delete;
    FCooldownTimer& operator=(FCooldownTimer&&)      = delete;

    // ----- 登録 / 解除 -----
    // duration_sec <= 0 は invalid id を返す。label は caller 側で寿命管理。
    // 登録直後は charged=false / remaining=duration (= reload 中扱い)。即時使用
    // したい場合は呼出側で ForceReady() を続けて呼ぶ。
    FCooldownId Register(const char* label, f32 duration_sec) noexcept;

    // slot を invalidate (gen 進む)。stale handle は以降全 API で false を返す。
    void Unregister(FCooldownId id) noexcept;

    // ----- 使用 / 状態取得 -----
    // charged なら remaining=duration / charged=false にして true を返す。
    // reload 中 (charged=false) は何もせず false。stale handle も false。
    bool TryUse(FCooldownId id) noexcept;

    bool IsCharged(FCooldownId id) const noexcept;
    f32  Remaining(FCooldownId id) const noexcept; // 0.0f if stale / charged
    f32  Progress (FCooldownId id) const noexcept; // [0,1]、1.0 = charged

    // ----- 直接操作 -----
    void ForceReady (FCooldownId id) noexcept;             // 即時 charged (ReadyCallback 発火)
    void Reset      (FCooldownId id) noexcept;             // 即時 duration にリセット (charged=false)
    void SetDuration(FCooldownId id, f32 new_duration_sec) noexcept; // 進行中 cooldown の長さ変更

    // ----- 一括 -----
    u32  Count() const noexcept { return _active_count; }
    void ClearAll() noexcept;

    // charged 遷移時 callback (単一スロット)。cb=nullptr で解除。
    void SetOnReadyCallback(ReadyCallback cb, void* user) noexcept {
        _ready_cb = cb;
        _ready_user = user;
    }

    // 毎フレーム呼ぶ。dt <= 0 は無視。
    // remaining が 0 を跨いだ瞬間に charged=true 遷移 + ReadyCallback 発火。
    void Tick(f32 dt) noexcept;

private:
    struct FSlot {
        const char* label     = nullptr;
        f32         duration  = 0.0f;
        f32         remaining = 0.0f;
        bool        charged   = false;
        bool        active    = false; // slot が現在使用中か
        u8          gen       = 0u;    // 0 = 未使用。Acquire で必ず 1 以上に。
    };

    u32        AcquireSlot() noexcept;
    FCooldownId MakeId(u32 index, u8 gen) const noexcept;
    // h が有効 (slot active + gen 一致) なら slot* を返す。さもなくば nullptr。
    FSlot*       Resolve(FCooldownId id) noexcept;
    const FSlot* Resolve(FCooldownId id) const noexcept;

    TArray<FSlot>   _slots;
    u32           _active_count = 0u;
    ReadyCallback _ready_cb     = nullptr;
    void*         _ready_user   = nullptr;
};

} // namespace acs::game
