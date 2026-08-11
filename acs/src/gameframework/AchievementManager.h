// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

class ISteamworksBridge;

/**
 * 起動時に 1 回登録される immutable な実績定義。
 *
 * @details
 * id は実 SDK のキー (例: Steamworks の "ACH_*") と一致させる。display_name /
 * description は UI 表示用。max_progress は累積カウンタの上限で、1 なら単一フラグ型
 * (Unlock 即時)、100 なら 100 ステップ進行型 (IncrementProgress 100 回で unlock)。
 * 文字列は所有せず static lifetime のリテラルを参照する。
 */
struct FAchievementDef {
    /** 実 SDK のキーと一致させる実績 id (所有しない static 文字列)。 */
    const char* id           = nullptr;

    /** UI 表示用の表示名 (所有しない static 文字列)。 */
    const char* display_name = nullptr;

    /** UI 表示用の説明文 (所有しない static 文字列)。 */
    const char* description  = nullptr;

    /** 累積カウンタの上限 (1 = 単発フラグ実績)。 */
    u32         max_progress = 1;

    /** true なら UI 側で未解除時にタイトル / 説明を伏字にする判定用フラグ。 */
    bool        secret       = false;
};

/**
 * 実行時に変化する 1 実績の状態。
 *
 * @details
 * FAchievementDef と同 index 位置で 1:1 対応する。id は Def 側のリテラルを参照コピー
 * して検索 / 表示で扱いやすくする。unlock_timestamp は CClock::MillisSinceStartup() の値
 * (起動からの ms) で、0 は「未 unlock」または「unlock 時に CClock 未取得」を表す。
 */
struct FAchievementProgress {
    /** 対応する Def の id を指す参照コピー (所有しない)。 */
    const char* id               = nullptr;

    /** 現在の累積進捗。 */
    u32         current_progress = 0;

    /** unlock に必要な進捗上限 (Def からコピー)。 */
    u32         max_progress     = 1;

    /** unlock 済みか。 */
    bool        unlocked         = false;

    /** unlock 時刻 (CClock::MillisSinceStartup()、起動からの ms。0 = 未 unlock)。 */
    u64         unlock_timestamp = 0;
};

/**
 * 実績の定義 + 進捗管理 + Storefront SDK 統合を行う高レベルマネージャ。
 *
 * @details
 * ゲームロジックから進捗を流し込み、max_progress に達したら自動 unlock し、attach 済みの
 * ISteamworksBridge があればプラットフォーム SDK へ伝搬する。定義 (immutable) と進捗 (mutable)
 * を別 TArray に 1:1 で持ち、id の線形検索で参照する。Bridge 未 attach 時はローカル進捗のみ
 * 追跡するオフラインモードで動作する。全 noexcept・非コピー・非ムーブ。
 */
class CAchievementManager {
public:
    /** 空のマネージャを構築する (実績は RegisterAchievement で登録)。 */
    CAchievementManager()  noexcept = default;

    /** 破棄する (実績配列は TArray が解放、Bridge は所有しないため触らない)。 */
    ~CAchievementManager() noexcept = default;

    /** コピー禁止 (Manager は唯一の状態 holder として扱うため)。 */
    CAchievementManager(const CAchievementManager&)            = delete;

    /** コピー代入も禁止。 */
    CAchievementManager& operator=(const CAchievementManager&) = delete;

    /** ムーブ禁止。 */
    CAchievementManager(CAchievementManager&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CAchievementManager& operator=(CAchievementManager&&)      = delete;

    /**
     * 実績定義を登録する (起動時に 1 度ずつ呼ぶ)。
     *
     * @details
     * 同 id の 2 重登録は no-op、id == nullptr も no-op (defensive)。max_progress = 0 は
     * 1 に丸めて「単発フラグ」として扱う。
     * @param def 登録する実績定義。
     */
    void RegisterAchievement(const FAchievementDef& def) noexcept;

    /**
     * 進捗を絶対値で設定する (max_progress 到達で自動 unlock + Bridge 送信)。
     *
     * @details 既に unlocked なら no-op。max_progress を超える値は max にクランプする。
     * @param id 対象実績の id。
     * @param progress 設定する進捗値。
     */
    void SetProgress(const char* id, u32 progress) noexcept;

    /**
     * 進捗を delta だけ加算する (max 到達で自動 unlock + Bridge 送信)。
     *
     * @details オーバーフロー / 上限超過はいずれも max_progress にクランプする。
     * @param id 対象実績の id。
     * @param delta 加算量 (既定 1)。
     */
    void IncrementProgress(const char* id, u32 delta = 1) noexcept;

    /**
     * 実績を即時 unlock する (current_progress を max に固定 + Bridge 送信)。
     *
     * @details 既に unlocked なら no-op (timestamp は最初の unlock を保持する)。
     * @param id 対象実績の id。
     */
    void Unlock(const char* id) noexcept;

    /**
     * 単一実績の進捗をリセットする (current_progress = 0, unlocked = false)。
     *
     * @details Bridge 側のリセットは呼ばない (SDK 仕様によっては不可能 / 危険なため、
     * 必要なら呼出側が Bridge を直接叩く)。
     * @param id 対象実績の id。
     */
    void Reset(const char* id) noexcept;

    /** 全実績の進捗をリセットする (デバッグ / テスト用、出荷ビルドでは UI から呼ばない)。 */
    void ResetAll() noexcept;

    /**
     * 実績が unlock 済みかを返す。
     *
     * @param id 対象実績の id。
     * @return unlock 済み (かつ id が存在) なら true。
     */
    bool IsUnlocked(const char* id) const noexcept;

    /**
     * 実績の現在進捗を返す。
     *
     * @param id 対象実績の id。
     * @return current_progress (id が見つからなければ 0)。
     */
    u32  GetProgress(const char* id) const noexcept;

    /**
     * 登録済み実績の総数を返す。
     *
     * @return 実績総数。
     */
    u32 TotalCount()    const noexcept;

    /**
     * unlock 済み実績の数を返す。
     *
     * @return unlock 済みの実績数。
     */
    u32 UnlockedCount() const noexcept;

    /**
     * 単一実績の状態を返す。
     *
     * @details 返却ポインタは次の RegisterAchievement() / ResetAll() で無効化される可能性がある。
     * @param id 対象実績の id。
     * @return 状態へのポインタ (見つからなければ nullptr)。
     */
    const FAchievementProgress* GetState(const char* id) const noexcept;

    /**
     * 全実績状態の生バッファを返す。
     *
     * @details 返却ポインタは TotalCount() 件の連続バッファで、
     * RegisterAchievement() / ResetAll() で無効化される。
     * @param out_count 件数の書き出し先。
     * @return 状態配列の先頭ポインタ。
     */
    const FAchievementProgress* AllStates(u32& out_count) const noexcept;

    /**
     * Storefront SDK ブリッジを attach / detach する。
     *
     * @details bridge = nullptr で detach (ローカル進捗のみ追跡するオフラインモード)。
     * attach 中は unlock の度に bridge->UnlockAchievement(id) を呼ぶ。
     * @param bridge 注入するブリッジ (所有しない、nullptr で detach)。
     */
    void AttachSteamworks(ISteamworksBridge* bridge) noexcept;

    /**
     * 毎フレーム更新フック (現状は完全に no-op の予約点)。
     *
     * @param dt 前フレームからの経過秒 (現状は無視)。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * id 文字列を全実績から線形検索する。
     *
     * @param id 探す実績の id。
     * @return 見つかった index、見つからなければ ~0u。
     */
    u32 FindIndex(const char* id) const noexcept;

    /**
     * 内部 unlock 共通処理 (Set/Increment/Unlock すべてが踏む)。
     *
     * @details index は FindIndex() で確定済みであること。Bridge への送信もここで実施する。
     * @param index unlock 対象の進捗インデックス。
     */
    void UnlockInternal(u32 index) noexcept;

    /** 実績定義の配列 (immutable、m_Progress と同 index で 1:1 対応)。 */
    TArray<FAchievementDef>      m_Defs;

    /** 実績進捗の配列 (mutable、m_Defs と同 index で 1:1 対応)。 */
    TArray<FAchievementProgress> m_Progress;

    /** Bridge 注入用 raw ポインタ (所有しない、寿命は呼出側責務)。 */
    ISteamworksBridge* m_Bridge = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAchievementManager = CAchievementManager;

} // namespace acs::game
