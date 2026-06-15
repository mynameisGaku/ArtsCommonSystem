// SPDX-License-Identifier: Apache-2.0
// GameFramework — FPrimitiveRenderer2D
//
// プリミティブ形状 (箱 / 円 / 三角) を描画する FComponent2D。shape で形状を選ぶ。
// エディタはこの shape を読んで viewport 描画 + コライダー形状を合わせる
// (= 選んだ形状で見た目もアタリも決まる)。ゲーム実行時はこの OnDraw が
// FSpriteBatch に積む。色/サイズはコンポーネント側に持つ (owner の scale で拡縮)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"           // ACS_LOG_INFO (LogState のデモ出力)
#include "math/Vec.h"
#include "gameframework/AcsClass.h"    // ACS_FUNCTION マーカー
#include "gameframework/Component2D.h"
#include "gameframework/Node2D.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"

#include <cmath>   // std::cos / std::sin

namespace acs::game {

/**
 * プリミティブ形状を描く FComponent2D。
 *
 * @details shape (Box / Circle / Triangle) を owner の world transform で配置・回転して
 * FSpriteBatch へ積む。円・三角は三角形ファンで塗る。エディタは shape を読んで描画と
 * コライダー形状を一致させる。
 */
class FPrimitiveRenderer2D : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FPrimitiveRenderer2D)

    /** 形状種別。 */
    enum class EShape : u8 { Box = 0, Circle = 1, Triangle = 2 };

    FPrimitiveRenderer2D() noexcept = default;

    /** 形状を設定する。 */
    void   SetShape(EShape s) noexcept { m_Shape = s; }
    /** 現在の形状。 */
    EShape Shape() const noexcept { return m_Shape; }
    /** 塗り色を設定する。 */
    void   SetColor(FVec4 c) noexcept { m_Color = c; }
    /** 塗り色。 */
    FVec4  Color() const noexcept { return m_Color; }
    /** ローカルサイズ (owner scale で拡縮) を設定する。 */
    void   SetSize(FVec2 s) noexcept { m_Size = s; }
    /** ローカルサイズ。 */
    FVec2  Size() const noexcept { return m_Size; }

    /** 影オクルーダーを形状に合わせるため、shape + ローカル半サイズを返す。 */
    bool QueryPrimitive(i32& shape, FVec2& half_size) const noexcept override {
        shape = static_cast<i32>(m_Shape);
        half_size = FVec2{ m_Size.x * 0.5f, m_Size.y * 0.5f };
        return true;
    }

    /** 現在の形状/サイズ/色をログ出力する(BlueprintCallable + CallInEditor のデモ関数)。
     *  Blueprint グラフの «関数» ノード→実ノード作用の実例。 */
    ACS_FUNCTION(BlueprintCallable, CallInEditor)
    void LogState() noexcept {
        ACS_LOG_INFO("[Prim] LogState shape=%d size=(%.2f,%.2f) color=(%.2f,%.2f,%.2f,%.2f)",
            static_cast<int>(m_Shape), static_cast<double>(m_Size.x), static_cast<double>(m_Size.y),
            static_cast<double>(m_Color.x), static_cast<double>(m_Color.y),
            static_cast<double>(m_Color.z), static_cast<double>(m_Color.w));
    }

    /** owner の world transform で形状を FSpriteBatch へ積む。 */
    void OnDraw(RenderContext& rc) noexcept override {
        if (!rc.HasSprites()) return;
        const FTransform2D wt = Owner().World();
        const f32 w = m_Size.x * wt.scale.x;
        const f32 h = m_Size.y * wt.scale.y;
        DrawShape(rc.Sprites(), static_cast<int>(m_Shape),
                  wt.position.x, wt.position.y, w, h, wt.rotation, m_Color);
    }

    /**
     * プリミティブ形状を FSpriteBatch に塗る (エディタ viewport と共用のヘルパ)。
     *
     * @param sb 積み先の SpriteBatch。
     * @param shape 0=Box, 1=Circle, 2=Triangle。
     * @param cx 中心 X。 @param cy 中心 Y。 @param w 幅。 @param h 高さ。
     * @param rot 回転 (rad)。 @param col 塗り色。
     */
    static void DrawShape(FSpriteBatch& sb, int shape, f32 cx, f32 cy,
                          f32 w, f32 h, f32 rot, FVec4 col) noexcept {
        const f32 c = std::cos(rot), s = std::sin(rot);
        if (shape == 1) {                                   // 円/楕円 (三角ファン)
            constexpr int N = 28;
            const f32 rx = w * 0.5f, ry = h * 0.5f;
            f32 px = 0.0f, py = 0.0f;
            for (int i = 0; i <= N; ++i) {
                const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(N);
                const f32 lx = rx * std::cos(a), ly = ry * std::sin(a);
                const f32 vx = cx + lx * c - ly * s;
                const f32 vy = cy + lx * s + ly * c;
                if (i > 0) sb.DrawTriangle(cx, cy, px, py, vx, vy, col);
                px = vx; py = vy;
            }
        } else if (shape == 2) {                            // 三角形 (上頂点)
            const f32 hx = w * 0.5f, hy = h * 0.5f;
            const f32 ax = cx + (0.0f) * c - (-hy) * s, ay = cy + (0.0f) * s + (-hy) * c;
            const f32 bx = cx + (-hx) * c - (hy) * s,   by = cy + (-hx) * s + (hy) * c;
            const f32 dx = cx + (hx) * c - (hy) * s,    dy = cy + (hx) * s + (hy) * c;
            sb.DrawTriangle(ax, ay, bx, by, dx, dy, col);
        } else {                                            // 箱
            sb.DrawRectRotated(cx, cy, w, h, rot, col);
        }
    }

private:
    EShape m_Shape = EShape::Box;
    FVec4  m_Color{ 0.6f, 0.7f, 0.9f, 1.0f };
    FVec2  m_Size { 1.0f, 1.0f };
};

} // namespace acs::game
