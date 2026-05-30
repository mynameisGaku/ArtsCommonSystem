// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FProgression (XP / Level / Milestones / Unlocks)
//
// 「累計 XP を加算するとレベルが上がり、所定の XP 閾値を越えると Milestone を
// 達成 → コンテンツが Unlock される」というジャンルを問わず広く使われる進行
// システムを 1 クラスにまとめた小型マネージャ。
//
// 想定する位置付け:
//   ・Pillar S (FAchievementManager) との違い:
//     - Achievement は「最終的に解除/未解除の 2 値フラグ」を管理する SDK 連動装置。
//       Storefront 配信プラットフォームへ片方向送信する責務まで含む。
//     - FProgression は「累積 XP に対してレベルとマイルストーンが線形に増える」
//       純粋にゲーム内で完結する進行カウンタ。プラットフォーム SDK には依存しない。
//   ・Unlock 連携:
//     - 各 MilestoneDef に `unlock_content_id` を持たせる。FProgression 自身は
//       コンテンツ解放の実体を持たず、達成時に FCallback でゲーム側へ通知する。
//       受け取った側が EntitlementRegistry / ContentManager / FAchievementManager
//       に橋渡しする責務。
//
// 使い方:
//   FProgression p;
//
//   // 起動時にゲーム側でマイルストーンを 1 度ずつ登録。
//   p.RegisterMilestone({ "ms.level_5",  "Lv.5 到達",   31,   "content.weapon_b" });
//   p.RegisterMilestone({ "ms.level_10", "Lv.10 到達",  1023, "content.area_2"   });
//   p.RegisterMilestone({ "ms.veteran",  "Veteran",     16383,"content.title_x"  });
//
//   // (任意) 達成時 callback を登録。ゲーム側で SE 再生・トースト UI 等。
//   p.SetOnAchievedCallback(&OnMilestoneAchieved, /*user*/ &game);
//
//   // ゲームプレイ中の XP 加算 (敵撃破・クエスト完了・収集物等で呼ぶ)。
//   p.AwardXp(50);
//   p.AwardXp(20);
//
//   // UI 表示。
//   u32 lv  = p.CurrentLevel();
//   u32 xp  = p.CurrentXp();
//   u32 cur = p.AchievedCount();
//   u32 tot = p.MilestoneCount();
//
// 設計選択 (v7 Phase 1):
//   ・**累計 XP は u32**:
//     - 64bit にする必要があるほど一回のセッションで XP を稼ぐタイトルは稀。
//       u32 max = 約 42 億で実用上の上限を遥かに超える。オーバーフロー時は
//       max にクランプする (足し算サイレントラップを避ける)。
//   ・**レベル計算は floor(log2(xp + 1))**:
//     - 単純な log2 ベース計算なので、初期は急成長 → 中盤以降は緩やかに、という
//       典型的な RPG 風カーブを近似できる。+1 補正で xp=0 のとき level=0 になる。
//     - 浮動小数 log2 を使わず、整数ビット走査で算出 (.cpp 側のループ実装で
//       プラットフォーム非依存)。
//     - XP <= 0 → Level 0、XP=1 → Level 1、XP=3 → 2、XP=7 → 3、XP=15 → 4、…
//     - 必要なら将来 `SetLevelCurve(callback)` で差し替え可能だが、今は YAGNI。
//   ・**MilestoneDef / MilestoneState を別配列で持つ**:
//     - FAchievementManager と同じ pattern。Def は immutable な定義 (id /
//       display_name / required_xp / unlock_content_id)、State は実行時の
//       達成状態 (id / achieved / achieved_timestamp)。1:1 対応で同 index を共有。
//   ・**所有しない const char***:
//     - id / display_name / unlock_content_id は呼出側 (ゲームコード or リソース
//       バンドル) が保証する static lifetime の文字列リテラルを想定。
//       FProgression 側ではコピーしない (STL <string> 禁止方針)。
//   ・**重複登録は黙って弾く**:
//     - 同 id を 2 度 RegisterMilestone しても 2 回目は no-op。
//       他 Manager 系 (FEntitlement / Achievement) と同じ防御方針。
//   ・**線形検索**:
//     - Milestone 件数は 1 タイトルで通常 10〜100 程度。TArray<T> の per-byte
//       文字列比較 + 線形走査で十分。
//   ・**FCallback は関数ポインタ + user data**:
//     - FTriggerWorld2D / FSceneTimer と同じ pattern。`std::function` は STL 禁止
//       方針で使えないので、`void(*)(void*,...)` で固定。1 種類のみ (達成時)
//       なので命名は `MilestoneCallback`。
//   ・**Save / Load は Phase 1 では TODO スタブ**:
//     - FSaveSlot / FSettings と同じく、形だけ TResult<void> を返す。実 I/O は
//       Phase 2 で FileSystem / FSaveSlot 経由で実装する。
//   ・**全 noexcept、非コピー・非ムーブ**:
//     - 他 Manager 系と統一。誤って値渡しされて進捗が分裂すると検知し辛い。
//   ・**STL 不使用、`<string>` 禁止**:
//     - ACS 全体方針。文字列は const char* 非所有のみ。
//
// 範囲外 (Phase 2+ で):
//   ・XP 倍率・経験値テーブル差し替え (今はハードコード log2 ベース)
//   ・SDK 統合 (Steamworks 等は FAchievementManager 側で扱う)
//   ・永続化の実装 (Phase 2 で FSaveSlot 経由に接続)
//   ・seasonal reset / prestige system (Phase 3+ で別 API として追加検討)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"

namespace acs::game {

// ---- MilestoneDef: 起動時に 1 回登録される immutable な定義 ----------------
// id           : 検索 / 永続化のキー。文字列リテラル想定 (非所有)。
// display_name : UI 表示用 (リスト・トースト等)。非所有。
// required_xp  : 累計 XP がこの値以上になった瞬間に達成扱い。
//                AwardXp 時に内部で線形走査して判定する。
// unlock_content_id:
//                達成時に「何がアンロックされるか」を表す ID。FProgression 自身は
//                解放処理を行わず、FCallback でゲーム側に通知するだけ。nullptr は
//                許容 (= 純粋に演出だけのマイルストーン)。
struct MilestoneDef {
    const char* id                = nullptr;
    const char* display_name      = nullptr;
    u32         required_xp       = 0;
    const char* unlock_content_id = nullptr;
};

// ---- MilestoneState: 実行時に変化する 1 マイルストーンの状態 ----------------
// MilestoneDef と同 index 位置で 1:1 対応する。id は Def 側のものを参照
// コピー (= 同じリテラルを指す) して、検索 / 表示で扱いやすくする。
// achieved_timestamp は Clock::MillisSinceStartup() の値 (起動からの ms)。
// 0 は「未達成」 or 「達成時に Clock 未取得」を表す。
struct MilestoneState {
    const char* id                 = nullptr;
    bool        achieved           = false;
    u64         achieved_timestamp = 0;  // Clock::MillisSinceStartup() at achievement
};

// ---- MilestoneCallback ----------------------------------------------------
// `void(*)(void* user, const char* milestone_id) noexcept`
// 達成した瞬間に 1 回だけ呼ばれる。user は SetOnAchievedCallback で渡した値。
// milestone_id は登録時の id (リテラル) がそのまま渡る。
using MilestoneCallback = void(*)(void* user, const char* milestone_id) noexcept;

// ---- FProgression ---------------------------------------------------------
class FProgression {
public:
    FProgression()  noexcept = default;
    ~FProgression() noexcept = default;

    FProgression(const FProgression&)            = delete;
    FProgression& operator=(const FProgression&) = delete;
    FProgression(FProgression&&)                 = delete;
    FProgression& operator=(FProgression&&)      = delete;

    // ---- 定義登録 (起動時に 1 度ずつ) -----------------------------------
    // 同 id の 2 重登録は no-op、`def.id == nullptr` も no-op (defensive)。
    void RegisterMilestone(const MilestoneDef& def) noexcept;

    // ---- XP 操作 --------------------------------------------------------
    // 累計 XP に amount を加算し、各 milestone の達成判定を行う。
    // 加算結果が u32 を超える場合は max にクランプ (オーバーフロー防御)。
    // amount = 0 は no-op (callback も発火しない)。
    void AwardXp(u32 amount) noexcept;

    // ---- 照会 -----------------------------------------------------------
    u32 CurrentXp()    const noexcept;
    // floor(log2(xp + 1))。xp = 0 → 0、xp = 1 → 1、xp = 3 → 2、xp = 7 → 3、…
    u32 CurrentLevel() const noexcept;

    // 指定 id が達成済みか。`id == nullptr` / 未登録 id は false。
    bool IsMilestoneAchieved(const char* id) const noexcept;

    // 登録総数 / 達成数。UI の "5/20 unlocked" 表示用。
    u32 MilestoneCount() const noexcept;
    u32 AchievedCount()  const noexcept;

    // 単一状態取得。見つからなければ nullptr。返却ポインタは次の
    // RegisterMilestone() / ResetProgress() で無効化される可能性がある。
    const MilestoneState* GetState(const char* id) const noexcept;

    // 全状態の生バッファ。`out_count` に件数を書き出して返す。
    // 返却ポインタは MilestoneCount() 件の連続バッファ、RegisterMilestone()
    // / ResetProgress() で無効化される。
    const MilestoneState* AllStates(u32& out_count) const noexcept;

    // ---- リセット (debug / NewGame+ 系) ---------------------------------
    // 累計 XP = 0、全 milestone を未達成に戻す。定義 (MilestoneDef 配列) は保持。
    // 出荷ビルドでは UI から呼ばないこと (デバッグメニュー / NewGame+ 想定)。
    void ResetProgress() noexcept;

    // ---- FCallback -------------------------------------------------------
    // 達成時 callback を登録 / 差し替え / 解除 (cb = nullptr で解除)。
    // 設定された callback は AwardXp 内で「未達成 → 達成」遷移を起こした
    // milestone ごとに 1 回呼ばれる。
    void SetOnAchievedCallback(MilestoneCallback cb, void* user) noexcept;

    // ---- 永続化 (FSaveArchive で実装済み、round-trip 検証済み) -----------
    // Save: 累計 XP + 達成済み milestone を FSaveArchive バイナリ (CRC32 付き) で
    //   file_path に書く。milestone は id の FNV-1a hash をキーにするので登録順に非依存。
    // Load: 同じ MilestoneDef 群を RegisterMilestone した後に呼ぶと、XP と各 milestone の
    //   達成状態を復元する (hash 一致でマッチング、未登録 hash は無視)。
    TResult<void> Save(const wchar_t* file_path) noexcept;
    TResult<void> Load(const wchar_t* file_path) noexcept;

private:
    // id 文字列を m_Defs から線形検索。未検出は -1。`id == nullptr` も -1。
    isize FindIndex(const char* id) const noexcept;

    // 累計 XP。AwardXp で加算され、ResetProgress で 0 に戻る。
    u32 m_Xp = 0;

    // Def / State は 1:1 対応 (同 index)。
    TArray<MilestoneDef>   m_Defs;
    TArray<MilestoneState> _states;

    // 達成時 callback (関数ポインタ + user data)。両方 nullptr で「未設定」。
    MilestoneCallback m_OnAchieved      = nullptr;
    void*             m_OnAchievedUser = nullptr;
};

} // namespace acs::game
