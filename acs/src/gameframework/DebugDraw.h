// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugDraw (immediate-mode デバッグ図形バッファ)
//
// 1 フレーム分の線分プリミティブを蓄積するだけのバッファ。実描画は行わない。
//
// 設計選択:
//   ・**描画と分離**: FDebugDraw 自体はジオメトリ蓄積のみ。レンダラ非依存。
//     描画システムが Lines() / LineCount() を読み取って FSpriteBatch 等で
//     1 フレーム末にまとめて描画する想定。Pillar H の他の描画系（FSpriteBatch /
//     Particles / 自前 DX12）どれでも消費できる。
//   ・**immediate-mode API**: DrawLine / DrawAabb / DrawCircle / DrawCross を
//     ゲームロジックの任意の場所から呼べる。フレーム頭で Clear() を呼ぶだけ。
//     state を持たず、衝突判定や物理デバッグから手軽に書ける。
//   ・**テストしやすい**: 描画副作用がないので、unit test では蓄積された
//     FLine 配列をそのまま検査できる（座標が正しいか・本数が合うか等）。
//   ・**ゼロ寄与パス可**: Clear() しなければ蓄積され続け、Clear() を毎フレーム
//     呼べば「今フレームの形状」だけが残る。描画システム側が消費後に Clear する
//     運用も可能（呼び出し責任は user に委ねる、library 側は意見を持たない）。
//   ・**FCircle は segment 化**: 内部で線分列に分解して保持。描画側はライン
//     ジオメトリだけ扱えば OK（円専用パスを描画側に要求しない）。
//   ・**非コピー・非ムーブ**: 内部 TArray が大きくなりがちで、誤コピー事故を防ぐ。
//     ゲーム全体で 1 インスタンス（Services 経由か static）を想定。
//
// 使い方:
//   acs::game::FDebugDraw dd;
//   void Frame() {
//       dd.Clear();
//       dd.DrawAabb(player_aabb, FVec4{1,0,0,1});
//       dd.DrawCircle(enemy_circle, FVec4{1,1,0,1});
//       // 描画システム:
//       for (u32 i = 0; i < dd.LineCount(); ++i) {
//           const auto& ln = dd.Lines()[i];
//           sprite_batch.DrawLine(ln.a, ln.b, ln.color);
//       }
//   }
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Collision2D.h"
#include "math/Vec.h"

namespace acs::game {

class FDebugDraw {
public:
    // 描画システムが読み込む生バッファ要素。
    // ライン 1 本 = 2 端点 + 色。
    struct FLine {
        FVec2 a;
        FVec2 b;
        FVec4 color;
    };

    FDebugDraw() noexcept = default;
    ~FDebugDraw() noexcept = default;

    // 非コピー・非ムーブ（内部 TArray の誤コピー / 所有移譲事故を防ぐ）
    FDebugDraw(const FDebugDraw&)            = delete;
    FDebugDraw& operator=(const FDebugDraw&) = delete;
    FDebugDraw(FDebugDraw&&)                 = delete;
    FDebugDraw& operator=(FDebugDraw&&)      = delete;

    // ---- 図形コマンド -------------------------------------------------------
    // 任意 2 点間の線分。
    void DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept;

    // AABB の輪郭（4 辺）。中身は塗らない。
    void DrawAabb(const FAabb2& a, FVec4 color) noexcept;

    // 円を segment 本の線分に分解した近似輪郭。segments=24 で十分滑らか。
    // segments < 3 のときは三角形扱いに丸める（縮退を防ぐ）。
    void DrawCircle(const FCircle& c, FVec4 color, u32 segments = 24) noexcept;

    // 中心 pos の "+" 記号（横線 + 縦線 各長さ size）。位置可視化に便利。
    void DrawCross(FVec2 pos, f32 size, FVec4 color) noexcept;

    // ---- バッファ管理 -------------------------------------------------------
    // 蓄積をクリア（容量は保持）。フレーム頭か描画消費後に呼ぶ。
    void Clear() noexcept { _lines.Clear(); }

    // 蓄積線数。
    u32 LineCount() const noexcept { return static_cast<u32>(_lines.Size()); }

    // 描画システムが読み取る生バッファ先頭（連続メモリ保証）。
    // 空のとき nullptr を返す可能性があるため、利用側は LineCount() で
    // ガードすること。
    const FLine* Lines() const noexcept {
        return _lines.IsEmpty() ? nullptr : &_lines.begin()[0];
    }

private:
    TArray<FLine> _lines;
};

} // namespace acs::game
