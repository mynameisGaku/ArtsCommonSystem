// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FAchievementManager (実績の定義 + 進捗 + Storefront 統合)
//
// ゲームロジック側から「ボス撃破、隠しエンディング到達、累計 100km 歩いた」等の
// 進捗を Achievement に流し込み、max_progress に達したら自動 unlock + プラット
// フォーム SDK (Steamworks / EOS / 各家庭機) へ伝搬する高レベルマネージャ。
// 低レイヤの FSteamworksBridge (純粋仮想 I/F) を seam として注入し、SDK 統合は
// ビルド時に差し替え可能 — Bridge 未 attach なら ローカル進捗だけ追跡する
// オフラインモードで動作する (テスト / Demo build 用)。
//
// 使い方:
//   FAchievementManager am;
//
//   // ゲーム起動時に全実績を 1 度だけ登録 (= 定義)。
//   am.RegisterAchievement({ "ACH_BOSS_01",       "First Boss",   "...", 1,    false });
//   am.RegisterAchievement({ "ACH_WALK_100KM",    "Marathon",     "...", 100,  false });
//   am.RegisterAchievement({ "ACH_SECRET_ENDING", "True Ending",  "...", 1,    true  });
//
//   // (任意) Storefront SDK を attach。null で detach 可。
//   am.AttachSteamworks(&acs::game::SteamworksBridgeStub::GetStub());
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
// 設計選択 (Pillar S Phase 2):
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
//   ・**FSteamworksBridge は seam 注入**: AttachSteamworks(nullptr) で detach。
//     attach 中は Unlock() が成功したら自動で Bridge::UnlockAchievement() を呼ぶ。
//     Bridge 戻り値の失敗 (= 未初期化 / 未実装) は ACS_LOG_WARN で記録し、
//     ローカル進捗は影響を受けない (オフライン → オンライン復帰時に再送する
//     責務は Phase 3+ で別途扱う)。
//   ・**max_progress に達したら自動 unlock**: SetProgress / IncrementProgress 経由
//     で current_progress が max に達した瞬間に unlocked = true、Bridge へ送信、
//     unlock_timestamp を Clock::MillisSinceStartup() で記録。Unlock() を直接
//     呼んだ場合は current_progress を max_progress に固定する。
//   ・**unlock_timestamp は起動からの ms**: 永続化 (Pillar J Serialize) と合わせ
//     て使う想定だが、Phase 2 では「起動以降のみ意味を持つ」相対時間で十分。
//     wall clock が必要になったら Phase 3 で `acs::SystemClock` を追加する。
//   ・**重複登録は黙って弾く**: 同 id を 2 度 RegisterAchievement しても 2 回目は
//     no-op。アセット二重ロード時の安全側挙動 (FModRegistry と揃える)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//
// 範囲外 (Phase 3+ で):
//   ・実績の永続化 (= ローカルセーブと合算)。現状は起動毎にリセットされる。
//     Pillar J Serialize と統合して `Save(slot)` / `Load(slot)` を追加予定。
//   ・統計 (Steamworks Stats) との連携。Bridge 側を Stat API 拡張してから。
//   ・Notification UI (右下からのトースト)。FUiLayer / NotificationCenter で別実装。
//   ・実績進捗の自動同期 (Bridge → ローカル)。今は片方向 (ローカル → Bridge)。
//   ・Tick で実時間タイマー実績 ("起動 1 時間で unlock" 等)。dt 蓄積は呼出側責務。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// FSteamworksBridge.h を引き込むと TResult<T> / Error 経由で foundation/TResult 等
// まで芋づるに引き込んでしまう。本ヘッダは公開 API のみ薄く保つ方針なので、
// 抽象 I/F は forward declare に留めて、実体 include は .cpp 側で行う。
class ISteamworksBridge;

// ---- FAchievementDef: 起動時 1 回登録される immutable な実績定義 ------------
// id は実 SDK のキー (例: Steamworks の "ACH_*") と一致させる。
// display_name / description は UI 表示用 (Pillar T Community でも参照)。
// max_progress は累積カウンタの上限。1 なら「単一フラグ型」(Unlock 即時)、
// 100 なら「100 ステップ進行型」(IncrementProgress 100 回で unlock)。
// secret = true は UI 側で未解除時にタイトル / 説明を伏字にする等の判定用 (Manager
// 自身は描画しない、単なるフラグ)。
struct FAchievementDef {
    const char* id           = nullptr;
    const char* display_name = nullptr;
    const char* description  = nullptr;
    u32         max_progress = 1;      // 1 = 単発フラグ実績
    bool        secret       = false;  // UI 側で未解除時に伏字にするか
};

// ---- FAchievementProgress: 実行時に変化する 1 実績の状態 -------------------
// FAchievementDef と同 index 位置で 1:1 対応する。id は Def 側のものを参照
// コピー (= 同じリテラルを指す) して、検索 / 表示で扱いやすくする。
// unlock_timestamp は Clock::MillisSinceStartup() の値 (起動からの ms)。
// 0 は「未 unlock」 or 「unlock 時に Clock 未取得」を表す。
struct FAchievementProgress {
    const char* id               = nullptr;
    u32         current_progress = 0;
    u32         max_progress     = 1;
    bool        unlocked         = false;
    u64         unlock_timestamp = 0;  // Clock::MillisSinceStartup() at unlock
};

// ---- FAchievementManager ---------------------------------------------------
class FAchievementManager {
public:
    FAchievementManager()  noexcept = default;
    ~FAchievementManager() noexcept = default;

    FAchievementManager(const FAchievementManager&)            = delete;
    FAchievementManager& operator=(const FAchievementManager&) = delete;
    FAchievementManager(FAchievementManager&&)                 = delete;
    FAchievementManager& operator=(FAchievementManager&&)      = delete;

    // ---- 定義登録 (起動時に 1 度ずつ) ------------------------------------
    // 同 id の 2 重登録は no-op、`id == nullptr` も no-op (defensive)。
    // max_progress = 0 は 1 に丸めて「単発フラグ」として扱う。
    void RegisterAchievement(const FAchievementDef& def) noexcept;

    // ---- 進捗操作 --------------------------------------------------------
    // 進捗を絶対値で設定。max_progress に達したら自動 unlock + Bridge 送信。
    // 既に unlocked なら no-op (max を超えて減らさない / 増やさない)。
    void SetProgress(const char* id, u32 progress) noexcept;

    // 進捗を delta だけ加算 (オーバーフロー時は max にクランプ)。
    void IncrementProgress(const char* id, u32 delta = 1) noexcept;

    // 即時 unlock (current_progress を max_progress に固定 + Bridge 送信)。
    // 既に unlocked なら no-op (timestamp は最初の unlock を保持する)。
    void Unlock(const char* id) noexcept;

    // 単一実績をリセット (current_progress = 0, unlocked = false)。
    // Bridge 側のリセットは呼ばない (SDK 仕様によっては不可能 / 危険なため、
    // 必要なら呼出側が Bridge を直接叩くこと)。
    void Reset(const char* id) noexcept;

    // 全実績をリセット (デバッグ / テスト用)。出荷ビルドでは UI から呼ばないこと。
    void ResetAll() noexcept;

    // ---- 照会 ------------------------------------------------------------
    bool IsUnlocked(const char* id) const noexcept;
    u32  GetProgress(const char* id) const noexcept;

    // 総数 / 既 unlock 数。UI の "23/50 unlocked" 表示用。
    u32 TotalCount()    const noexcept;
    u32 UnlockedCount() const noexcept;

    // 単一状態取得。見つからなければ nullptr。返却ポインタは次の
    // RegisterAchievement() / ResetAll() で無効化される可能性がある。
    const FAchievementProgress* GetState(const char* id) const noexcept;

    // 全状態の生バッファ。`out_count` に件数を書き出して返す。
    // 返却ポインタは TotalCount() 件の連続バッファ、RegisterAchievement() /
    // ResetAll() で無効化される。
    const FAchievementProgress* AllStates(u32& out_count) const noexcept;

    // ---- FSteamworksBridge 統合 -------------------------------------------
    // bridge = nullptr で detach (ローカル進捗のみ追跡するオフラインモード)。
    // attach 中は Unlock() の度に bridge->UnlockAchievement(id) を呼ぶ。
    void AttachSteamworks(ISteamworksBridge* bridge) noexcept;

    // ---- Tick ------------------------------------------------------------
    // 統計集計やタイムアウト系の更新フックを将来足すための予約。
    // Phase 2 では完全に no-op (dt は無視)。
    void Tick(f32 dt) noexcept;

private:
    // id 文字列を全実績から線形検索。見つからない場合 ~0u を返す。
    // .cpp 内 anonymous namespace の StrEq を使うため、メンバ関数のみ宣言する。
    u32 FindIndex(const char* id) const noexcept;

    // 内部 unlock 共通処理 (Set/Increment/Unlock すべてが踏む)。
    // index は FindIndex() で確定済みであること。Bridge への送信もここで実施。
    void UnlockInternal(u32 index) noexcept;

    TArray<FAchievementDef>      _defs;
    TArray<FAchievementProgress> _progress;

    // Bridge 注入用 raw ポインタ (所有しない)。寿命は呼出側 (= FApplication) 責務。
    ISteamworksBridge* _bridge = nullptr;
};

} // namespace acs::game
