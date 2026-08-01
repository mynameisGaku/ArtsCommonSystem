// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — CAchievementManager (実績の定義 + 進捗 + Storefront 統合)
//
// ゲームロジック側から「ボス撃破、隠しエンディング到達、累計 100km 歩いた」等の
// 進捗を Achievement に流し込み、max_progress に達したら自動 unlock + プラット
// フォーム SDK (Steamworks / EOS / 各家庭機) へ伝搬する高レベルマネージャ。
// 低レイヤの ISteamworksBridge (純粋仮想 I/F) を seam として注入し、SDK 統合は
// ビルド時に差し替え可能 — Bridge 未 attach なら ローカル進捗だけ追跡する
// オフラインモードで動作する (テスト / Demo build 用)。
//
// 使い方:
//   CAchievementManager am;
//
//   // ゲーム起動時に全実績を 1 度だけ登録 (= 定義)。
//   am.RegisterAchievement({ "ACH_BOSS_01",       "First Boss",   "...", 1,    false });
//   am.RegisterAchievement({ "ACH_WALK_100KM",    "Marathon",     "...", 100,  false });
//   am.RegisterAchievement({ "ACH_SECRET_ENDING", "True Ending",  "...", 1,    true  });
//
//   // (任意) Storefront SDK を attach。null で detach 可。
//   am.AttachSteamworks(&acs::game::CSteamworksBridgeStub::GetStub());
//
//   // ゲームロジック内で進捗更新。
//   am.IncrementProgress("ACH_WALK_100KM", 1);     // 1km 歩いた
//   am.Unlock("ACH_BOSS_01");                      // ボス撃破 (即時 unlock)
//
//   // UI / Pause 画面で一覧表示。
//   u32 n = 0;
//   const FAchievementProgress* states = am.AllStates(n);
//   for (u32 i = 0; i < n; ++i) { /* secret は別扱いで表示 */ }
//
// 設計選択 (Pillar S):
//   ・**定義 / 進捗を別配列で持つ**: FAchievementDef はゲーム起動時に固定で登録
//     される immutable な定義 (id / 表示名 / max_progress / secret)。
//     FAchievementProgress は実行時に変化する状態 (current_progress / unlocked /
//     unlock_timestamp)。両者を別 TArray に分けることで「定義は const、状態は
//     mutable」が型レベルで明確になる。1:1 対応で同じ index を共有する。
//   ・**所有しない const char***: id / display_name / description は呼び出し側
//     (ゲームコード or リソースバンドル) が保証する static lifetime の文字列
//     リテラルを想定。Manager 側ではコピーしない (STL <string> 禁止)。
//   ・**線形検索**: 実績件数は AAA タイトルでも通常 50〜300 程度のため、
//     TArray<T> の per-byte 文字列比較 + 線形走査で十分。ハッシュテーブル化は
//     プロファイラで実測されたら検討。
//   ・**ISteamworksBridge は seam 注入**: AttachSteamworks(nullptr) で detach。
//     attach 中は Unlock() が成功したら自動で Bridge::UnlockAchievement() を呼ぶ。
//     Bridge 戻り値の失敗 (= 未初期化 / 未実装) は ACS_LOG_WARN で記録し、
//     ローカル進捗は影響を受けない。
//   ・**max_progress に達したら自動 unlock**: SetProgress / IncrementProgress 経由
//     で current_progress が max に達した瞬間に unlocked = true、Bridge へ送信、
//     unlock_timestamp を FClock::MillisSinceStartup() で記録。Unlock() を直接
//     呼んだ場合は current_progress を max_progress に固定する。
//   ・**unlock_timestamp は起動からの ms**: 永続化 (Pillar J Serialize) と合わせ
//     て使う想定だが、「起動以降のみ意味を持つ」相対時間で十分。
//   ・**重複登録は黙って弾く**: 同 id を 2 度 RegisterAchievement しても 2 回目は
//     no-op。アセット二重ロード時の安全側挙動 (CModRegistry と揃える)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//
// 範囲外:
//   ・実績の永続化 (= ローカルセーブと合算)。現状は起動毎にリセットされる。
//   ・統計 (Steamworks Stats) との連携。
//   ・Notification UI (右下からのトースト)。
//   ・実績進捗の自動同期 (Bridge → ローカル)。今は片方向 (ローカル → Bridge)。
//   ・Tick で実時間タイマー実績 ("起動 1 時間で unlock" 等)。dt 蓄積は呼出側責務。
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
 * して検索 / 表示で扱いやすくする。unlock_timestamp は FClock::MillisSinceStartup() の値
 * (起動からの ms) で、0 は「未 unlock」または「unlock 時に FClock 未取得」を表す。
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

    /** unlock 時刻 (FClock::MillisSinceStartup()、起動からの ms。0 = 未 unlock)。 */
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
