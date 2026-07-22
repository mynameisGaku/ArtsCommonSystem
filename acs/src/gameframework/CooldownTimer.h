// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCooldownTimer (スキル/アビリティ cooldown 管理)
//
// 複数の cooldown (スキル / アビリティ / 弾薬リロード等) を同時追跡する軽量
// マネージャ。各 cooldown は `label` (デバッグ / UI 表示用) と `duration_sec`
// を持ち、Tick で remaining を減算、0 到達で charged 状態になる。TryUse は
// charged のときだけ true を返して即時 reload を開始する (= remaining=duration)。
//
// 使い方:
//   class APlayer : public ANode {
//       acs::game::FCooldownTimer m_Cd;
//       acs::game::FCooldownId   m_Fireball;
//       acs::game::FCooldownId   m_Dash;
//
//       void OnEnter() noexcept override {
//           m_Fireball = m_Cd.Register("fireball", 3.0f);
//           m_Dash     = m_Cd.Register("dash",     0.8f);
//           m_Cd.SetOnReadyCallback(&APlayer::OnReady, this);
//           m_Cd.ForceReady(m_Fireball); // 初期は即撃てる
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Cd.Tick(dt);
//           if (input.JustPressed(EKey::Z) && m_Cd.TryUse(m_Fireball)) {
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

/**
 * 24bit index + 8bit generation を packed した cooldown handle。
 *
 * @details m_Packed == 0 を invalid と定義する (gen は常に 1 以上を保つ)。
 */
struct FCooldownId {
    /** index と generation を packed した値 (0 = invalid)。 */
    u32 m_Packed = 0u;

    /**
     * handle が有効かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index 部のビット幅。 */
    static constexpr u32 kIndexBits = 24u;

    /** index 部を取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** index の最大値 (= 同時に扱える cooldown 数の上限、16777215)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * index と generation を 1 つの handle へ pack する。
     *
     * @param index slot インデックス (下位 24bit)。
     * @param gen generation (上位 8bit)。
     * @return packed した FCooldownId。
     */
    static FCooldownId Pack(u32 index, u8 gen) noexcept {
        FCooldownId h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * index 部を取り出す。
     *
     * @return slot インデックス (下位 24bit)。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * generation 部を取り出す。
     *
     * @return generation (上位 8bit)。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }

    /**
     * 等値比較。
     *
     * @param o 比較対象の handle。
     * @return packed 値が等しければ true。
     */
    constexpr bool operator==(FCooldownId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等値比較。
     *
     * @param o 比較対象の handle。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FCooldownId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * UI / デバッグ表示用の cooldown スナップショット (内部 FSlot とは別の公開型)。
 */
struct FCooldownState {
    /** Register に渡した文字列ポインタ (寿命は caller 管理)。 */
    const char* label    = nullptr;

    /** 設定された cooldown 長 (sec)。 */
    f32         duration = 0.0f;

    /** 残り時間 (sec)。0 以下で charged。 */
    f32         remaining = 0.0f;

    /** 使用可能 (charged) なら true。 */
    bool        charged  = false;
};

/**
 * charged 遷移時に呼ばれる callback の型。
 *
 * @param user SetOnReadyCallback で渡した不透明ポインタ。
 * @param id charged になった cooldown の handle。
 * @param label Register に渡したラベル (デバッグ / HUD 用)。
 */
using ReadyCallback = void(*)(void* user, FCooldownId id, const char* label) noexcept;

/**
 * 複数 cooldown (スキル / アビリティ / 弾薬リロード等) を同時追跡する軽量マネージャ。
 *
 * @details
 * 各 cooldown は label と duration を持ち、Tick で remaining を減算、0 到達で charged に
 * 遷移して ReadyCallback を発火する。TryUse は charged のときだけ true を返し、即時 reload
 * を開始する。handle は 24bit index + 8bit generation で stale 検出する。
 */
class FCooldownTimer {
public:
    /** 空のマネージャを構築する。 */
    FCooldownTimer() noexcept = default;

    /** 破棄する。 */
    ~FCooldownTimer() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐため)。 */
    FCooldownTimer(const FCooldownTimer&)            = delete;

    /** コピー代入も禁止。 */
    FCooldownTimer& operator=(const FCooldownTimer&) = delete;

    /** ムーブ禁止。 */
    FCooldownTimer(FCooldownTimer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FCooldownTimer& operator=(FCooldownTimer&&)      = delete;

    /**
     * 新しい cooldown を登録し、handle を返す。
     *
     * @details
     * 登録直後は charged=false / remaining=duration (= reload 中扱い)。即時使用したい場合は
     * 呼出側で続けて ForceReady() を呼ぶ。
     * @param label デバッグ / UI 表示用ラベル (寿命は caller 側で管理)。
     * @param duration_sec cooldown 長 (sec)。0 以下なら invalid id を返す。
     * @return 登録した cooldown の handle (失敗時は invalid)。
     */
    FCooldownId Register(const char* label, f32 duration_sec) noexcept;

    /**
     * cooldown を解除し slot を invalidate する (generation を進める)。
     *
     * @details 以降この handle は stale となり、全 API で無効扱いになる。
     * @param id 解除する cooldown の handle。
     */
    void Unregister(FCooldownId id) noexcept;

    /**
     * charged なら消費して reload を開始する。
     *
     * @details charged のとき remaining=duration / charged=false にして true を返す。reload 中
     * (charged=false) や stale handle は何もせず false。
     * @param id 使用する cooldown の handle。
     * @return 使用できたら true。
     */
    bool TryUse(FCooldownId id) noexcept;

    /**
     * cooldown が charged (使用可能) かを返す。
     *
     * @param id 対象の handle。
     * @return charged なら true (stale handle は false)。
     */
    bool IsCharged(FCooldownId id) const noexcept;

    /**
     * 残り cooldown 秒数を返す。
     *
     * @param id 対象の handle。
     * @return 残り秒数 (charged / stale handle は 0)。
     */
    f32  Remaining(FCooldownId id) const noexcept;

    /**
     * cooldown 進行率を返す。
     *
     * @param id 対象の handle。
     * @return [0,1] の進行率 (1.0 = charged、stale handle は 0)。
     */
    f32  Progress (FCooldownId id) const noexcept;

    /**
     * cooldown を即時 charged にする (ReadyCallback を発火)。
     *
     * @param id 対象の handle。
     */
    void ForceReady (FCooldownId id) noexcept;

    /**
     * cooldown を即時 duration にリセットする (charged=false)。
     *
     * @param id 対象の handle。
     */
    void Reset      (FCooldownId id) noexcept;

    /**
     * 進行中 cooldown の長さを変更する。
     *
     * @details charged は維持し、reload 中で remaining > new_duration なら new_duration へ
     * クランプする (短縮しても進捗を巻き戻さない)。
     * @param id 対象の handle。
     * @param new_duration_sec 新しい cooldown 長 (sec)。0 以下は無視。
     */
    void SetDuration(FCooldownId id, f32 new_duration_sec) noexcept;

    /**
     * 現在アクティブな cooldown 数を返す。
     *
     * @return アクティブな cooldown 数。
     */
    u32  Count() const noexcept { return m_ActiveCount; }

    /** 全 cooldown をアクティブ解除する (slot は再利用可能になる)。 */
    void ClearAll() noexcept;

    /**
     * charged 遷移時の callback を設定する (単一スロット)。
     *
     * @param cb 発火させる callback (nullptr で解除)。
     * @param user callback に渡す不透明ポインタ (所有しない)。
     */
    void SetOnReadyCallback(ReadyCallback cb, void* user) noexcept {
        _ready_cb = cb;
        _ready_user = user;
    }

    /**
     * 毎フレーム呼んで全 cooldown を進める。
     *
     * @details remaining が 0 を跨いだ瞬間に charged=true へ遷移し ReadyCallback を発火する。
     * @param dt 経過秒。0 以下は無視。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * 1 cooldown 分の内部状態。
     */
    struct FSlot {
        /** Register に渡したラベル (非所有)。 */
        const char* label     = nullptr;

        /** 設定された cooldown 長 (sec)。 */
        f32         duration  = 0.0f;

        /** 残り時間 (sec)。 */
        f32         remaining = 0.0f;

        /** 使用可能 (charged) なら true。 */
        bool        charged   = false;

        /** slot が現在使用中なら true。 */
        bool        active    = false;

        /** generation (0 = 未使用、Acquire で必ず 1 以上にする)。 */
        u8          gen       = 0u;
    };

    /**
     * inactive な slot を確保する (なければ末尾追加)。
     *
     * @return 確保した slot の index (上限到達時は kMaxIndex を返す)。
     */
    u32        AcquireSlot() noexcept;

    /**
     * index と generation から handle を作る。
     *
     * @param index slot インデックス。
     * @param gen generation。
     * @return 構築した FCooldownId。
     */
    FCooldownId MakeId(u32 index, u8 gen) const noexcept;

    /**
     * handle を解決して slot を返す。
     *
     * @param id 解決する handle。
     * @return slot active + gen 一致なら slot へのポインタ、さもなくば nullptr。
     */
    FSlot*       Resolve(FCooldownId id) noexcept;

    /**
     * handle を解決して const slot を返す。
     *
     * @param id 解決する handle。
     * @return slot active + gen 一致なら const slot へのポインタ、さもなくば nullptr。
     */
    const FSlot* Resolve(FCooldownId id) const noexcept;

    /** cooldown slot 配列 (index で参照)。 */
    TArray<FSlot>   m_Slots;

    /** アクティブな cooldown 数。 */
    u32           m_ActiveCount = 0u;

    /** charged 遷移時に発火する callback。 */
    ReadyCallback _ready_cb     = nullptr;

    /** callback に渡す不透明ポインタ。 */
    void*         _ready_user   = nullptr;
};

} // namespace acs::game
