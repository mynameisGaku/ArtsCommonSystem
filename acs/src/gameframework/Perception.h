// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — CPerception (NPC sight / hearing sense)
//
// NPC が複数 target (player / 他の NPC / 音源等) を「視覚」「聴覚」で
// 検知できるかを判定するための最小モジュール。CBehaviorTree の leaf や
// blackboard の値ソースとして使う想定で、本体は単純な幾何判定の集約。
//
// 視覚判定 (sight):
//     distance(eye, target) <= sight_range
//     AND  dot(forward, normalize(target - eye)) >= cos(sight_fov_rad / 2)
//
//   ・前者が距離フィルタ、後者が "視野角内" フィルタ。両方満たした時のみ visible。
//   ・forward は SetEyePos で渡された正規化済み向きを保持。
//   ・target が eye と完全同位置 (距離 0) の場合は方向が定義できないため
//     visible 扱いとする (= 自分の足下に target がいるケースをロストしない)。
//
// 聴覚判定 (hearing):
//     distance(eye, target) <= hearing_range
//
//   ・障害物無視 (壁ごしも聞こえる)。raycast による occlusion は未対応。
//   ・視野角に依存しない 360 度判定。
//
// 設計選択:
//   ・target は `acs::TArray<FPerceptionTarget>` で線形管理。
//     N が小さい想定 (1 NPC あたり数〜数十 target) なので hash map 不要。
//     RemoveAtSwap で順序非保証の高速削除。
//   ・cos(fov/2) は SetConfig 呼び出し時に 1 回だけ計算してキャッシュ。
//     Tick の hot path に Cos を入れない。
//   ・visible / audible は Tick 内でまとめて更新し、結果を FPerceptionTarget に
//     書き戻す。これにより IsTargetVisible / IsTargetAudible は O(N) lookup。
//   ・非コピー・非ムーブ — AScene / Actor のメンバとして固定の場所に置く想定。
//   ・全 noexcept、STL 不使用。
//
// 使い方:
//   acs::game::CPerception perc;
//   acs::game::FSenseConfig cfg;
//   cfg.sight_range   = 10.0f;
//   cfg.sight_fov_rad = acs::kPi * 0.5f;   // 90 度
//   cfg.hearing_range = 5.0f;
//   perc.SetConfig(cfg);
//
//   perc.AddTarget(/*id=*/1, acs::FVec2{ 3.0f, 0.0f });
//   perc.SetEyePos(acs::FVec2{ 0.0f, 0.0f }, acs::FVec2{ 1.0f, 0.0f });   // +X 向き
//   perc.Tick(/*dt=*/0.016f);
//
//   if (perc.IsTargetVisible(1)) { /* 視認した */ }
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * NPC の感覚パラメータ (視覚・聴覚の到達範囲と視野角)。
 *
 * @details 視覚判定は距離フィルタ (sight_range) と視野角フィルタ (cos(fov/2) との dot 比較)
 *          の両方、聴覚判定は距離フィルタ (hearing_range) のみで行う。
 */
struct FSenseConfig {
    /** 視認距離の上限 (m, world unit)。0 以下なら誰も見えない。 */
    f32 sight_range   = 0.0f;

    /** 視野角 (radian, 全幅)。例: 90 度なら kPi/2 を渡す。判定は cos(fov/2) との dot 比較。 */
    f32 sight_fov_rad = 0.0f;

    /** 聴取距離の上限。0 以下なら何も聞こえない。 */
    f32 hearing_range = 0.0f;
};

/**
 * 知覚対象 1 件。
 *
 * @details pos / id は外部から与え、is_visible / is_audible は Tick が更新する出力スロット。
 */
struct FPerceptionTarget {
    /** target のワールド座標。 */
    FVec2 pos        = FVec2::Zero();

    /** target を識別する ID。 */
    u32  id         = 0;

    /** 前回 Tick 時点で視認可能だったか (Tick が更新)。 */
    bool is_visible = false;

    /** 前回 Tick 時点で聴取可能だったか (Tick が更新)。 */
    bool is_audible = false;
};

/**
 * NPC 1 体ぶんの視覚 / 聴覚知覚を管理するモジュール。
 *
 * @details
 * eye 位置 / forward / config と複数 target を保持し、Tick で全 target の visible/audible
 * フラグを再計算する。CBehaviorTree の leaf や blackboard の値ソースとして使う想定。
 * target は線形管理 (N が小さい想定)、cos(fov/2) は SetConfig でキャッシュする。AScene/Actor
 * のメンバとして固定位置に置く想定で非コピー・非ムーブ。
 */
class CPerception {
public:
    /** 空状態 (config なし、target なし、forward = +X) で構築する。 */
    CPerception() noexcept = default;

    /** 破棄する。 */
    ~CPerception() noexcept = default;

    /** コピー禁止 (固定位置に置く state holder のため)。 */
    CPerception(const CPerception&)            = delete;

    /** コピー代入も禁止。 */
    CPerception& operator=(const CPerception&) = delete;

    /** ムーブ禁止。 */
    CPerception(CPerception&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPerception& operator=(CPerception&&)      = delete;

    /**
     * 感覚パラメータを差し替える。
     *
     * @details
     * cos(fov/2) をここでキャッシュし、Tick の hot path に Cos を入れない。既存 target の
     * visible/audible フラグは次の Tick まで変化しない (現値保持)。
     * @param cfg 適用する感覚パラメータ。
     */
    void SetConfig(const FSenseConfig& cfg) noexcept;

    /**
     * eye 位置と forward ベクトルを更新する。
     *
     * @details forward が長さ 0 だった場合は (1, 0) にフォールバックする。
     * @param pos 新しい eye のワールド座標。
     * @param forward 視線方向 (正規化済み前提、長さ 0 なら +X に置換)。
     */
    void SetEyePos(FVec2 pos, FVec2 forward) noexcept;

    /**
     * 新規 target を追加する。
     *
     * @details 同じ id が既に存在する場合は pos を更新するだけ (UpdateTarget と同じ振る舞い)。
     * @param id 追加する target の ID。
     * @param pos target のワールド座標。
     */
    void AddTarget(u32 id, FVec2 pos) noexcept;

    /**
     * 指定 id の target を削除する。
     *
     * @details 存在しなければ no-op。順序非保証の高速削除のため順序は保持されない。
     * @param id 削除する target の ID。
     */
    void RemoveTarget(u32 id) noexcept;

    /**
     * 指定 id の target 位置を更新する。
     *
     * @details 存在しなければ no-op (AddTarget せず静かに無視。削除済み target 参照の想定)。
     * @param id 更新する target の ID。
     * @param pos 新しいワールド座標。
     */
    void UpdateTarget(u32 id, FVec2 pos) noexcept;

    /**
     * 指定 id の target が前回 Tick 時点で視認可能だったかを返す。
     *
     * @param id 問い合わせる target の ID。
     * @return 視認可能なら true。存在しなければ false。
     */
    bool IsTargetVisible(u32 id) const noexcept;

    /**
     * 指定 id の target が前回 Tick 時点で聴取可能だったかを返す。
     *
     * @param id 問い合わせる target の ID。
     * @return 聴取可能なら true。存在しなければ false。
     */
    bool IsTargetAudible(u32 id) const noexcept;

    /**
     * 現在登録されている target 数を返す。
     *
     * @return target 件数。
     */
    u32 TargetCount() const noexcept;

    /**
     * 全 target 配列への読み取り専用ポインタを返す。
     *
     * @details 戻り値は内部バッファで、次の Add/Remove/Update 呼び出しで invalidate される。
     * @param out_count target 件数を書き込む出力先。
     * @return target 配列の先頭ポインタ (空のときの扱いは TArray::Data に従う)。
     */
    const FPerceptionTarget* AllTargets(u32& out_count) const noexcept;

    /**
     * 1 フレームぶんの知覚を再計算する。
     *
     * @details
     * 全 target の is_visible / is_audible を現在の eye / forward / config に基づいて更新する。
     * 聴覚は距離のみ、視覚は距離 + 視野角で判定する。距離 0 の target は無条件 visible。
     * @param dt 将来の perception delay 用に予約された経過秒 (本実装では未使用)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 全 target を削除する。
     *
     * @details config / eye 状態は保持する (再利用時の利便性)。
     */
    void ClearAll() noexcept;

private:
    /**
     * id から target の配列インデックスを線形検索する。
     *
     * @param id 探す target の ID。
     * @return 一致する index。見つからなければ m_Targets.Size() (not-found の番兵)。
     */
    usize FindIndexById(u32 id) const noexcept;

    /** 現在の感覚パラメータ。 */
    FSenseConfig m_Cfg            = {};

    /** SetConfig でキャッシュした cos(fov/2) (視野角判定の閾値)。 */
    f32         m_CosHalfFov   = 1.0f;

    /** eye のワールド座標。 */
    FVec2        m_EyePos        = FVec2::Zero();

    /** eye の視線方向 (正規化済み、既定 +X)。 */
    FVec2        m_EyeForward    = FVec2{1.0f, 0.0f};

    /** 知覚対象の配列 (線形管理)。 */
    TArray<FPerceptionTarget> m_Targets;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPerception = CPerception;

} // namespace acs::game
