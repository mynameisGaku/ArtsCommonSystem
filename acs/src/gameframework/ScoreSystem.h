// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/O — FScoreSystem (スコア累積 + コンボ + マルチプライヤ)
//
// アーケード / 高速アクション系で必要になる「スコア + コンボ計算 + マイル
// ストン通知」を 1 か所にまとめた高レベルマネージャ。ヒット連鎖 (NotifyHit)
// でコンボがインクリメントされ、AddScore() 時にコンボから算出した倍率を
// 掛けてスコアに加算する。NotifyMiss() / 一定時間 (combo_duration) 内に
// 次のヒットが無ければコンボはリセットされる。
//
// 設計位置付け:
//   ・Pillar R/O のスコア / 進行系。FAchievementManager (実績) や
//     FEconomyDirector (通貨) と並列のレイヤで、これら 3 つは
//     「ゲームプレイの数値報酬」を担当する。
//   ・スコア値 (u64) は AAA でも 4e18 を超えない想定で十分。
//     score * multiplier_x100 の中間計算で wrap しないよう、加算時に
//     u64 オーバーフロークランプ (~0ull) を入れる。
//   ・コンボ→倍率は SetMultiplierFn() で完全に差し替え可能。Manager は
//     ロジックの強制をせず、デフォルトは `1.0 + combo*0.1` を [1.0, 10.0]
//     にクランプした関数を採用 (アーケード系で広く使われる線形上昇)。
//
// 使い方:
//   FScoreSystem ss;
//   ss.Init();
//   ss.SetComboDuration(2.5f);                        // 任意
//   ss.RegisterMilestone(10000);                      // スコア 10k 通過で通知
//   ss.RegisterMilestone(100000);
//   ss.SetOnMilestoneCallback(&OnMilestone, user);
//
//   // 毎フレ
//   ss.Tick(dt);
//
//   // ヒット時 (敵撃破等)
//   ss.NotifyHit();
//   ss.AddScore("enemy.normal", 100);                 // base × current_multiplier
//
//   // 失敗時 (被弾 / ミス)
//   ss.NotifyMiss();
//
//   // UI 表示
//   u64 cur  = ss.CurrentScore();
//   u32 cmb  = ss.ComboCount();
//   f32 left = ss.ComboTimeRemaining();
//   f32 mul  = ss.ComboMultiplier();
//
// 設計選択 (Pillar R/O Phase 1):
//   ・**multiplier は ×100 整数で entry に記録**: ScoreEntry.multiplier_x100 は
//     例えば 250 = 2.5x。倍率を f32 で持つと bit 完全一致が取れず、Replay /
//     Telemetry での比較で偽差分が出るため整数化する。
//   ・**所有しない const char* category**: FAchievementManager と同設計で
//     呼出側 (= ゲームコード or リソースバンドル) が long lifetime を保証する
//     文字列リテラルを想定。Manager 側はコピーしない (STL <string> 禁止)。
//   ・**entry log は capped append (max 100)**: FScoreSystem は履歴を内蔵保持
//     するが、メモリを線形に増やさないため上限 100。100 件超は最古を捨てる
//     (= 末尾ベースの簡易リング)。詳細な分析が必要なら呼出側で Analytics に
//     流す責務 (= FEconomyDirector の callback 設計と思想を合わせる)。
//   ・**HighScore は Reset() で保持 / ClearAll() で破棄**: ゲームセッション
//     終了時に Reset で「累積スコアと combo は消すが best record は残す」と
//     いう典型挙動を表現。ClearAll はテスト / セーブデータリセット用。
//   ・**Milestone は「現在スコアが値を超えた瞬間に 1 度だけ通知」**: 既に
//     クリア済の milestone は再通知しない (Reset で再武装される)。milestone
//     値は登録順序非依存で AddScore のたびに全件 (= 線形) チェックする。
//     件数は通常 5〜20 なので線形で十分。
//   ・**MultiplierFn は C 関数ポインタ + noexcept**: STL <functional> 禁止。
//     nullptr 指定で内部デフォルトに戻す。combo を入力に f32 倍率を返す。
//   ・**MilestoneCallback は単一登録 + user pointer**: FEconomyDirector の
//     PurchaseCallback と同じ規約。複数 listener は呼出側で fan-out。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 2+ で):
//   ・永続化 (HighScore Save/Load) — Pillar J Serialize と統合予定。
//     現状は SetHighScore() で外部から注入する手動 wiring。
//   ・難易度補正 / グレード判定 — FDynamicDifficulty / FGameFlow と連携想定。
//   ・コンボ chain 種別 (perfect / good 等の品質スケール) — 必要になったら
//     NotifyHit(quality) 引数を追加して倍率ファンクションに渡す形に拡張。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- ScoreEntry: 1 回の AddScore() に対応する記録 --------------------------
// category         : 加算カテゴリ (例: "enemy.normal" / "bonus.air_combo")。文字列リテラル想定 (非所有)。
// base_value       : AddScore() に渡された素点 (multiplier 適用前)。
// weighted_value   : base * (multiplier_x100 / 100) の最終加算値 (実際に CurrentScore に足した量)。
// multiplier_x100  : この加算時点での倍率を ×100 整数化したもの (例: 250 = 2.5x)。
struct ScoreEntry {
    const char* category        = nullptr;
    u64         base_value      = 0;
    u64         weighted_value  = 0;
    u32         multiplier_x100 = 100;  // 100 = 1.0x
};

// ---- FScoreSystem ----------------------------------------------------------
class FScoreSystem {
public:
    // コンボ→倍率の差し替え関数型。nullptr 指定で内部デフォルトに戻す。
    // 戻り値 multiplier は呼出側で f32 として使われるが、内部では ×100 して
    // u32 に丸めて ScoreEntry に記録する。
    using MultiplierFn = f32(*)(u32 combo) noexcept;

    // マイルストン通過通知。MilestoneCallback はスコアが milestone_score を
    // 超えた最初の AddScore() でだけ呼ばれる (= 1 度だけ)。Reset() で再武装。
    //   user            : SetOnMilestoneCallback で渡したコンテキスト (Manager は所有しない)
    //   milestone       : 通過した milestone 値 (RegisterMilestone で登録した値そのもの)
    //   current_score   : 通過時点での CurrentScore() 値
    using MilestoneCallback = void(*)(void* user, u64 milestone, u64 current_score) noexcept;

    FScoreSystem()  noexcept = default;
    ~FScoreSystem() noexcept = default;

    FScoreSystem(const FScoreSystem&)            = delete;
    FScoreSystem& operator=(const FScoreSystem&) = delete;
    FScoreSystem(FScoreSystem&&)                 = delete;
    FScoreSystem& operator=(FScoreSystem&&)      = delete;

    // ---- 初期化 -----------------------------------------------------------
    // combo_duration をデフォルト (3 秒)、multiplier 関数をデフォルトに戻し、
    // 現在スコア / コンボ / entry log / milestone 通過状態をすべて 0 / 空に
    // クリアする。HighScore は保持する (= ゲーム再起動時に外部から SetHighScore
    // で復元される想定)。
    void Init() noexcept;

    // ---- スコア加算 -------------------------------------------------------
    // base × current_multiplier を CurrentScore に加算し、ScoreEntry log に
    // 記録する (max 100 件 — 超えると最古を捨てる)。
    //   ・base_value == 0     : 加算 0 だが log には記録する (倍率の履歴として
    //                           意味があるケースがあるため。空打ち判定は category
    //                           で表現)。
    //   ・category == nullptr : log の category 欄も nullptr で記録。加算自体は
    //                           通常通り行う。
    //   ・u64 オーバーフロー  : ~0ull にクランプ (CurrentScore / weighted_value
    //                           共に)。
    // 加算後に登録済 milestone をすべて走査し、初回通過時に callback を発火する。
    void AddScore(const char* category, u64 base_value) noexcept;

    // ---- スコア照会 -------------------------------------------------------
    u64 CurrentScore() const noexcept { return m_CurrentScore; }
    u64 HighScore()    const noexcept { return m_HighScore; }

    // 外部から HighScore を注入 (Save/Load から復元)。score が現在の
    // HighScore より低い値であっても上書きする (= 強制セット)。永続化との
    // 連携を意識した最低限の API。
    void SetHighScore(u64 score) noexcept { m_HighScore = score; }

    // ---- コンボ -----------------------------------------------------------
    u32 ComboCount()          const noexcept { return m_ComboCount; }
    f32 ComboTimeRemaining()  const noexcept { return m_ComboTimer; }

    // ヒット成功 (敵撃破 / コンボ継続)。combo_count++、combo_timer をリセット
    // (= combo_duration に再設定)。max 件数の上限はかけない (u32 max まで)。
    void NotifyHit() noexcept;

    // ミス / 被弾。combo_count = 0、combo_timer = 0。AddScore 時の倍率も
    // デフォルト (1.0x) に戻る。
    void NotifyMiss() noexcept;

    // ---- 駆動 -------------------------------------------------------------
    // 毎フレ呼び、combo_timer を dt 秒減算する。0 を切ったら NotifyMiss と同じ
    // 効果でコンボリセット。dt <= 0 は no-op。
    void Tick(f32 dt) noexcept;

    // コンボのタイムアウト秒を変更。sec <= 0 は意味不明なので 0 に丸める
    // (= 「コンボ無効化」相当: NotifyHit 直後に Tick で即リセット)。
    void SetComboDuration(f32 sec) noexcept;

    // 現時点での combo→倍率 (= 関数適用結果)。AddScore は内部でこの値を
    // 参照して加算する。UI のリアルタイム倍率表示にも使う。
    f32 ComboMultiplier() const noexcept;

    // 倍率関数を差し替える。nullptr で内部デフォルト (1.0 + combo*0.1, clamp
    // [1.0, 10.0]) に戻る。
    void SetMultiplierFn(MultiplierFn fn) noexcept;

    // ---- entry log --------------------------------------------------------
    u32 EntryCount() const noexcept;

    // 全 entry の生バッファ。`out_count` に件数を書き出して返す。
    // 返却ポインタは EntryCount() 件の連続バッファ、次の AddScore() / Reset() /
    // ClearAll() で無効化される可能性がある。
    const ScoreEntry* AllEntries(u32& out_count) const noexcept;

    // ---- milestone --------------------------------------------------------
    // 通過時に MilestoneCallback を発火するスコア値を登録。重複値は黙って弾く
    // (no-op + WARN)。milestone_score == 0 は意味不明なので no-op。
    // 登録順序非依存 (= AddScore のたびに全件走査して通過判定する)。
    void RegisterMilestone(u64 milestone_score) noexcept;

    // cb == nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnMilestoneCallback(MilestoneCallback cb, void* user) noexcept;

    // ---- リセット ---------------------------------------------------------
    // 現在スコア / コンボ / entry log / milestone 通過状態をクリア。
    // HighScore / milestone 定義 / multiplier 関数 / callback はすべて保持。
    // ゲームセッション開始時に呼ぶ典型 reset。
    void Reset() noexcept;

    // すべてをクリア (HighScore / milestone 定義 / callback も含む完全リセット)。
    // テスト / セーブデータ削除時に使う。
    void ClearAll() noexcept;

private:
    // ScoreEntry log の上限件数 (capped append、超えると最古を捨てる)。
    static constexpr u32 kMaxEntries = 100;

    // 内部 default multiplier 関数。1.0 + combo*0.1 を [1.0, 10.0] に clamp。
    // public な static にしておくと unit test 側で直接比較できるが、現状は
    // 内部実装としてのみ使う。
    static f32 DefaultMultiplierFn(u32 combo) noexcept;

    // 加算後の milestone 走査。CurrentScore() が新しい値に更新された直後に
    // 呼ばれる。m_Milestones / m_MilestoneHit を 1:1 並行 TArray で管理する。
    void CheckMilestones() noexcept;

    // ScoreEntry を log に push (capped append)。
    void PushEntry(const ScoreEntry& e) noexcept;

    // 状態
    u64 m_CurrentScore = 0;
    u64 m_HighScore    = 0;
    u32 m_ComboCount   = 0;
    f32 m_ComboTimer   = 0.0f;
    f32 m_ComboDuration = 3.0f;

    // entry log。最大 kMaxEntries 件で capped append。
    TArray<ScoreEntry> m_Entries;

    // milestone 定義と通過済フラグ。1:1 並行 TArray (= 同 index で対応)。
    TArray<u64>  m_Milestones;
    TArray<bool> m_MilestoneHit;

    // 差し替え可能な倍率関数 (nullptr = 内部デフォルト)。
    MultiplierFn m_MultiplierFn = nullptr;

    // milestone callback (C 関数ポインタ + user)。Manager は user を所有しない。
    MilestoneCallback m_OnMilestone      = nullptr;
    void*             m_OnMilestoneUser = nullptr;
};

} // namespace acs::game
