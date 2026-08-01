// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (ジャンルキット: Racing) — CLapTimer
//
// 複数 racer のラップ時間計測 + 順位算出を一元管理する高レベル API。
// チェックポイント順序検証、ベストラップ更新、フィニッシュ判定 (= total_laps
// 到達)、リアルタイム順位ソートをまとめる。タイトル側はトラック側 collider
// から `NotifyCheckpointPassed` / `NotifyLapCompleted` を投げ、UI 側は
// `GetStats` / `PositionOf` / `GetLeader` を読むだけで成立する。
//
// 使い方:
//   CLapTimer lt;
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
//   ・**FRacerId は 24bit idx + 8bit gen の packed u32**: CHealthSystem / ANode
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
//   ・rubberband AI / handicap — CDynamicDifficulty 側で別途。
//   ・順位変動の onPositionChanged callback — UI 側で前フレームを保持して
//     差分検出する方が柔軟と判断 (Manager 内蔵しない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 1 ラップ分の結果スナップショット。
 *
 * @details lap_index は 1-origin (= 周回番号)。NotifyLapCompleted でラップ完了時に記録される。
 */
struct FLapRecord {
    /** 周回番号 (1-origin)。 */
    u32  lap_index        = 0;

    /** このラップ単体の所要秒数 (= ラップ開始から完了まで)。 */
    f32  lap_time_sec     = 0.0f;

    /** Start から完了までの累計秒数 (= total_time のスナップ)。 */
    f32  split_time_sec   = 0.0f;

    /** このラップが当該 racer の自己ベスト更新だったか。 */
    bool is_personal_best = false;
};

/**
 * レース参加者識別子。
 *
 * @details
 * FHealthId と同じく 24bit index + 8bit generation を packed u32 で持つ。
 * 0 は invalid 予約 (index 0 dummy)。slot 再利用後も generation 不一致で古い
 * handle を弾ける。
 */
struct FRacerId {
    /** packed handle。0 = invalid。layout: low24=index, high8=generation。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed=0) な FRacerId を構築する。 */
    constexpr FRacerId() noexcept = default;

    /**
     * index と generation から packed handle を構築する。
     *
     * @param index slot インデックス (low 24bit に格納)。
     * @param gen generation 値 (high 8bit に格納)。
     */
    constexpr FRacerId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot インデックスを返す。
     *
     * @return low 24bit の index。
     */
    constexpr u32  Index()      const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation 値を返す。
     *
     * @return high 8bit の generation。
     */
    constexpr u8   Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 等値比較。
     *
     * @param o 比較対象の FRacerId。
     * @return packed 値が一致すれば true。
     */
    constexpr bool operator==(FRacerId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等値比較。
     *
     * @param o 比較対象の FRacerId。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FRacerId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * レース中・終了時に外部から読まれる racer 統計。
 *
 * @details GetStats() で const 参照として取得し、UI 側が表示に使う。
 */
struct FRacerStats {
    /** AddRacer で渡された表示名 (所有しない / lifetime は呼出側)。 */
    const char* display_name       = nullptr;

    /** 完了周回数。0 = まだフィニッシュラインを踏んでいない、N = N 周完了。 */
    u32         current_lap        = 0;

    /** 現在のラップ内で踏んだ checkpoint 数 (0 〜 checkpoints_per_lap)。 */
    u32         checkpoints_passed = 0;

    /** Start からの累計秒数 (Pause 中は進まない)。 */
    f32         total_time_sec     = 0.0f;

    /** 自己ベストラップ秒。未確定なら 0.0f。 */
    f32         best_lap_time_sec  = 0.0f;

    /** 1-origin 順位。未確定 (= 1 度も計算されてない) なら 0。フィニッシュ確定後は最終順位で固定。 */
    u32         racer_position     = 0;
};

/**
 * ラップ完了 callback。
 *
 * @details
 * 最終ラップ含む全 lap 完了で発火する。
 * @param user SetOnLapCallback で渡した user ポインタ。
 * @param id 完了した racer。
 * @param lap 1-origin の完了ラップ番号。
 * @param lap_time このラップ単体の秒数。
 * @param is_personal_best 自己ベスト更新したか。
 */
using LapCallback = void(*)(void* user, FRacerId id, u32 lap, f32 lap_time, bool is_personal_best) noexcept;

/**
 * レース完了 (= total_laps 到達) callback。
 *
 * @details
 * 1 racer につき 1 回だけ発火する。
 * @param user SetOnFinishCallback で渡した user ポインタ。
 * @param id フィニッシュした racer。
 * @param final_time Start からの累計秒数。
 * @param final_position 1-origin の最終順位 (= フィニッシュ順)。
 */
using FinishCallback = void(*)(void* user, FRacerId id, f32 final_time, u32 final_position) noexcept;

/**
 * 複数 racer のラップ時間計測と順位算出を一元管理する高レベル API。
 *
 * @details
 * checkpoint 順序検証、ベストラップ更新、フィニッシュ判定 (= total_laps 到達)、
 * リアルタイム順位ソートをまとめる。トラック側 collider から
 * NotifyCheckpointPassed / NotifyLapCompleted を投げ、UI 側は GetStats /
 * PositionOf / GetLeader を読むだけで成立する。順位は要求時に全 active racer を
 * 線形比較して算出する。全 noexcept、非コピー・非ムーブ。
 */
class CLapTimer {
public:
    /** 空の状態で構築する (total_laps=1, checkpoints_per_lap=1, Stopped)。 */
    CLapTimer()  noexcept = default;

    /** 破棄する。 */
    ~CLapTimer() noexcept = default;

    /** コピー禁止 (race state を 1 箇所にとどめるため)。 */
    CLapTimer(const CLapTimer&)            = delete;

    /** コピー代入も禁止。 */
    CLapTimer& operator=(const CLapTimer&) = delete;

    /** ムーブ禁止。 */
    CLapTimer(CLapTimer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CLapTimer& operator=(CLapTimer&&)      = delete;

    /**
     * レース定義を設定し、racer 一覧は維持したまま race state を Stopped に戻す。
     *
     * @details total_laps==0 / checkpoints_per_lap==0 は 1 にクランプする。
     * @param total_laps 総周回数。
     * @param checkpoints_per_lap 1 周あたりの checkpoint 数。
     */
    void Init(u32 total_laps, u32 checkpoints_per_lap) noexcept;

    /**
     * 新規 racer を追加する。
     *
     * @details 最大 (24bit) 個まで登録可能。display_name はコピーせず非所有で保持する。
     * @param display_name racer の表示名 (呼出側保証の static lifetime を期待)。
     * @return 割り当てられた FRacerId。
     */
    FRacerId AddRacer(const char* display_name) noexcept;

    /**
     * racer を削除する。
     *
     * @details slot は再利用され generation が進む。invalid id は no-op。
     * @param id 削除する racer の FRacerId。
     */
    void RemoveRacer(FRacerId id) noexcept;

    /** レースを開始する (全 racer の stats / records をリセットして Running 状態に)。 */
    void Start() noexcept;

    /** レースを終了する (Tick の進行を停止 = 確定)。Pause と違い再開不可。 */
    void Stop() noexcept;

    /** 一時停止する (Tick が dt を加算しなくなる)。Running 中のみ有効。 */
    void Pause() noexcept;

    /** 一時停止から再開する。Paused 中のみ有効。 */
    void Resume() noexcept;

    /**
     * レースが進行中かを返す。
     *
     * @return Running 中なら true (Paused / Stopped は false)。
     */
    bool IsRunning() const noexcept;

    /**
     * checkpoint 通過を通知する。
     *
     * @details
     * expected_checkpoint と一致した index のみ受理する。ショートカット / 重複通知は
     * 黙って棄却する (誤発火に強い API)。race が Running でない場合 / racer フィニッシュ後は no-op。
     * @param id 通過した racer。
     * @param checkpoint_index 通過した checkpoint の index。
     */
    void NotifyCheckpointPassed(FRacerId id, u32 checkpoint_index) noexcept;

    /**
     * フィニッシュライン通過を通知する。
     *
     * @details
     * 当該 racer が全 checkpoint を踏んでいる場合にラップ完了として記録し LapCallback を
     * 発火する。total_laps 到達なら FinishCallback も追加で発火し racer_position を確定する。
     * @param id フィニッシュラインを踏んだ racer。
     */
    void NotifyLapCompleted(FRacerId id) noexcept;

    /**
     * 毎フレームの時間進行を駆動する。
     *
     * @details Running 中なら全 racer の total_time_sec を dt 加算する。Pause/Stop 中は no-op。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * Start からの累計秒数 (race clock) を返す。
     *
     * @return race clock の秒数 (Paused / Stopped でも値は保持される)。
     */
    f32 RaceTimeSec() const noexcept;

    /**
     * racer 統計の const 参照を返す。
     *
     * @param id 取得する racer。
     * @return racer 統計 (invalid id / Remove 済 / gen 不一致なら nullptr)。
     */
    const FRacerStats* GetStats(FRacerId id) const noexcept;

    /**
     * 現時点での 1 位 racer を返す。
     *
     * @return 1 位の FRacerId (0 racer or 全 invalid なら invalid FRacerId)。
     */
    FRacerId GetLeader() const noexcept;

    /**
     * racer の現在順位を返す。
     *
     * @param id 順位を問い合わせる racer。
     * @return 1-origin の現在順位 (invalid id は 0、フィニッシュ後は確定順位)。
     */
    u32 PositionOf(FRacerId id) const noexcept;

    /**
     * 登録中 racer 数を返す。
     *
     * @return active な racer の数。
     */
    u32 RacerCount() const noexcept { return m_RacerCount; }

    /**
     * racer の完了ラップ数を返す。
     *
     * @param id 問い合わせる racer。
     * @return 完了ラップ数 (= FLapRecord 件数。invalid id は 0)。
     */
    u32 LapRecordCount(FRacerId id) const noexcept;

    /**
     * racer の n 番目のラップ記録を返す。
     *
     * @param id 問い合わせる racer。
     * @param lap_index ラップ記録の index (0-origin)。
     * @return ラップ記録 (範囲外 / invalid id は nullptr)。
     */
    const FLapRecord* GetLapRecord(FRacerId id, u32 lap_index) const noexcept;

    /**
     * ラップ完了 callback を登録する。
     *
     * @param cb 発火する callback (nullptr で登録解除)。
     * @param user callback に渡す user ポインタ。
     */
    void SetOnLapCallback(LapCallback cb, void* user) noexcept;

    /**
     * レース完了 callback を登録する。
     *
     * @param cb 発火する callback (nullptr で登録解除)。
     * @param user callback に渡す user ポインタ。
     */
    void SetOnFinishCallback(FinishCallback cb, void* user) noexcept;

    /**
     * 全 racer を破棄し race state を Stopped に戻す。
     *
     * @details Init で設定された total_laps / checkpoints_per_lap、各 callback、設定値は保持する。
     */
    void ClearAll() noexcept;

private:
    /** race 状態 (bit flag ではなく排他的な 3 値)。 */
    enum class EState : u8 {
        /** 停止中 (Tick 進まず)。Init / Stop / ClearAll で遷移。 */
        Stopped = 0,

        /** 進行中 (Tick が時間を進める)。Start / Resume で遷移。 */
        Running = 1,

        /** 一時停止中 (Tick が時間を進めない)。Pause で遷移。 */
        Paused  = 2,
    };

    /** racer 1 人分の内部スロット (統計・ラップ記録・進捗状態)。 */
    struct FSlot {
        /** 外部公開用の racer 統計。 */
        FRacerStats        stats;

        /** 完了したラップの記録列。 */
        TArray<FLapRecord>  records;

        /** スロットが使用中か (RemoveRacer で false)。 */
        bool              active                = false;

        /** total_laps 到達でフィニッシュ確定済みか。 */
        bool              finished              = false;

        /** slot の generation (FRacerId 検証用)。 */
        u8                gen                   = 0;

        /** 次に踏むべき checkpoint の index。 */
        u32               expected_checkpoint   = 0;

        /** 現在ラップ開始時の RaceTime (ラップ時間算出の基点)。 */
        f32               lap_start_time        = 0.0f;
    };

    /**
     * 空き slot を取得する。
     *
     * @details index 0 は invalid 予約 dummy として使わない。
     * @return 確保した slot の index。
     */
    u32 AcquireSlot() noexcept;

    /**
     * id に対応する slot の可変参照を返す。
     *
     * @param id 検索する FRacerId。
     * @return 有効なら slot ポインタ、無効 / gen 不一致なら nullptr。
     */
    FSlot*       FindSlot(FRacerId id) noexcept;

    /**
     * id に対応する slot の const 参照を返す。
     *
     * @param id 検索する FRacerId。
     * @return 有効なら slot ポインタ、無効 / gen 不一致なら nullptr。
     */
    const FSlot* FindSlot(FRacerId id) const noexcept;

    /**
     * 2 racer の順位優劣を比較する。
     *
     * @details
     * 優先順: (1) 残りラップ少ない方が上 (m_TotalLaps - current_lap 昇順)、
     * (2) 同ラップ数なら checkpoint 多い方が上 (checkpoints_passed 降順)、
     * (3) 同進捗なら速い方が上 (total_time_sec 昇順)。フィニッシュ後の racer は
     * racer_position が固定されているため比較から除外して別途処理する。
     * @param lhs 比較元の racer 統計。
     * @param rhs 比較先の racer 統計。
     * @return lhs が rhs より上位なら true。
     */
    bool IsBetterRank(const FRacerStats& lhs, const FRacerStats& rhs) const noexcept;

    /** racer slot 配列 (index 0 は invalid 予約 dummy)。 */
    TArray<FSlot>    m_Slots;

    /** 登録中 racer 数。 */
    u32            m_RacerCount          = 0;

    /** 総周回数 (最小 1)。 */
    u32            m_TotalLaps           = 1;

    /** 1 周あたりの checkpoint 数 (最小 1)。 */
    u32            m_CheckpointsPerLap  = 1;

    /** 現在の race 状態。 */
    EState          _state                = EState::Stopped;

    /** race clock の累計秒数。 */
    f32            m_RaceTimeSec        = 0.0f;

    /** 既にフィニッシュした racer 数 (最終順位の確定に使う)。 */
    u32            m_FinishedCount       = 0;

    /** ラップ完了 callback (nullptr で無効)。 */
    LapCallback    m_OnLap       = nullptr;

    /** ラップ完了 callback に渡す user ポインタ。 */
    void*          m_OnLapUser  = nullptr;

    /** レース完了 callback (nullptr で無効)。 */
    FinishCallback m_OnFinish      = nullptr;

    /** レース完了 callback に渡す user ポインタ。 */
    void*          m_OnFinishUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLapTimer = CLapTimer;

} // namespace acs::game
