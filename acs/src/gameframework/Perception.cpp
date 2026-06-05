// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FPerception 実装
//
// 視覚判定の数学的要点:
//   ・距離フィルタ:   |target - eye| <= sight_range
//                     LengthSq で比較 (sqrt 不要、両辺二乗のまま比較)。
//   ・視野角フィルタ: dot(forward, normalize(target - eye)) >= cos(fov / 2)
//                     forward は単位ベクトル前提。normalize(target - eye) も
//                     単位ベクトルなので、両辺の dot は cos(角度差) に等しい。
//                     角度差が fov/2 以下 ⇔ cos(角度差) >= cos(fov/2)
//                     (cos は [0, π] で単調減少なので不等号が反転する)。
//
//   distance 0 の特殊ケース: target が eye の位置に重なっている場合、
//   方向ベクトルが未定義になるため normalize は 0 を返す (FVec2 仕様)。
//   この場合は dot = 0 となり fov 90 度未満では false 判定されてしまうが、
//   "足下の敵を見失う" のは違和感が大きいので、距離 0 は無条件 visible と扱う。
//
// 聴覚判定:
//   ・距離フィルタのみ: |target - eye| <= hearing_range
//   ・障害物 (壁) による遮蔽は無視 (raycast 遮蔽は未対応)。
#include "gameframework/Perception.h"
#include "math/Math.h"

namespace acs::game {

/** 感覚設定を更新し、視野角半分の cos をキャッシュする。 */
void FPerception::SetConfig(const SenseConfig& cfg) noexcept {
    m_Cfg = cfg;
    // 視野角の半分 (fov/2) の cos をキャッシュ。Tick の hot path に Cos を入れない。
    // fov が負や 2π を超えるような不正値は Cos の domain に渡しても問題ないので、
    // 異常値検出はせずそのまま委ねる (= ゲーム側のミスは観測可能な挙動で出す)。
    m_CosHalfFov = Cos(m_Cfg.sight_fov_rad * 0.5f);
}

/** 視点位置と前方ベクトルを設定する (長さ 0 の前方は +X に補正)。 */
void FPerception::SetEyePos(FVec2 pos, FVec2 forward) noexcept {
    m_EyePos = pos;
    // 呼び出し側で正規化されている前提だが、長さ 0 のベクトルが渡されると
    // 視覚判定が全て fail する。フォールバックとして +X を入れて挙動を安定させる。
    if (LengthSq(forward) <= 0.0f) {
        m_EyeForward = FVec2{1.0f, 0.0f};
    } else {
        m_EyeForward = forward;
    }
}

/** id からターゲット配列の index を線形検索で返す (無ければ要素数)。 */
usize FPerception::FindIndexById(u32 id) const noexcept {
    // 線形検索。N が小さい想定 (1 NPC あたり数〜数十) なので妥当。
    const usize n = m_Targets.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Targets[i].id == id) return i;
    }
    return n;   // not found
}

/** ターゲットを追加する (同一 id が既存なら位置を更新するだけ)。 */
void FPerception::AddTarget(u32 id, FVec2 pos) noexcept {
    // 同一 id が既存ならば「位置を更新するだけ」のセマンティクス。
    // これにより呼び出し側が "Add 前に Remove が必要か" を判断しなくて済む。
    const usize idx = FindIndexById(id);
    if (idx < m_Targets.Size()) {
        m_Targets[idx].pos = pos;
        return;
    }
    PerceptionTarget t;
    t.pos        = pos;
    t.id         = id;
    t.is_visible = false;   // 初期状態は未検知 (次の Tick で再計算)
    t.is_audible = false;
    m_Targets.PushBack(t);
}

/** 指定 id のターゲットを除去する (存在しなければ no-op)。 */
void FPerception::RemoveTarget(u32 id) noexcept {
    const usize idx = FindIndexById(id);
    if (idx >= m_Targets.Size()) return;   // 存在しない id は静かに no-op
    // 順序非保証の高速削除 (末尾を idx に swap してから縮める)。
    m_Targets.RemoveAtSwap(idx);
}

/** 既存ターゲットの位置を更新する (削除済みなら no-op)。 */
void FPerception::UpdateTarget(u32 id, FVec2 pos) noexcept {
    const usize idx = FindIndexById(id);
    if (idx >= m_Targets.Size()) return;   // 削除済み target への Update は no-op
    m_Targets[idx].pos = pos;
}

/** 指定ターゲットが直近 Tick で可視判定されたかを返す。 */
bool FPerception::IsTargetVisible(u32 id) const noexcept {
    const usize idx = FindIndexById(id);
    if (idx >= m_Targets.Size()) return false;
    return m_Targets[idx].is_visible;
}

/** 指定ターゲットが直近 Tick で可聴判定されたかを返す。 */
bool FPerception::IsTargetAudible(u32 id) const noexcept {
    const usize idx = FindIndexById(id);
    if (idx >= m_Targets.Size()) return false;
    return m_Targets[idx].is_audible;
}

/** 登録中のターゲット数を返す。 */
u32 FPerception::TargetCount() const noexcept {
    return static_cast<u32>(m_Targets.Size());
}

/** 全ターゲット配列の先頭を返し、out_count に個数を入れる。 */
const PerceptionTarget* FPerception::AllTargets(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Targets.Size());
    return m_Targets.Data();
}

/** 全ターゲットの可視・可聴フラグを距離と視野角から再計算する。 */
void FPerception::Tick(f32 /*dt*/) noexcept {
    // 範囲ガード: 範囲が 0 以下なら "感覚なし" として全 target を非検知にする。
    // (range == 0 を「届かない」と扱うため、< ではなく <= で比較する)。
    const bool sight_enabled   = (m_Cfg.sight_range   > 0.0f);
    const bool hearing_enabled = (m_Cfg.hearing_range > 0.0f);

    // sqrt 回避のため距離は二乗値で比較。
    const f32 sight_range_sq   = m_Cfg.sight_range   * m_Cfg.sight_range;
    const f32 hearing_range_sq = m_Cfg.hearing_range * m_Cfg.hearing_range;

    const usize n = m_Targets.Size();
    for (usize i = 0; i < n; ++i) {
        PerceptionTarget& t = m_Targets[i];
        const FVec2 delta    = t.pos - m_EyePos;
        const f32  dist_sq  = LengthSq(delta);

        // 聴覚 (距離のみ、視野角無関係)
        t.is_audible = hearing_enabled && (dist_sq <= hearing_range_sq);

        // 視覚 (距離 + 視野角)
        if (!sight_enabled || dist_sq > sight_range_sq) {
            t.is_visible = false;
            continue;
        }
        // 距離 0 の特殊ケース: 方向が定義できないが、足下の target を
        // 見失うのは違和感が強いので無条件 visible とする。
        if (dist_sq <= 0.0f) {
            t.is_visible = true;
            continue;
        }
        // 視野角判定: dot(forward, normalize(delta)) >= cos(fov/2)
        //   forward は単位ベクトル、normalize(delta) も単位ベクトルなので
        //   両者の dot は cos(角度差) に等しい。
        const FVec2 dir = Normalize(delta);
        const f32  d   = Dot(m_EyeForward, dir);
        t.is_visible   = (d >= m_CosHalfFov);
    }
}

/** ターゲット配列のみクリアする (設定・視点状態は維持)。 */
void FPerception::ClearAll() noexcept {
    // target 配列のみクリア。config / eye 状態は維持 (再利用時の利便性)。
    m_Targets.Clear();
}

} // namespace acs::game
