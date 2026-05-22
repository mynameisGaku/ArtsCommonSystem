// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L Phase 3 — Perception (NPC sight / hearing sense)
//
// NPC が複数 target (player / 他の NPC / 音源等) を「視覚」「聴覚」で
// 検知できるかを判定するための最小モジュール。BehaviorTree の leaf や
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
//   ・障害物無視 (壁ごしも聞こえる)。raycast による occlusion は Phase 2 で導入。
//   ・視野角に依存しない 360 度判定。
//
// 設計選択:
//   ・target は `acs::Array<PerceptionTarget>` で線形管理。
//     N が小さい想定 (1 NPC あたり数〜数十 target) なので hash map 不要。
//     RemoveAtSwap で順序非保証の高速削除。
//   ・cos(fov/2) は SetConfig 呼び出し時に 1 回だけ計算してキャッシュ。
//     Tick の hot path に Cos を入れない。
//   ・visible / audible は Tick 内でまとめて更新し、結果を PerceptionTarget に
//     書き戻す。これにより IsTargetVisible / IsTargetAudible は O(N) lookup。
//   ・非コピー・非ムーブ — Scene / Actor のメンバとして固定の場所に置く想定。
//   ・全 noexcept、STL 不使用。
//
// 使い方:
//   acs::game::Perception perc;
//   acs::game::SenseConfig cfg;
//   cfg.sight_range   = 10.0f;
//   cfg.sight_fov_rad = acs::kPi * 0.5f;   // 90 度
//   cfg.hearing_range = 5.0f;
//   perc.SetConfig(cfg);
//
//   perc.AddTarget(/*id=*/1, acs::Vec2{ 3.0f, 0.0f });
//   perc.SetEyePos(acs::Vec2{ 0.0f, 0.0f }, acs::Vec2{ 1.0f, 0.0f });   // +X 向き
//   perc.Tick(/*dt=*/0.016f);
//
//   if (perc.IsTargetVisible(1)) { /* 視認した */ }
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// NPC の感覚パラメータ。
//   sight_range   : 視認距離の上限 (m, world unit)。0 以下なら誰も見えない。
//   sight_fov_rad : 視野角 (radian, 全幅)。例: 90 度なら kPi/2 を渡す。
//                   実際の判定は cos(fov/2) との dot 比較。
//   hearing_range : 聴取距離の上限。0 以下なら何も聞こえない。
struct SenseConfig {
    f32 sight_range   = 0.0f;
    f32 sight_fov_rad = 0.0f;
    f32 hearing_range = 0.0f;
};

// 知覚対象 1 件。pos / id は外部から与え、is_visible / is_audible は
// Tick が更新する出力スロット。
struct PerceptionTarget {
    Vec2 pos        = Vec2::Zero();
    u32  id         = 0;
    bool is_visible = false;
    bool is_audible = false;
};

// NPC 1 体ぶんの知覚状態。eye 位置 / forward / config と、複数 target を保持。
// Tick で全 target の visible/audible フラグを再計算する。
class Perception {
public:
    Perception() noexcept = default;
    ~Perception() noexcept = default;

    Perception(const Perception&)            = delete;
    Perception& operator=(const Perception&) = delete;
    Perception(Perception&&)                 = delete;
    Perception& operator=(Perception&&)      = delete;

    // 感覚パラメータを差し替える。cos(fov/2) もここでキャッシュする。
    // 既存 target の visible/audible フラグは次の Tick まで変化しない (現値保持)。
    void SetConfig(const SenseConfig& cfg) noexcept;

    // eye 位置と forward ベクトル (正規化済み前提) を更新する。
    // forward が長さ 0 だった場合は (1, 0) にフォールバックする。
    void SetEyePos(Vec2 pos, Vec2 forward) noexcept;

    // 新規 target を追加。同じ id が既に存在する場合は pos を更新するだけ
    // (UpdateTarget と同じ振る舞いで、重複追加を防ぐ)。
    void AddTarget(u32 id, Vec2 pos) noexcept;

    // 指定 id の target を削除。存在しなければ no-op。順序は保持されない。
    void RemoveTarget(u32 id) noexcept;

    // 指定 id の target 位置を更新。存在しなければ no-op
    // (= AddTarget せず静かに無視。AI が削除済み target を参照するケースの想定)。
    void UpdateTarget(u32 id, Vec2 pos) noexcept;

    // 指定 id の target が前回 Tick 時点で visible/audible だったか。
    // 存在しなければ false。
    bool IsTargetVisible(u32 id) const noexcept;
    bool IsTargetAudible(u32 id) const noexcept;

    // 現在登録されている target 数。
    u32 TargetCount() const noexcept;

    // 全 target 配列への読み取り専用ポインタ。out_count に件数を書き込む。
    // 戻り値は内部バッファ — 次の Add/Remove/Update 呼び出しで invalidate される。
    const PerceptionTarget* AllTargets(u32& out_count) const noexcept;

    // 1 フレームぶんの知覚再計算。全 target の is_visible / is_audible を
    // 現在の eye / forward / config に基づいて更新する。
    // dt は将来の「視認までの遅延 (perception delay)」用に予約。本実装では未使用。
    void Tick(f32 dt) noexcept;

    // 全 target を削除 (config / eye 状態は保持)。
    void ClearAll() noexcept;

private:
    // id から target の配列インデックスを線形検索。
    // 見つからなければ _targets.Size() を返す (= "not found" の番兵)。
    usize FindIndexById(u32 id) const noexcept;

    // ---- 設定 ----
    SenseConfig _cfg            = {};
    f32         _cos_half_fov   = 1.0f;   // SetConfig でキャッシュ (= cos(fov/2))

    // ---- eye 状態 ----
    Vec2        _eye_pos        = Vec2::Zero();
    Vec2        _eye_forward    = Vec2{1.0f, 0.0f};   // 既定 +X

    // ---- target ----
    Array<PerceptionTarget> _targets;
};

} // namespace acs::game
