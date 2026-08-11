// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/AComponent.h"
#include "gameframework/TriggerWorld2D.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * ANode を CTriggerWorld2D に橋渡しし、overlap の enter / exit をコンポーネント
 * 単位で受け取れるようにする AComponent。
 *
 * @details
 * CTriggerWorld2D 自体は world 全体で 1 組のコールバックしか持たないため、本
 * コンポーネントは各 trigger に自分自身を user data として紐付け、world の global
 * コールバックを静的ディスパッチャに差し替えて id→component を逆引きする。これに
 * より player / pickup / hazard などを各ノードのハンドラで個別に処理できる。layer
 * は自分が属するレイヤ、mask は反応したいレイヤで、(other.layer & this.mask != 0)
 * のときだけ自分のハンドラが発火する (幾何 overlap 判定自体は world が全 pair で行う)。
 */
class ATriggerComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ATriggerComponent)

    /** layer / mask の全ビット ON (どのレイヤとも反応する) を表す既定値。 */
    static constexpr u32 kAllLayers = 0xFFFFFFFFu;

    /**
     * 所属ワールドとレイヤ/マスクを指定してコンポーネントを構築する。
     *
     * @param world 所属するトリガワールド (通常 Services().Triggers())。
     * @param layer 自分が属するレイヤ bit (既定 kAllLayers)。
     * @param mask 反応したい相手のレイヤ bitmask (既定 kAllLayers)。
     */
    explicit ATriggerComponent(CTriggerWorld2D& world,
                               u32 layer = kAllLayers,
                               u32 mask  = kAllLayers) noexcept
        : m_World(&world), m_Layer(layer), m_Mask(mask) {}

    /**
     * 形状を円に設定する。
     *
     * @details OnAttach までに呼ぶ。後から変えても次の OnUpdate で反映される。
     * @param radius 円の半径 (world 単位)。
     */
    void SetCircle(f32 radius) noexcept { m_Kind = EKind::Circle; m_Radius = radius; }

    /**
     * 形状を AABB に設定する。
     *
     * @details OnAttach までに呼ぶ。後から変えても次の OnUpdate で反映される。
     * @param half_size 中心からの半径ベクトル (各軸の半幅)。
     */
    void SetAabb(FVec2 half_size) noexcept { m_Kind = EKind::Aabb; m_Half = half_size; }

    /**
     * 自分が属するレイヤ bit を返す。
     *
     * @return レイヤ bit。
     */
    u32 Layer() const noexcept { return m_Layer; }

    /**
     * 反応したい相手のレイヤ bitmask を返す。
     *
     * @return マスク bit。
     */
    u32 Mask()  const noexcept { return m_Mask; }

    /**
     * 自分が属するレイヤ bit を設定する。
     *
     * @param layer 設定するレイヤ bit。
     */
    void SetLayer(u32 layer) noexcept { m_Layer = layer; }

    /**
     * 反応したい相手のレイヤ bitmask を設定する。
     *
     * @param mask 設定するマスク bit。
     */
    void SetMask(u32 mask) noexcept { m_Mask = mask; }

    /**
     * トリガコールバックの関数ポインタ型。
     *
     * @details サブクラスの OnTriggerEnter/OnTriggerExit override と併用可能。
     * @param self イベントを受け取った本コンポーネント。
     * @param other 相手のトリガコンポーネント (world 外なら nullptr)。
     * @param user SetOnEnter/SetOnExit で渡した任意ポインタ。
     */
    using TriggerFn = void(*)(ATriggerComponent& self, ATriggerComponent* other, void* user) noexcept;

    /**
     * overlap 開始時のコールバックを設定する。
     *
     * @param fn enter 時に呼ぶ関数ポインタ。
     * @param user fn に渡す任意ポインタ。
     */
    void SetOnEnter(TriggerFn fn, void* user) noexcept { m_OnEnter = fn; m_OnEnterUser = user; }

    /**
     * overlap 終了時のコールバックを設定する。
     *
     * @param fn exit 時に呼ぶ関数ポインタ。
     * @param user fn に渡す任意ポインタ。
     */
    void SetOnExit (TriggerFn fn, void* user) noexcept { m_OnExit  = fn; m_OnExitUser  = user; }

    /**
     * overlap 開始時に呼ばれるサブクラス用フック。
     *
     * @param other 相手のトリガコンポーネント (world 外なら nullptr)。
     */
    virtual void OnTriggerEnter(ATriggerComponent* /*other*/) noexcept {}

    /**
     * overlap 終了時に呼ばれるサブクラス用フック。
     *
     * @param other 相手のトリガコンポーネント (world 外なら nullptr)。
     */
    virtual void OnTriggerExit (ATriggerComponent* /*other*/) noexcept {}

    /**
     * ノードに attach された直後に呼ばれる (owner 参照を保持する)。
     *
     * @param owner このコンポーネントを所有するノード。
     */
    void OnAttach(ANode& owner) noexcept override;

    /**
     * 毎フレーム呼ばれ、初回の遅延登録と形状の world への同期を行う。
     *
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /** ノードから detach される際に trigger を world から削除する。 */
    void OnDetach() noexcept override;

private:
    /** トリガ形状の種別。 */
    enum class EKind : u8 {
        /** 円形トリガ。 */
        Circle,

        /** AABB トリガ。 */
        Aabb
    };

    /** 初回 OnUpdate で trigger を world に遅延登録する (SetCircle/SetAabb 後を尊重)。 */
    void Register() noexcept;

    /** 現在の transform / 形状を world 側の trigger に反映する。 */
    void SyncShape() noexcept;

    /**
     * mask フィルタを通過した enter を自身とコールバックへ配送する。
     *
     * @param other overlap した相手のトリガコンポーネント。
     */
    void HandleEnter(ATriggerComponent* other) noexcept;

    /**
     * mask フィルタを通過した exit を自身とコールバックへ配送する。
     *
     * @param other overlap が解消した相手のトリガコンポーネント。
     */
    void HandleExit (ATriggerComponent* other) noexcept;

    /**
     * world の global enter コールバックに差し込む静的ディスパッチャ。
     *
     * @param user world ポインタ (id→component 逆引きの起点)。
     * @param self enter した側の trigger id。
     * @param other 相手側の trigger id。
     */
    static void SDispatchEnter(void* user, FTriggerId self, FTriggerId other) noexcept;

    /**
     * world の global exit コールバックに差し込む静的ディスパッチャ。
     *
     * @param user world ポインタ (id→component 逆引きの起点)。
     * @param self exit した側の trigger id。
     * @param other 相手側の trigger id。
     */
    static void SDispatchExit (void* user, FTriggerId self, FTriggerId other) noexcept;

    /** 所属するトリガワールド。 */
    CTriggerWorld2D* m_World = nullptr;

    /** world に登録した自身の trigger id (未登録なら invalid)。 */
    FTriggerId       m_Id;

    /** 現在の形状種別。 */
    EKind            m_Kind   = EKind::Circle;

    /** 円形時の半径 (world 単位)。 */
    f32              m_Radius = 0.5f;

    /** AABB 時の各軸半幅。 */
    FVec2            m_Half{0.5f, 0.5f};

    /** 自分が属するレイヤ bit。 */
    u32              m_Layer  = kAllLayers;

    /** 反応したい相手のレイヤ bitmask。 */
    u32              m_Mask   = kAllLayers;

    /** world への登録済みフラグ (二重登録防止)。 */
    bool             m_Registered = false;

    /** overlap 開始コールバック (関数ポインタ)。 */
    TriggerFn m_OnEnter     = nullptr;

    /** overlap 終了コールバック (関数ポインタ)。 */
    TriggerFn m_OnExit      = nullptr;

    /** m_OnEnter に渡す任意ポインタ。 */
    void*     m_OnEnterUser = nullptr;

    /** m_OnExit に渡す任意ポインタ。 */
    void*     m_OnExitUser  = nullptr;
};

} // namespace acs::game
