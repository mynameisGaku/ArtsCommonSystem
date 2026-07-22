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
// 設計選択 (Pillar R/O):
//   ・**multiplier は ×100 整数で entry に記録**: FScoreEntry.multiplier_x100 は
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
// 範囲外:
//   ・永続化 (HighScore Save/Load) — Pillar J Serialize と統合予定。
//     現状は SetHighScore() で外部から注入する手動 wiring。
//   ・難易度補正 / グレード判定 — FDynamicDifficulty / FGameFlow と連携想定。
//   ・コンボ chain 種別 (perfect / good 等の品質スケール) — 必要になったら
//     NotifyHit(quality) 引数を追加して倍率ファンクションに渡す形に拡張。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 1 回の AddScore() に対応する記録。
 *
 * @details
 * category は文字列リテラル想定 (非所有)。weighted_value は base に倍率を掛けて
 * 実際に CurrentScore へ足した値、multiplier_x100 はその加算時点の倍率の ×100 整数。
 */
struct FScoreEntry {
    /** 加算カテゴリ (例: "enemy.normal" / "bonus.air_combo")。文字列リテラル想定 (非所有)。 */
    const char* category        = nullptr;

    /** AddScore() に渡された素点 (倍率適用前)。 */
    u64         base_value      = 0;

    /** base * (multiplier_x100 / 100) の最終加算値 (実際に CurrentScore に足した量)。 */
    u64         weighted_value  = 0;

    /** この加算時点での倍率を ×100 整数化したもの (例: 250 = 2.5x、既定 100 = 1.0x)。 */
    u32         multiplier_x100 = 100;
};

/**
 * スコア累積 + コンボ計算 + マイルストン通知をまとめた高レベルマネージャ。
 *
 * @details
 * アーケード / 高速アクション系向け。NotifyHit でコンボがインクリメントされ、
 * AddScore() 時にコンボから算出した倍率を掛けて加算する。NotifyMiss() または
 * combo_duration 内に次のヒットが無ければコンボがリセットされる。倍率は entry に
 * ×100 整数で記録し (Replay / Telemetry の偽差分回避)、category は所有しない
 * const char*、entry log は max 100 件の capped append。全 noexcept で非コピー・
 * 非ムーブ。
 */
class FScoreSystem {
public:
    /**
     * コンボ数から倍率を算出する差し替え可能な関数型。
     *
     * @details
     * 戻り値は呼出側で f32 として扱われるが、内部では ×100 して u32 に丸めて
     * FScoreEntry に記録する。SetMultiplierFn に nullptr を渡すと内部デフォルトに戻る。
     */
    using MultiplierFn = f32(*)(u32 combo) noexcept;

    /**
     * マイルストン通過を通知するコールバック型。
     *
     * @details
     * スコアが milestone_score を超えた最初の AddScore() でだけ呼ばれ (1 度だけ)、
     * Reset() で再武装される。
     */
    using MilestoneCallback = void(*)(void* user, u64 milestone, u64 current_score) noexcept;

    /** 空状態で構築する。 */
    FScoreSystem()  noexcept = default;

    /** 破棄する。 */
    ~FScoreSystem() noexcept = default;

    /** コピー禁止 (他 Manager 系と統一)。 */
    FScoreSystem(const FScoreSystem&)            = delete;

    /** コピー代入も禁止。 */
    FScoreSystem& operator=(const FScoreSystem&) = delete;

    /** ムーブ禁止。 */
    FScoreSystem(FScoreSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FScoreSystem& operator=(FScoreSystem&&)      = delete;

    /**
     * 最初の状態に初期化する。
     *
     * @details
     * combo_duration をデフォルト (3 秒)、multiplier 関数 / milestone callback を
     * デフォルト (nullptr) に戻し、現在スコア / コンボ / entry log / milestone 定義 /
     * 通過状態をすべてクリアする。HighScore は保持する (= 外部から SetHighScore で
     * 復元される想定)。
     */
    void Init() noexcept;

    /**
     * base × 現在倍率を CurrentScore に加算し、entry log に記録する。
     *
     * @details
     * base_value == 0 でも倍率履歴として log には記録する。category == nullptr は
     * log の category 欄も nullptr で記録する。u64 オーバーフローは ~0ull に
     * クランプする。加算後に登録済 milestone を全件走査し、初回通過時に callback を
     * 発火する。
     * @param category 加算カテゴリ (非所有、nullptr 可)。
     * @param base_value 倍率適用前の素点。
     */
    void AddScore(const char* category, u64 base_value) noexcept;

    /**
     * 現在の累積スコアを返す。
     *
     * @return 現在スコア。
     */
    u64 CurrentScore() const noexcept { return m_CurrentScore; }

    /**
     * ハイスコアを返す。
     *
     * @return これまでの最高スコア。
     */
    u64 HighScore()    const noexcept { return m_HighScore; }

    /**
     * HighScore を外部から注入する (Save/Load からの復元用)。
     *
     * @details 現在値より低い値でも上書きする (強制セット)。
     * @param score 設定する HighScore 値。
     */
    void SetHighScore(u64 score) noexcept { m_HighScore = score; }

    /**
     * 現在のコンボ数を返す。
     *
     * @return 連続ヒット回数。
     */
    u32 ComboCount()          const noexcept { return m_ComboCount; }

    /**
     * コンボ継続の残り時間を返す。
     *
     * @return combo_timer の残り秒。
     */
    f32 ComboTimeRemaining()  const noexcept { return m_ComboTimer; }

    /**
     * ヒット成功を通知する (敵撃破 / コンボ継続)。
     *
     * @details combo_count を 1 増やし (u32 max でクランプ)、combo_timer を combo_duration に再設定する。
     */
    void NotifyHit() noexcept;

    /**
     * ミス / 被弾を通知する。
     *
     * @details combo_count を 0、combo_timer を 0 にしてコンボをリセットする (倍率もデフォルトに戻る)。
     */
    void NotifyMiss() noexcept;

    /**
     * 毎フレーム呼んで combo_timer を減算する。
     *
     * @details 0 を切ったら NotifyMiss と同じ効果でコンボをリセットする。dt <= 0 は no-op。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * コンボのタイムアウト秒を変更する。
     *
     * @details sec <= 0 は 0 に丸める (コンボ無効化相当: NotifyHit 直後の Tick で即リセット)。
     * @param sec 新しい combo_duration (秒)。
     */
    void SetComboDuration(f32 sec) noexcept;

    /**
     * 現時点での combo→倍率 (倍率関数の適用結果) を返す。
     *
     * @details AddScore は内部でこの値を参照して加算する。UI のリアルタイム倍率表示にも使える。
     * @return 現在の倍率。
     */
    f32 ComboMultiplier() const noexcept;

    /**
     * 倍率関数を差し替える。
     *
     * @param fn 新しい倍率関数。nullptr で内部デフォルト (1.0 + combo*0.1, clamp [1.0, 10.0]) に戻る。
     */
    void SetMultiplierFn(MultiplierFn fn) noexcept;

    /**
     * entry log の件数を返す。
     *
     * @return 記録済み FScoreEntry の数。
     */
    u32 EntryCount() const noexcept;

    /**
     * 全 entry の生バッファを返す。
     *
     * @details 返却ポインタは EntryCount() 件の連続バッファで、次の AddScore() / Reset() / ClearAll() で無効化され得る。
     * @param out_count 件数の書き出し先。
     * @return entry バッファの先頭ポインタ。
     */
    const FScoreEntry* AllEntries(u32& out_count) const noexcept;

    /**
     * 通過時に MilestoneCallback を発火するスコア値を登録する。
     *
     * @details
     * 重複値は黙って弾く (no-op + WARN)。milestone_score == 0 は no-op。登録順序
     * 非依存 (AddScore のたびに全件走査して通過判定する)。
     * @param milestone_score 通過を通知するスコア閾値。
     */
    void RegisterMilestone(u64 milestone_score) noexcept;

    /**
     * マイルストン通過コールバックを設定する。
     *
     * @param cb 通過時に呼ぶコールバック (nullptr で detach)。
     * @param user cb に渡すコンテキスト (この Manager は所有しない)。
     */
    void SetOnMilestoneCallback(MilestoneCallback cb, void* user) noexcept;

    /**
     * セッション開始時の典型 reset。
     *
     * @details
     * 現在スコア / コンボ / entry log / milestone 通過状態をクリアする。HighScore /
     * milestone 定義 / multiplier 関数 / callback はすべて保持する。
     */
    void Reset() noexcept;

    /**
     * すべてをクリアする完全リセット。
     *
     * @details HighScore / milestone 定義 / callback も含めて消す。テスト / セーブデータ削除時に使う。
     */
    void ClearAll() noexcept;

private:
    /** FScoreEntry log の上限件数 (capped append、超えると最古を捨てる)。 */
    static constexpr u32 kMaxEntries = 100;

    /**
     * 内部デフォルトの倍率関数。
     *
     * @details 1.0 + combo*0.1 を [1.0, 10.0] にクランプして返す。
     * @param combo 現在のコンボ数。
     * @return クランプ済みの倍率。
     */
    static f32 DefaultMultiplierFn(u32 combo) noexcept;

    /**
     * 加算後に milestone を走査し、初回通過時に callback を発火する。
     *
     * @details CurrentScore が更新された直後に呼ばれる。m_Milestones / m_MilestoneHit を 1:1 並行 TArray で管理する。
     */
    void CheckMilestones() noexcept;

    /**
     * FScoreEntry を log に push する (capped append)。
     *
     * @param e 追加する entry。
     */
    void PushEntry(const FScoreEntry& e) noexcept;

    /** 現在の累積スコア。 */
    u64 m_CurrentScore = 0;

    /** これまでの最高スコア。 */
    u64 m_HighScore    = 0;

    /** 現在のコンボ数。 */
    u32 m_ComboCount   = 0;

    /** コンボ継続の残り時間 (秒)。 */
    f32 m_ComboTimer   = 0.0f;

    /** コンボのタイムアウト秒 (既定 3 秒)。 */
    f32 m_ComboDuration = 3.0f;

    /** entry log (最大 kMaxEntries 件で capped append)。 */
    TArray<FScoreEntry> m_Entries;

    /** milestone 定義 (m_MilestoneHit と 1:1 並行)。 */
    TArray<u64>  m_Milestones;

    /** milestone ごとの通過済フラグ (m_Milestones と 1:1 並行)。 */
    TArray<bool> m_MilestoneHit;

    /** 差し替え可能な倍率関数 (nullptr = 内部デフォルト)。 */
    MultiplierFn m_MultiplierFn = nullptr;

    /** milestone 通過コールバック (nullptr = 未登録)。 */
    MilestoneCallback m_OnMilestone      = nullptr;

    /** milestone コールバックに渡す user pointer (Manager は所有しない)。 */
    void*             m_OnMilestoneUser = nullptr;
};

} // namespace acs::game
