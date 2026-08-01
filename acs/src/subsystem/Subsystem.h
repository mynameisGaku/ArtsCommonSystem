// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "subsystem/SubsystemFrameContext.h"
#include "subsystem/SubsystemOwner.h"
#include "subsystem/SubsystemScope.h"

namespace acs {

/** owner の寿命に従って生成・更新・破棄される共有サービスの基底。 */
class FSubsystem {
public:
    /** 派生を基底ポインタから破棄する。 */
    virtual ~FSubsystem() noexcept;

    /**
     * owner 割り当て後、初期化 callback より前に責務を検証する。
     * 永続的な変更を行わず、owner を利用できない場合は false を返す。
     */
    virtual bool OnOwnerAssigned() noexcept { return true; }

    /** owner 検証が全件成功した後、決定済み順序で初期化する。 */
    virtual void OnInitialize() noexcept {}

    /** 初期化済み要素を逆順に終了する。 */
    virtual void OnDeinitialize() noexcept {}

    /** 旧 API のスケール済み経過秒を受け取る更新 callback。 */
    virtual void OnTick(f32 /*delta_seconds*/) noexcept {}

    /**
     * 時刻と段階を含む更新 callback。
     * 既定実装は旧 OnTick(f32) へスケール済み経過秒を転送する。
     */
    virtual void OnTickFrame(const FSubsystemFrameContext& context) noexcept;

    /** 派生型固有の同一 link image 内種別 ID を返す。 */
    virtual const void* Kind() const noexcept = 0;

    /** 診断と決定順序に使う型名を返す。 */
    virtual const char* Name() const noexcept { return "FSubsystem"; }

    /** 割り当て済み owner を返す。 */
    void* Owner() const noexcept { return m_Owner.pointer; }

    /** 割り当て済み owner の責務種別を返す。 */
    ESubsystemOwnerKind OwnerKind() const noexcept { return m_Owner.kind; }

    /** 呼び出し側が責務種別を保証した owner を型付きで返す。 */
    template<typename T>
    T* OwnerAs() const noexcept
    {
        return static_cast<T*>(m_Owner.pointer);
    }

    /** owner descriptor を設定するコレクション内部 API。 */
    void _SetOwnerDescriptor(FSubsystemOwner owner) noexcept { m_Owner = owner; }

    /** 種別を持たない旧 owner ポインタを設定する互換 API。 */
    void _SetOwner(void* owner) noexcept
    {
        m_Owner = FSubsystemOwner{owner, ESubsystemOwnerKind::Unknown};
    }

private:
    /** owner の非所有ポインタと責務種別。 */
    FSubsystemOwner m_Owner{};
};

/** RTTI を使わず、同じ link image 内で型ごとに一意な ID を返す。 */
template<typename T>
const void* SubsystemKindOf() noexcept
{
    /** 型 T 専用の process lifetime タグ。 */
    static const int Tag = 0;
    return static_cast<const void*>(&Tag);
}

} // namespace acs

/** FSubsystem 派生へ型 ID と診断名を実装する。 */
#define ACS_SUBSYSTEM_KIND(T)                              \
    const void* Kind() const noexcept override             \
    {                                                       \
        return ::acs::SubsystemKindOf<T>();                 \
    }                                                       \
    const char* Name() const noexcept override { return #T; }

/** GameFramework 旧派生宣言を正規マクロへ転送する。 */
#define ACS_GAME_SUBSYSTEM_KIND(T) ACS_SUBSYSTEM_KIND(T)
