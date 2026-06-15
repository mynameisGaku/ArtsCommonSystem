// SPDX-License-Identifier: Apache-2.0
// GameFramework — FSubsystem (UE 風サブシステム基底)
//
// 「特定スコープ(寿命)に 1 つだけ存在し、そのスコープ内のオブジェクトから
// 取得して使える管理オブジェクト」の基底。シングルトン/サービスの置き場であり、
// ワールドに居るオブジェクト同士のやり取り(スコア・イベントバス・スポーン管理 等)
// のハブになる。スコープは Engine / GameInstance / World の 3 種(将来拡張可)。
//
// 使い方(利用者):
//   ACS_CLASS()
//   class FScoreSubsystem : public acs::game::FSubsystem {
//   public:
//       ACS_GAME_SUBSYSTEM_KIND(FScoreSubsystem)
//       void OnInitialize() noexcept override { m_Score = 0; }
//       void Add(int n) noexcept { m_Score += n; }
//       int  Score() const noexcept { return m_Score; }
//   private:
//       int m_Score = 0;
//   };
//   ACS_REGISTER_SUBSYSTEM(FScoreSubsystem, acs::game::ESubsystemScope::World)
//
//   // 取得 (Scene / FGame / FNode2D / FComponent2D の GetSubsystem<T>() から):
//   GetSubsystem<FScoreSubsystem>()->Add(10);
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * サブシステムの «スコープ»(生存期間)。下位スコープから上位スコープを参照できる
 * (World → GameInstance → Engine の順にフォールバック検索される)。
 */
enum class ESubsystemScope : u8 {
    /** アプリ全体(プロセス寿命)。最も長寿命の共有サービス。 */
    Engine       = 0,
    /** ゲームセッション(FGame 寿命)。シーンを跨いで保持したいシングルトン。 */
    GameInstance = 1,
    /** ロード中の World/Scene 寿命。シーン固有の管理オブジェクト。 */
    World        = 2,
};

/**
 * スコープ管理オブジェクトの基底。スコープ開始時に 1 つ生成され、終了時に破棄される。
 *
 * @details
 * 種別 ID は RTTI 不使用の `SubsystemKindOf<T>()` 型タグ(コンポーネントと同方式)で識別する。
 * 派生は `ACS_GAME_SUBSYSTEM_KIND(T)` を 1 行入れること。
 */
class FSubsystem {
public:
    /** 仮想デストラクタ(派生を base ポインタで破棄するため)。 */
    virtual ~FSubsystem() noexcept = default;

    /** スコープ開始時(生成直後)に 1 度呼ばれる初期化フック。 */
    virtual void OnInitialize() noexcept {}

    /** スコープ終了時(破棄直前)に 1 度呼ばれる後始末フック。 */
    virtual void OnDeinitialize() noexcept {}

    /**
     * 毎フレーム呼ばれる更新フック(任意)。
     *
     * @param dt 経過秒(World サブシステムはシーンと同じスケール済み dt、
     *           GameInstance/Engine は生 dt)。
     */
    virtual void OnTick(f32 /*dt*/) noexcept {}

    /**
     * この型固有の種別 ID を返す(派生は ACS_GAME_SUBSYSTEM_KIND で実装)。
     *
     * @return SubsystemKindOf<派生型>() の安定ポインタ。
     */
    virtual const void* Kind() const noexcept = 0;

    /** 型名(デバッグ/ログ用)。 */
    virtual const char* Name() const noexcept { return "FSubsystem"; }

    /**
     * 所有コンテキスト(UE の Outer 相当)を返す。World サブシステムは所属 Scene、
     * GameInstance/Engine サブシステムは FGame が設定される(非所有・生ポインタ)。
     *
     * @return 所有コンテキスト(未設定なら nullptr)。OnInitialize 時点で有効。
     */
    void* Owner() const noexcept { return m_Owner; }

    /**
     * 所有コンテキストを型付きで取り出す(例: World サブシステムから OwnerAs<FScene2D>())。
     *
     * @tparam T キャスト先の型(呼び出し側が正しいスコープ型を保証する)。
     * @return T*(未設定なら nullptr)。
     */
    template<typename T>
    T* OwnerAs() const noexcept { return static_cast<T*>(m_Owner); }

    /** 所有コンテキストを設定する(内部用。コレクションが OnInitialize 前に呼ぶ)。 */
    void _SetOwner(void* owner) noexcept { m_Owner = owner; }

private:
    void* m_Owner = nullptr;   ///< 所有コンテキスト(Scene / FGame、非所有)。
};

/**
 * 型 T 固有の安定したポインタ ID を返す(RTTI 不使用の種別タグ)。
 *
 * @tparam T 種別 ID を取りたい FSubsystem 派生型。
 * @return T に固有の安定したポインタ。
 */
template<typename T>
const void* SubsystemKindOf() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

} // namespace acs::game

/** FSubsystem 派生に種別 ID と型名を実装するマクロ(クラス本体に 1 行)。 */
#define ACS_GAME_SUBSYSTEM_KIND(T)                                                   \
    const void* Kind() const noexcept override                                       \
    { return ::acs::game::SubsystemKindOf<T>(); }                                     \
    const char* Name() const noexcept override { return #T; }
