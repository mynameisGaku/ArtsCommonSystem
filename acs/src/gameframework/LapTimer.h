// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (ジャンルキット: Racing) — FLapTimer
//
// 複数 racer のラップ時間計測 + 順位算出を一元管理する高レベル API。
// チェックポイント順序検証、ベストラップ更新、フィニッシュ判定 (= total_laps
// 到達)、リアルタイム順位ソートをまとめる。タイトル側はトラック側 collider
// から `NotifyCheckpointPassed` / `NotifyLapCompleted` を投げ、UI 側は
// `GetStats` / `PositionOf` / `GetLeader` を読むだけで成立する。
//
// 使い方:
//   FLapTimer lt;
//   lt.Init(/*total_laps=*/3, /*checkpoints_per_lap=*/4);
//   FRacerId player = lt.AddRacer("Player");
//   FRacerId rival  = lt.AddRacer("Rival");
//
//   lt.SetOnLapCallback([](void*, FRacerId id, u32 lap, f32 t, bool pb) noexcept {
//       // HUD でラップタイム表示 / ベスト更新の演出
//   }, nullptr);
//   lt.SetOnFinishCallback([](void*, FRacerId id, f32 total, u32 pos) noexcept {
//       // リザルト画面遷移
//   }, nullptr);
//
//   lt.Start();
//
//   // 毎フレーム:
//   lt.Tick(dt);
//
//   // collider event:
//   lt.NotifyCheckpointPassed(player, checkpoint_index);
//   lt.NotifyLapCompleted(player);
//
//   // UI:
//   u32 pos       = lt.PositionOf(player);   // 1 = leader
//   const auto* s = lt.GetStats(player);
//
// 設計選択:
//   ・**FRacerId は 24bit idx + 8bit gen の packed u32**: FHealthSystem / FNode2D
//     と同パターン。AddRacer → RemoveRacer → AddRacer で slot 再利用しても
//     古い handle は generation 不一致で弾かれる。0 は invalid 予約 (index 0
//     dummy slot)。
//   ・**race state は 3 値マシン**: Stopped / Running / Paused。Start で
//     reset + Running 開始、Pause で時刻凍結、Resume で再開、Stop で
//     確定 (Tick が進まなくなる)。Init / ClearAll で Stopped に戻る。
//   ・**checkpoint 順序検証**: NotifyCheckpointPassed は `expected_checkpoint`
//     と一致した index のみ受理 (= ショートカット防止)。違う index は黙って
//     棄却 (誤発火に強い)。`checkpoints_per_lap` 到達後は次の
//     NotifyLapCompleted を待つ。
//   ・**ラップ完了は順序検証通過後のみ**: フィニッシュライン collider を踏むと
//     `NotifyLapCompleted` が呼ばれるが、checkpoint をすべて踏み切っていない
//     racer は同じく棄却 (= 逆走防止)。
//   ・**ベストラップ判定**: lap_time が現在の best_lap_time_sec より短い、もしくは
//     初回ラップなら is_personal_best=true で FLapRecord に記録 + callback フラグ
//     も true で発火。最終 lap でも同様に判定する。
//   ・**順位計算は要求時に算出**: 内部に sorted index buffer は持たず、
//     `PositionOf` / `GetLeader` 呼び出しの度に全 active racer を線形比較。
//     racer 数は最大でも 32 想定なので O(N) で十分 (N=32 で命令数 < 1000)。
//   ・**最終順位の確定はゴール到達順**: total_laps を満たしたタイミングで
//     `racer_position` を 1 から順に確定。後続の Tick / NotifyCheckpointPassed
//     はこの racer に対して no-op になり、`stats->racer_position` は固定値を
//     保持する (= フィニッシュ後にゴール待機中の racer に順位が抜かれない)。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Pillar Manager 系と統一。
//
// 範囲外 (将来 Phase で):
//   ・周回別 split (各 checkpoint 通過時刻の累積差分) — 現状は 1 lap 単位の time
//     だけ記録する。UI 上で詳細 split を出したくなったら `FLapRecord` に
//     TArray<f32> sector_times を追加する。
//   ・rubberband AI / handicap — FDynamicDifficulty 側で別途。
//   ・順位変動の onPositionChanged callback — UI 側で前フレームを保持して
//     差分検出する方が柔軟と判断 (Manager 内蔵しない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 1 ラップ分の結果スナップショット。lap_index は 1-origin (= 周回番号)。
//   lap_time_sec        : このラップ単体の所要秒数 (= ラップ開始から完了まで)
//   split_time_sec      : Start から完了までの累計秒数 (= total_time のスナップ)
//   is_personal_best    : このラップが当該 racer の自己ベスト更新だったか
struct FLapRecord {
    u32  lap_index        = 0;
    f32  lap_time_sec     = 0.0f;
    f32  split_time_sec   = 0.0f;
    bool is_personal_best = false;
};

// レース参加者識別子。FHealthId と同じく 24bit index + 8bit generation。
// 0 は invalid 予約 (index 0 dummy)。
struct FRacerId {
    u32 _packed = 0;   // 0 = invalid。layout: low24=index, high8=generation

    constexpr FRacerId() noexcept = default;
    constexpr FRacerId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index()      const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept { return static_cast<u8>(_packed >> 24); }
    bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(FRacerId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(FRacerId o) const noexcept { return _packed != o._packed; }
};

// レース中・終了時に外部から読まれる racer 統計。GetStats() で const 参照。
//   display_name        : AddRacer で渡された文字列 (所有しない / lifetime は呼出側)
//   current_lap         : 0 = まだフィニッシュラインを踏んでいない、N = N 周完了
//   checkpoints_passed  : 現在のラップ内で踏んだ checkpoint 数 (0 〜 checkpoints_per_lap)
//   total_time_sec      : Start からの累計秒数 (Pause 中は進まない)
//   best_lap_time_sec   : 自己ベストラップ秒。未確定なら 0.0f
//   racer_position      : 1-origin 順位。未確定 (= 1 度も計算されてない) なら 0。
//                         フィニッシュ確定後はその時の最終順位で固定。
struct FRacerStats {
    const char* display_name       = nullptr;
    u32         current_lap        = 0;
    u32         checkpoints_passed = 0;
    f32         total_time_sec     = 0.0f;
    f32         best_lap_time_sec  = 0.0f;
    u32         racer_position     = 0;
};

// ラップ完了 callback。最終ラップ含む全 lap 完了で発火。
//   id              : 完了した racer
//   lap             : 1-origin の完了ラップ番号
//   lap_time        : このラップ単体の秒数
//   is_personal_best: 自己ベスト更新したか
using LapCallback = void(*)(void* user, FRacerId id, u32 lap, f32 lap_time, bool is_personal_best) noexcept;

// レース完了 (= total_laps 到達) callback。1 racer 1 回だけ発火する。
//   id            : フィニッシュした racer
//   final_time    : Start からの累計秒数
//   final_position: 1-origin の最終順位 (= フィニッシュ順)
using FinishCallback = void(*)(void* user, FRacerId id, f32 final_time, u32 final_position) noexcept;

class FLapTimer {
public:
    FLapTimer()  noexcept = default;
    ~FLapTimer() noexcept = default;

    FLapTimer(const FLapTimer&)            = delete;
    FLapTimer& operator=(const FLapTimer&) = delete;
    FLapTimer(FLapTimer&&)                 = delete;
    FLapTimer& operator=(FLapTimer&&)      = delete;

    // ---- セットアップ ----------------------------------------------------
    // レース定義 (周回数 / 1 周の checkpoint 数) を設定し、racer 一覧は維持
    // したまま race state を Stopped に戻す。total_laps==0 / checkpoints==0
    // は 1 にクランプ。
    void Init(u32 total_laps, u32 checkpoints_per_lap) noexcept;

    // 新規 racer 追加。最大 (24bit) 個まで登録可能。display_name は
    // 呼出側保証の static lifetime を期待 (コピーしない)。
    FRacerId AddRacer(const char* display_name) noexcept;

    // racer を削除。slot は再利用 (generation 進む)。invalid id は no-op。
    void RemoveRacer(FRacerId id) noexcept;

    // ---- レース制御 ------------------------------------------------------
    // レース開始。全 racer の stats / records をリセットして Running 状態に。
    void Start() noexcept;

    // レース終了。Tick の進行を停止 (= 確定)。Pause とは違い、再開不可。
    void Stop() noexcept;

    // 一時停止。Tick が dt を加算しなくなる。Running 中のみ有効。
    void Pause() noexcept;

    // 一時停止からの再開。Paused 中のみ有効。
    void Resume() noexcept;

    // Running 中なら true (Paused / Stopped は false)。
    bool IsRunning() const noexcept;

    // ---- 進捗通知 (collider / event 側から) ------------------------------
    // checkpoint 通過。`expected_checkpoint` と一致した index のみ受理。
    // ショートカット / 重複通知は黙って棄却する (誤発火に強い API)。
    // race が Running でない場合 / racer フィニッシュ後は no-op。
    void NotifyCheckpointPassed(FRacerId id, u32 checkpoint_index) noexcept;

    // フィニッシュライン通過。当該 racer が全 checkpoint を踏んでいる場合に
    // ラップ完了として記録し、LapCallback を発火。total_laps 到達なら
    // FinishCallback も追加で発火し、racer_position を確定する。
    void NotifyLapCompleted(FRacerId id) noexcept;

    // ---- driver ----------------------------------------------------------
    // Running 中なら全 racer の total_time_sec を dt 加算。Pause/Stop 中は no-op。
    void Tick(f32 dt) noexcept;

    // ---- 問い合わせ ------------------------------------------------------
    // Start からの累計秒数 (race clock)。Paused / Stopped でも値は保持される。
    f32 RaceTimeSec() const noexcept;

    // racer 統計の const 参照。invalid id / Remove 済 / gen 不一致なら nullptr。
    const FRacerStats* GetStats(FRacerId id) const noexcept;

    // 現時点での 1 位 racer。0 racer or 全 invalid なら invalid FRacerId。
    FRacerId GetLeader() const noexcept;

    // 1-origin の現在順位。invalid id は 0。フィニッシュ後は確定順位を返す。
    u32 PositionOf(FRacerId id) const noexcept;

    // 登録中 racer 数 (生存問わず active なもの)。
    u32 RacerCount() const noexcept { return _racer_count; }

    // 当該 racer の完了ラップ数 (= FLapRecord 件数)。invalid id は 0。
    u32 LapRecordCount(FRacerId id) const noexcept;

    // n 番目 (0-origin) のラップ記録。範囲外 / invalid id は nullptr。
    const FLapRecord* GetLapRecord(FRacerId id, u32 lap_index) const noexcept;

    // ---- callback --------------------------------------------------------
    // cb == nullptr で登録解除。
    void SetOnLapCallback(LapCallback cb, void* user) noexcept;
    void SetOnFinishCallback(FinishCallback cb, void* user) noexcept;

    // ---- 一括破棄 --------------------------------------------------------
    // 全 racer を破棄 + race state を Stopped に。Init で設定された
    // total_laps / checkpoints_per_lap、各 callback、設定値は保持する。
    void ClearAll() noexcept;

private:
    // race 状態。bit flag ではなく排他的な 3 値。
    enum class FState : u8 {
        Stopped = 0,
        Running = 1,
        Paused  = 2,
    };

    struct FSlot {
        FRacerStats        stats;
        TArray<FLapRecord>  records;
        bool              active                = false;
        bool              finished              = false;  // total_laps 到達確定
        u8                gen                   = 0;
        u32               expected_checkpoint   = 0;      // 次に踏むべき checkpoint
        f32               lap_start_time        = 0.0f;   // 現在ラップ開始時の RaceTime
    };

    // 空き slot を取得 (index 0 は invalid 予約 dummy)。
    u32 AcquireSlot() noexcept;

    // 内部アクセサ: id が有効なら slot 参照を返し、無効なら nullptr。
    FSlot*       FindSlot(FRacerId id) noexcept;
    const FSlot* FindSlot(FRacerId id) const noexcept;

    // 順位比較: lhs が rhs より上位なら true。
    //   1. (_total_laps - current_lap) 昇順 (= 残りラップ少ない方が上)
    //   2. checkpoints_passed 降順 (= 同じラップ数なら checkpoint 多い方が上)
    //   3. total_time_sec 昇順 (= 同じ進捗なら速い方が上)
    // 注: フィニッシュ後の racer は racer_position が固定されているため、
    //     比較から除外して別途処理する (Calculate*).
    bool IsBetterRank(const FRacerStats& lhs, const FRacerStats& rhs) const noexcept;

    TArray<FSlot>    _slots;
    u32            _racer_count          = 0;

    u32            _total_laps           = 1;
    u32            _checkpoints_per_lap  = 1;

    FState          _state                = FState::Stopped;
    f32            _race_time_sec        = 0.0f;
    u32            _finished_count       = 0;   // 既にフィニッシュした racer 数

    LapCallback    _on_lap       = nullptr;
    void*          _on_lap_user  = nullptr;

    FinishCallback _on_finish      = nullptr;
    void*          _on_finish_user = nullptr;
};

} // namespace acs::game
